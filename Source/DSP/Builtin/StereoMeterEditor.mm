#include "DSP/Builtin/StereoMeterEditor.h"

#include "DSP/Builtin/StereoFreqAnalyzer.h"
#include "DSP/Builtin/StereoMeterMath.h"
#include "DSP/Builtin/StereoMeterProcessor.h"

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_extra/juce_gui_extra.h> // juce::NSViewComponent

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <simd/simd.h>

#include <algorithm>
#include <cmath>
#include <vector>

// ============================================================================
// Stereo 3D Pan Scatter — Metal point-cloud renderer hosted in JUCE.
//   X = stereo pan (-1 L … +1 R) · Y = log frequency · Z = intensity
//   colour = phase coherence (red anti-phase ↔ white ↔ green in-phase)
// Phase B: point cloud + time-decayed trails + drop-line stems + room
// wireframe + orbit camera. Audio arrives via the processor's lock-free L/R
// rings; a 60 Hz timer (UI thread) drains them, runs the FFT/analysis, and
// pushes points + stems to the GPU.
// ============================================================================

namespace
{
struct PointVertex
{
    float pos[3];   // matches MSL packed_float3 (12 bytes)
    float color[4]; // packed_float4 (16 bytes)
    float size;
};
struct LineVertex
{
    float pos[3];
    float color[4];
};
struct TexVertex
{
    float pos[3]; // packed_float3 (12 bytes)
    float uv[2];  // packed_float2 (8 bytes)
};
constexpr float kFreqAxisX = -1.0f;  // labels/ticks sit on the left edge
constexpr float kLabelHalfH = 0.05f; // billboard half-height, world units
constexpr int kMaxLabels = 16;
struct Uniforms
{
    matrix_float4x4 mvp;
    float pixelScale;
    float alphaMul; // global alpha multiplier (trail fade)
};

constexpr float kZScale = 1.4f; // intensity → world Z height
constexpr int kMaxTrail = 32;
constexpr int kPointCap = 512;

matrix_float4x4 perspective(float fovy, float aspect, float n, float f)
{
    const float ct = 1.0f / std::tan(fovy * 0.5f);
    matrix_float4x4 m = {};
    m.columns[0] = (simd_float4){ct / aspect, 0, 0, 0};
    m.columns[1] = (simd_float4){0, ct, 0, 0};
    m.columns[2] = (simd_float4){0, 0, f / (n - f), -1};
    m.columns[3] = (simd_float4){0, 0, (f * n) / (n - f), 0};
    return m;
}

// Parallel (orthographic) projection for the 2D Front / RTA modes -- maps the
// box [l,r]x[b,t]x[n,f] straight to clip space with no perspective divide.
matrix_float4x4 ortho(float l, float r, float b, float t, float n, float f)
{
    matrix_float4x4 m = matrix_identity_float4x4;
    m.columns[0] = (simd_float4){2.0f / (r - l), 0, 0, 0};
    m.columns[1] = (simd_float4){0, 2.0f / (t - b), 0, 0};
    m.columns[2] = (simd_float4){0, 0, 1.0f / (n - f), 0};
    m.columns[3] = (simd_float4){-(r + l) / (r - l), -(t + b) / (t - b), n / (n - f), 1};
    return m;
}

matrix_float4x4 lookAt(simd_float3 eye, simd_float3 centre, simd_float3 up)
{
    const simd_float3 fwd = simd_normalize(centre - eye);
    const simd_float3 side = simd_normalize(simd_cross(fwd, up));
    const simd_float3 u = simd_cross(side, fwd);
    matrix_float4x4 m = matrix_identity_float4x4;
    m.columns[0] = (simd_float4){side.x, u.x, -fwd.x, 0};
    m.columns[1] = (simd_float4){side.y, u.y, -fwd.y, 0};
    m.columns[2] = (simd_float4){side.z, u.z, -fwd.z, 0};
    m.columns[3] = (simd_float4){-simd_dot(side, eye), -simd_dot(u, eye), simd_dot(fwd, eye), 1};
    return m;
}

// Render a short string to a small premultiplied BGRA texture (UI thread, once).
static id<MTLTexture> makeLabelTexture(id<MTLDevice> device, const juce::String& text, float& outAspect)
{
    const float fontH = 30.0f;
    const int pad = 6;
    juce::Font font{juce::FontOptions(fontH)};
    juce::GlyphArrangement ga;
    ga.addLineOfText(font, text, 0.0f, 0.0f);
    const int w =
        juce::jmax(8, (int)std::ceil(ga.getBoundingBox(0, -1, true).getWidth()) + pad * 2);
    const int h = (int)std::ceil(fontH) + pad;
    outAspect = (float)w / (float)h;

    juce::Image img(juce::Image::ARGB, w, h, true);
    {
        juce::Graphics g(img);
        g.setColour(juce::Colours::white);
        g.setFont(font);
        g.drawText(text, 0, 0, w, h, juce::Justification::centred, false);
    }
    juce::Image::BitmapData bd(img, juce::Image::BitmapData::readOnly);
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    id<MTLTexture> tex = [device newTextureWithDescriptor:td];
    [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
           mipmapLevel:0
             withBytes:bd.data
           bytesPerRow:(NSUInteger)bd.lineStride];
    return tex;
}

const char* kShaderSrc = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Uniforms { float4x4 mvp; float pixelScale; float alphaMul; };
struct PV { packed_float3 pos; packed_float4 color; float size; };
struct POut { float4 pos [[position]]; float4 color; float psize [[point_size]]; };
vertex POut point_vs(uint vid [[vertex_id]],
                     const device PV* v [[buffer(0)]],
                     constant Uniforms& u [[buffer(1)]]) {
    POut o;
    o.pos = u.mvp * float4(float3(v[vid].pos), 1.0);
    float4 c = float4(v[vid].color);
    o.color = float4(c.rgb, c.a * u.alphaMul);
    o.psize = max(1.0, v[vid].size * u.pixelScale);
    return o;
}
fragment float4 point_fs(POut in [[stage_in]], float2 pc [[point_coord]]) {
    float r = length(pc - float2(0.5)) * 2.0;
    if (r > 1.0) discard_fragment();
    float a = in.color.a * smoothstep(1.0, 0.35, r);
    return float4(in.color.rgb * a, a);   // premultiplied
}
struct LV { packed_float3 pos; packed_float4 color; };
struct LOut { float4 pos [[position]]; float4 color; };
vertex LOut line_vs(uint vid [[vertex_id]],
                    const device LV* v [[buffer(0)]],
                    constant Uniforms& u [[buffer(1)]]) {
    LOut o;
    o.pos = u.mvp * float4(float3(v[vid].pos), 1.0);
    float4 c = float4(v[vid].color);
    o.color = float4(c.rgb, c.a * u.alphaMul);
    return o;
}
fragment float4 line_fs(LOut in [[stage_in]]) {
    return float4(in.color.rgb * in.color.a, in.color.a); // premultiplied
}
struct TLV { packed_float3 pos; packed_float2 uv; };
struct TLOut { float4 pos [[position]]; float2 uv; };
vertex TLOut label_vs(uint vid [[vertex_id]],
                      const device TLV* v [[buffer(0)]],
                      constant Uniforms& u [[buffer(1)]]) {
    TLOut o;
    o.pos = u.mvp * float4(float3(v[vid].pos), 1.0);
    o.uv = float2(v[vid].uv);
    return o;
}
fragment float4 label_fs(TLOut in [[stage_in]],
                         texture2d<float> tex [[texture(0)]],
                         constant float& opacity [[buffer(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float4 c = tex.sample(s, in.uv);   // JUCE ARGB is premultiplied
    return c * opacity;                // scale premultiplied rgb + a together
}
)METAL";
} // namespace

// ---- MTKView subclass owns the orbit camera + mouse interaction. -----------
@interface DCRScatterMTKView : MTKView
@property (nonatomic) float camYaw;
@property (nonatomic) float camPitch;
@property (nonatomic) float camDist;
@property (nonatomic) int viewMode;    // 0=3D 1=Front 2=RTA
@property (nonatomic) float orthoZoom; // 2D zoom factor
@end

@implementation DCRScatterMTKView
- (instancetype)initWithFrame:(CGRect)f device:(id<MTLDevice>)d
{
    if (self = [super initWithFrame:f device:d])
    {
        _camYaw = 0.6f;
        _camPitch = -0.35f;
        _camDist = 3.6f;
        _viewMode = 0;
        _orthoZoom = 1.0f;
    }
    return self;
}
- (BOOL)acceptsFirstResponder
{
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)e
{
    return YES;
}
- (void)mouseDragged:(NSEvent*)e
{
    if (_viewMode != 0)
        return;                         // orbit only in 3D; the 2D views are fixed parallel projections
    _camYaw -= (float)e.deltaX * 0.01f; // drag right → scene rotates right
    _camPitch += (float)e.deltaY * 0.01f;
    const float lim = 1.5f;
    _camPitch = std::max(-lim, std::min(lim, _camPitch));
    [self setNeedsDisplay:YES];
}
- (void)scrollWheel:(NSEvent*)e
{
    if (_viewMode == 0)
    {
        _camDist *= (1.0f - (float)e.scrollingDeltaY * 0.02f);
        _camDist = std::max(1.4f, std::min(12.0f, _camDist));
    }
    else
    {
        _orthoZoom *= (1.0f + (float)e.scrollingDeltaY * 0.02f);
        _orthoZoom = std::max(0.4f, std::min(3.0f, _orthoZoom));
    }
    [self setNeedsDisplay:YES];
}
@end

// ---- Renderer: pipelines, trail ring, stems, box, draw. --------------------
@interface DCRScatterRenderer : NSObject <MTKViewDelegate>
- (instancetype)initWithDevice:(id<MTLDevice>)device view:(MTKView*)view;
- (void)pushPoints:(const PointVertex*)pts count:(NSUInteger)count;
- (void)setStems:(const LineVertex*)lines count:(NSUInteger)count;
- (void)setTrailDepth:(int)depth decay:(float)decay;
- (void)buildAxisForMode:(int)mode nyquist:(double)nyquist floorDb:(float)floorDb ceilDb:(float)ceilDb;
- (void)setAxisOpacity:(float)opacity;
@end

@implementation DCRScatterRenderer
{
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pointPSO;
    id<MTLRenderPipelineState> _linePSO;
    id<MTLBuffer> _trail[kMaxTrail];
    NSUInteger _trailCount[kMaxTrail];
    int _trailHead;
    int _trailDepth;
    float _trailDecay;
    id<MTLBuffer> _stemBuf;
    NSUInteger _stemCount;
    id<MTLBuffer> _boxBuf;
    NSUInteger _boxCount;
    id<MTLRenderPipelineState> _labelPSO;
    id<MTLBuffer> _freqAxisBuf;
    NSUInteger _freqAxisCount;
    id<MTLBuffer> _labelQuadBuf;
    id<MTLTexture> _labelTex[kMaxLabels];
    float _labelAX[kMaxLabels];
    float _labelAY[kMaxLabels];
    float _labelAspect[kMaxLabels];
    int _labelCount;
    float _axisOpacity;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device view:(MTKView*)view
{
    if (!(self = [super init]))
        return nil;
    _device = device;
    _queue = [device newCommandQueue];
    _trailHead = -1;
    _trailDepth = 10;
    _trailDecay = 0.8f;
    _stemCount = 0;

    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSrc]
                                              options:nil
                                                error:&err];
    if (lib == nil)
    {
        NSLog(@"[StereoMeter] shader compile failed: %@", err);
        return self;
    }

    auto makePSO = ^id<MTLRenderPipelineState>(NSString* vs, NSString* fs)
    {
        MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
        d.vertexFunction = [lib newFunctionWithName:vs];
        d.fragmentFunction = [lib newFunctionWithName:fs];
        d.colorAttachments[0].pixelFormat = view.colorPixelFormat;
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        d.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne; // premultiplied
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        NSError* e = nil;
        id<MTLRenderPipelineState> pso = [_device newRenderPipelineStateWithDescriptor:d error:&e];
        if (pso == nil)
            NSLog(@"[StereoMeter] pipeline failed: %@", e);
        return pso;
    };
    _pointPSO = makePSO(@"point_vs", @"point_fs");
    _linePSO = makePSO(@"line_vs", @"line_fs");
    _labelPSO = makePSO(@"label_vs", @"label_fs");
    _labelQuadBuf = [device newBufferWithLength:kMaxLabels * 4 * sizeof(TexVertex)
                                        options:MTLResourceStorageModeShared];
    _labelCount = 0;
    _freqAxisCount = 0;
    _axisOpacity = 0.85f;

    // Room wireframe: [-1,1] x [-1,1] x [0,kZScale] box, 12 edges.
    {
        const float zx = kZScale;
        const float c[8][3] = {
            {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, {-1, -1, zx}, {1, -1, zx}, {1, 1, zx}, {-1, 1, zx}};
        const int e[12][2] = {
            {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        std::vector<LineVertex> lv;
        const float col[4] = {0.0f, 1.0f, 0.82f, 0.22f}; // theme cyan, faint
        auto addCorner = [&](int idx)
        {
            LineVertex v;
            v.pos[0] = c[idx][0];
            v.pos[1] = c[idx][1];
            v.pos[2] = c[idx][2];
            v.color[0] = col[0];
            v.color[1] = col[1];
            v.color[2] = col[2];
            v.color[3] = col[3];
            lv.push_back(v);
        };
        for (auto& edge : e)
        {
            addCorner(edge[0]);
            addCorner(edge[1]);
        }
        _boxCount = lv.size();
        _boxBuf = [device newBufferWithBytes:lv.data()
                                      length:lv.size() * sizeof(LineVertex)
                                     options:MTLResourceStorageModeShared];
    }

    for (int i = 0; i < kMaxTrail; ++i)
    {
        _trail[i] = [device newBufferWithLength:kPointCap * sizeof(PointVertex)
                                        options:MTLResourceStorageModeShared];
        _trailCount[i] = 0;
    }
    _stemBuf = [device newBufferWithLength:2 * kPointCap * sizeof(LineVertex)
                                   options:MTLResourceStorageModeShared];
    return self;
}

- (void)pushPoints:(const PointVertex*)pts count:(NSUInteger)count
{
    count = std::min<NSUInteger>(count, kPointCap);
    _trailHead = (_trailHead + 1) % kMaxTrail;
    if (count > 0)
        std::memcpy(_trail[_trailHead].contents, pts, count * sizeof(PointVertex));
    _trailCount[_trailHead] = count;
}

- (void)setStems:(const LineVertex*)lines count:(NSUInteger)count
{
    count = std::min<NSUInteger>(count, 2 * kPointCap);
    if (count > 0)
        std::memcpy(_stemBuf.contents, lines, count * sizeof(LineVertex));
    _stemCount = count;
}

- (void)setTrailDepth:(int)depth decay:(float)decay
{
    _trailDepth = std::max(1, std::min(kMaxTrail, depth));
    _trailDecay = std::max(0.0f, std::min(0.999f, decay));
}

- (void)setAxisOpacity:(float)opacity
{
    _axisOpacity = std::max(0.0f, std::min(1.0f, opacity));
}

- (void)buildAxisForMode:(int)mode nyquist:(double)nyquist floorDb:(float)floorDb ceilDb:(float)ceilDb
{
    struct Tick
    {
        float hz;
        const char* label;
    }; // label == nullptr -> minor tick, no text
    static const Tick ticks[] = {{20, "20"},
                                 {50, nullptr},
                                 {100, "100"},
                                 {200, nullptr},
                                 {500, nullptr},
                                 {1000, "1k"},
                                 {2000, nullptr},
                                 {5000, nullptr},
                                 {10000, "10k"},
                                 {20000, "20k"}};
    const float col[4] = {0.0f, 1.0f, 0.82f, 0.7f}; // theme cyan; alpha scaled by axisOpacity at draw
    std::vector<LineVertex> lv;
    _labelCount = 0;

    auto addLabel = [&](const juce::String& text, float ax, float ay)
    {
        if (_labelCount >= kMaxLabels)
            return;
        float aspect = 1.0f;
        _labelTex[_labelCount] = makeLabelTexture(_device, text, aspect);
        _labelAX[_labelCount] = ax;
        _labelAY[_labelCount] = ay;
        _labelAspect[_labelCount] = aspect;
        ++_labelCount;
    };
    auto addTick = [&](float x0, float y0, float x1, float y1)
    {
        LineVertex a, b;
        a.pos[0] = x0;
        a.pos[1] = y0;
        a.pos[2] = 0.0f;
        b.pos[0] = x1;
        b.pos[1] = y1;
        b.pos[2] = 0.0f;
        for (int k = 0; k < 4; ++k)
        {
            a.color[k] = col[k];
            b.color[k] = col[k];
        }
        lv.push_back(a);
        lv.push_back(b);
    };

    if (mode == 2) // RTA: frequency on the horizontal axis, level on the vertical.
    {
        for (auto& t : ticks)
        {
            if (t.hz >= (float)nyquist)
                continue;
            const float xN = 2.0f * dcr::builtin::freqToNorm(t.hz, 20.0f, (float)nyquist) - 1.0f;
            const bool major = (t.label != nullptr);
            const float len = major ? 0.16f : 0.08f;
            addTick(xN, -1.0f, xN, -1.0f + len);
            if (major)
                addLabel(juce::String(t.label), xN, -1.08f);
        }
        // Vertical dB scale: ceiling (top), midpoint, floor (bottom).
        const float midDb = 0.5f * (floorDb + ceilDb);
        addLabel(juce::String(juce::roundToInt(ceilDb)), -1.12f, 1.0f);
        addLabel(juce::String(juce::roundToInt(midDb)), -1.12f, 0.0f);
        addLabel(juce::String(juce::roundToInt(floorDb)), -1.12f, -1.0f);
        addTick(-1.0f, 0.0f, -0.92f, 0.0f); // mid gridline stub
    }
    else // 3D / Front: frequency on the vertical axis at the left edge.
    {
        for (auto& t : ticks)
        {
            if (t.hz >= (float)nyquist)
                continue;
            const float yN = 2.0f * dcr::builtin::freqToNorm(t.hz, 20.0f, (float)nyquist) - 1.0f;
            const bool major = (t.label != nullptr);
            const float len = major ? 0.16f : 0.08f;
            addTick(kFreqAxisX, yN, kFreqAxisX + len, yN);
            if (major)
                addLabel(juce::String(t.label), kFreqAxisX - 0.14f, yN);
        }
    }

    _freqAxisCount = lv.size();
    _freqAxisBuf = lv.empty() ? nil
                              : [_device newBufferWithBytes:lv.data()
                                                     length:lv.size() * sizeof(LineVertex)
                                                    options:MTLResourceStorageModeShared];
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
}

- (void)drawInMTKView:(MTKView*)view
{
    MTLRenderPassDescriptor* rpd = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (rpd == nil || drawable == nil || _pointPSO == nil || _linePSO == nil)
        return;

    auto* sv = (DCRScatterMTKView*)view;
    const CGSize ds = view.drawableSize;
    const float aspect = ds.height > 0 ? (float)(ds.width / ds.height) : 1.0f;

    simd_float3 camRight, camUp;
    Uniforms u;
    if (sv.viewMode == 0)
    {
        const simd_float3 target = {0, 0, kZScale * 0.4f};
        const simd_float3 dir = {std::cos(sv.camPitch) * std::sin(sv.camYaw),
                                 std::sin(sv.camPitch),
                                 std::cos(sv.camPitch) * std::cos(sv.camYaw)};
        const simd_float3 eye = target + sv.camDist * dir;
        const simd_float3 camFwd = simd_normalize(target - eye);
        camRight = simd_normalize(simd_cross(camFwd, (simd_float3){0, 1, 0}));
        camUp = simd_cross(camRight, camFwd);
        u.mvp = matrix_multiply(perspective(1.0f, aspect, 0.05f, 100.0f),
                                lookAt(eye, target, (simd_float3){0, 1, 0}));
    }
    else
    {
        // 2D parallel projection, fixed front-on camera (looking down -Z).
        const float z = sv.orthoZoom > 0.0f ? sv.orthoZoom : 1.0f;
        const float halfH = 1.25f / z;
        const float halfW = halfH * aspect;
        u.mvp = ortho(-halfW, halfW, -halfH, halfH, -10.0f, 10.0f);
        camRight = (simd_float3){1, 0, 0};
        camUp = (simd_float3){0, 1, 0};
    }
    const CGFloat bw = view.bounds.size.width;
    u.pixelScale = bw > 0 ? (float)(ds.width / bw) : 2.0f;
    u.alphaMul = 1.0f;

    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];

    // Room wireframe.
    [enc setRenderPipelineState:_linePSO];
    [enc setVertexBuffer:_boxBuf offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:_boxCount];

    // Frequency-axis tick lines — faded by Axis opacity.
    if (_freqAxisCount > 0 && _freqAxisBuf != nil)
    {
        u.alphaMul = _axisOpacity;
        [enc setVertexBuffer:_freqAxisBuf offset:0 atIndex:0];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:_freqAxisCount];
        u.alphaMul = 1.0f;
    }

    // Stems (current frame only).
    if (_stemCount > 0)
    {
        [enc setVertexBuffer:_stemBuf offset:0 atIndex:0];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:_stemCount];
    }

    // Point cloud + trail: oldest → newest, alpha decaying with age.
    if (_trailHead >= 0)
    {
        [enc setRenderPipelineState:_pointPSO];
        for (int k = _trailDepth - 1; k >= 0; --k)
        {
            const int slot = ((_trailHead - k) % kMaxTrail + kMaxTrail) % kMaxTrail;
            if (_trailCount[slot] == 0)
                continue;
            u.alphaMul = std::pow(_trailDecay, (float)k);
            [enc setVertexBuffer:_trail[slot] offset:0 atIndex:0];
            [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
            [enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:_trailCount[slot]];
        }
    }

    // Frequency labels — camera-facing billboards at each major tick height.
    if (_labelCount > 0 && _labelPSO != nil)
    {
        TexVertex quads[kMaxLabels * 4];
        for (int i = 0; i < _labelCount; ++i)
        {
            const float hh = kLabelHalfH;
            const float hw = hh * _labelAspect[i];
            const simd_float3 anchor = {_labelAX[i], _labelAY[i], 0.0f};
            auto setc = [&](int j, float sx, float sy, float uu, float vv)
            {
                const simd_float3 p = anchor + camRight * (sx * hw) + camUp * (sy * hh);
                quads[i * 4 + j].pos[0] = p.x;
                quads[i * 4 + j].pos[1] = p.y;
                quads[i * 4 + j].pos[2] = p.z;
                quads[i * 4 + j].uv[0] = uu;
                quads[i * 4 + j].uv[1] = vv;
            };
            setc(0, -1, -1, 0, 1);
            setc(1, 1, -1, 1, 1);
            setc(2, -1, 1, 0, 0);
            setc(3, 1, 1, 1, 0);
        }
        std::memcpy(_labelQuadBuf.contents, quads,
                    (size_t)_labelCount * 4 * sizeof(TexVertex));
        [enc setRenderPipelineState:_labelPSO];
        [enc setVertexBuffer:_labelQuadBuf offset:0 atIndex:0];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
        float op = _axisOpacity;
        [enc setFragmentBytes:&op length:sizeof(op) atIndex:0];
        for (int i = 0; i < _labelCount; ++i)
        {
            [enc setFragmentTexture:_labelTex[i] atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:(NSUInteger)(i * 4)
                    vertexCount:4];
        }
    }

    [enc endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
}
@end

namespace dcr::builtin
{

// Right-hand control sidebar: a vertical stack of sliders bound to the
// processor's APVTS, plus an axis legend at the bottom. (The Metal NSView can't
// be overlaid by JUCE, so the "coordinates" live here as a legend rather than
// on-plot camera-tracked ticks — that's a later pass.)
class StereoControls : public juce::Component
{
   public:
    explicit StereoControls(StereoMeterProcessor& p) : proc(p)
    {
        auto& s = p.getValueTreeState();
        struct Def
        {
            const char* id;
            const char* name;
        };
        static const Def defs[kN] = {
            {"floorDb", "Floor (dB)"},
            {"ceilDb", "Ceiling (dB)"},
            {"highLift", "High lift"},
            {"liftPivot", "Lift pivot"},
            {"pointMin", "Min size"},
            {"pointMax", "Max size"},
            {"heightScale", "Height"},
            {"smooth", "Smooth"},
            {"colorSat", "Colour"},
            {"axisOpacity", "Axis opacity"},
            {"trailDepth", "Trail"},
            {"trailDecay", "Trail fade"},
            {"stemAmount", "Stems"}};
        for (int i = 0; i < kN; ++i)
        {
            sliders[i].setSliderStyle(juce::Slider::LinearHorizontal);
            sliders[i].setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 15);
            addAndMakeVisible(sliders[i]);
            labels[i].setText(defs[i].name, juce::dontSendNotification);
            labels[i].setFont(juce::FontOptions(10.5f));
            labels[i].setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.82f));
            addAndMakeVisible(labels[i]);
            atts[i] = std::make_unique<Attach>(s, defs[i].id, sliders[i]);
        }
        legend.setText("3D: X=Pan Y=Freq Z=Level (drag = orbit)\nFront: Pan x Freq    RTA: Freq x Level\ncolour = phase (red anti / green in)   scroll = zoom",
                       juce::dontSendNotification);
        legend.setJustificationType(juce::Justification::topLeft);
        legend.setFont(juce::FontOptions(10.0f));
        legend.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(legend);
        saveBtn.setButtonText("Save");
        resetBtn.setButtonText("Reset");
        addAndMakeVisible(saveBtn);
        addAndMakeVisible(resetBtn);
        saveBtn.onClick = [this]
        { proc.saveUserDefault(); };
        resetBtn.onClick = [this]
        { proc.resetToFactory(); };

        // View-mode selector: 3 segmented buttons bound to the "viewMode" choice.
        viewParam = dynamic_cast<juce::AudioParameterChoice*>(s.getParameter("viewMode"));
        static const char* segNames[3] = {"3D", "Front", "RTA"};
        for (int i = 0; i < 3; ++i)
        {
            seg[i].setButtonText(segNames[i]);
            seg[i].setClickingTogglesState(true);
            seg[i].setRadioGroupId(9201);
            seg[i].setConnectedEdges(((i > 0) ? juce::Button::ConnectedOnLeft : 0) | ((i < 2) ? juce::Button::ConnectedOnRight : 0));
            addAndMakeVisible(seg[i]);
            seg[i].onClick = [this, i]
            {
                if (viewParam != nullptr)
                    *viewParam = i;
            };
        }
        syncViewSeg();
    }

    // Keep the segmented buttons in sync with the parameter (e.g. after a
    // snapshot restore or Reset). Called from the editor's 60 Hz timer.
    void syncViewSeg()
    {
        const int idx = viewParam != nullptr ? viewParam->getIndex() : 0;
        for (int i = 0; i < 3; ++i)
            seg[i].setToggleState(i == idx, juce::dontSendNotification);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour::fromRGB(18, 18, 22));
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.drawLine(0.5f, 0.0f, 0.5f, (float)getHeight());
    }

    void resized() override
    {
        auto full = getLocalBounds().reduced(8);
        auto top = full.removeFromTop(24);
        saveBtn.setBounds(top.removeFromLeft(top.getWidth() / 2).reduced(2, 0));
        resetBtn.setBounds(top.reduced(2, 0));
        full.removeFromTop(6);
        auto segRow = full.removeFromTop(22);
        for (int i = 0; i < 3; ++i)
            seg[i].setBounds(segRow.removeFromLeft(segRow.getWidth() / (3 - i)));
        full.removeFromTop(6);
        auto r = full;
        legend.setBounds(r.removeFromBottom(84));
        const int rh = juce::jmax(30, r.getHeight() / kN);
        for (int i = 0; i < kN; ++i)
        {
            auto row = r.removeFromTop(rh);
            labels[i].setBounds(row.removeFromTop(13));
            sliders[i].setBounds(row.removeFromTop(juce::jmin(20, row.getHeight())));
        }
    }

   private:
    static constexpr int kN = 13;
    using Attach = juce::AudioProcessorValueTreeState::SliderAttachment;
    StereoMeterProcessor& proc;
    juce::TextButton saveBtn, resetBtn;
    juce::TextButton seg[3];
    juce::AudioParameterChoice* viewParam = nullptr;
    juce::Slider sliders[kN];
    juce::Label labels[kN];
    juce::Label legend;
    std::unique_ptr<Attach> atts[kN];
};

struct StereoMeterEditor::Impl : private juce::Timer
{
    StereoMeterProcessor& proc;
    StereoFreqAnalyzer analyzer;
    StereoFreqAnalyzer::Frame frame;
    std::vector<float> winL, winR, tmp;
    std::vector<PointVertex> pts;
    std::vector<LineVertex> stems;
    float nyquistHz = 24000.0f;
    int lastViewMode = -1;
    float lastFloor = 1.0f, lastCeil = 1.0f;

    id<MTLDevice> device = nil;
    DCRScatterMTKView* view = nil;
    DCRScatterRenderer* renderer = nil;
    juce::NSViewComponent nsView;
    std::unique_ptr<StereoControls> controls;

    explicit Impl(StereoMeterProcessor& p)
        : proc(p),
          analyzer(juce::jmax(8000.0, p.meterSampleRate()), 2048, 256, 20.0f)
    {
        nyquistHz = (float)(juce::jmax(8000.0, p.meterSampleRate()) * 0.5);
        const int M = analyzer.windowSize();
        winL.assign((size_t)M, 0.0f);
        winR.assign((size_t)M, 0.0f);

        device = MTLCreateSystemDefaultDevice();
        view = [[DCRScatterMTKView alloc] initWithFrame:NSMakeRect(0, 0, 800, 600) device:device];
        view.clearColor = MTLClearColorMake(0.015, 0.015, 0.020, 1.0);
        view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        view.framebufferOnly = YES;
        view.paused = YES;
        view.enableSetNeedsDisplay = YES;
        renderer = [[DCRScatterRenderer alloc] initWithDevice:device view:view];
        view.delegate = renderer;
        [renderer buildAxisForMode:0 nyquist:(double)nyquistHz floorDb:-60.0f ceilDb:0.0f];

        nsView.setView((__bridge void*)view);
        controls = std::make_unique<StereoControls>(p);
        startTimerHz(60);
    }

    ~Impl() override
    {
        stopTimer();
        nsView.setView(nullptr);
        if (view != nil)
            view.delegate = nil;
    }

    static void rollInto(std::vector<float>& win, const float* src, int got)
    {
        const int M = (int)win.size();
        if (got >= M)
            std::memcpy(win.data(), src + (got - M), (size_t)M * sizeof(float));
        else if (got > 0)
        {
            std::memmove(win.data(), win.data() + got, (size_t)(M - got) * sizeof(float));
            std::memcpy(win.data() + (M - got), src, (size_t)got * sizeof(float));
        }
    }

    void timerCallback() override
    {
        const int M = analyzer.windowSize();
        const int cap = 4 * M;
        if ((int)tmp.size() < cap)
            tmp.resize((size_t)cap);

        auto drain = [&](dcr::FloatRingBuffer& ring, std::vector<float>& win)
        {
            size_t avail = ring.readAvailable();
            if (avail == 0)
                return;
            const size_t want = std::min(avail, (size_t)cap);
            if (avail > want)
            {
                float junk[1024];
                size_t toSkip = avail - want;
                while (toSkip > 0)
                {
                    const size_t s = std::min(toSkip, (size_t)1024);
                    ring.read(junk, s);
                    toSkip -= s;
                }
            }
            const size_t got = ring.read(tmp.data(), want);
            rollInto(win, tmp.data(), (int)got);
        };
        drain(proc.ringL(), winL);
        drain(proc.ringR(), winR);

        // Live parameters from the sidebar sliders.
        auto& s = proc.getValueTreeState();
        const float pFloor = s.getRawParameterValue("floorDb")->load();
        const float pCeil = s.getRawParameterValue("ceilDb")->load();
        const float pHighLift = s.getRawParameterValue("highLift")->load();
        const float pLiftPivot = s.getRawParameterValue("liftPivot")->load();
        const float pPointMin = s.getRawParameterValue("pointMin")->load();
        const float pPointMax = s.getRawParameterValue("pointMax")->load();
        const float pHeight = s.getRawParameterValue("heightScale")->load();
        const float pSmooth = s.getRawParameterValue("smooth")->load();
        const float pColorSat = s.getRawParameterValue("colorSat")->load();
        const int pTrailDepth = (int)s.getRawParameterValue("trailDepth")->load();
        const float pTrailDecay = s.getRawParameterValue("trailDecay")->load();
        const float pStemAmount = s.getRawParameterValue("stemAmount")->load();
        const int pView = (int)s.getRawParameterValue("viewMode")->load();
        analyzer.setIntensityRange(pFloor, pCeil);
        analyzer.setSmoothing(pSmooth);

        // Push the view mode to the Metal view, keep the selector synced, and
        // rebuild the axis when the mode (or, in RTA, the dB window) changes.
        view.viewMode = pView;
        controls->syncViewSeg();
        if (pView != lastViewMode || (pView == 2 && (pFloor != lastFloor || pCeil != lastCeil)))
        {
            [renderer buildAxisForMode:pView nyquist:(double)nyquistHz floorDb:pFloor ceilDb:pCeil];
            lastViewMode = pView;
            lastFloor = pFloor;
            lastCeil = pCeil;
        }

        analyzer.process(winL.data(), winR.data(), frame);

        const int N = (int)frame.ints.size();
        pts.clear();
        pts.reserve((size_t)N);
        stems.clear();
        const bool wantStems = pStemAmount > 0.005f;

        for (int i = 0; i < N; ++i)
        {
            const float intensity = frame.ints[(size_t)i];
            const float freqNorm = N > 1 ? (float)i / (float)(N - 1) : 0.0f; // 0 low → 1 high (Y axis)
            // HF tilt: lift only true highs (flat below the pivot) so bass/mids stay put.
            const float gain = dcr::builtin::highLiftGain(frame.freqs[(size_t)i], pLiftPivot, nyquistHz, pHighLift);
            const float it = std::min(1.0f, intensity * gain);
            if (it <= 0.01f)
                continue; // gate silence

            const float pan = frame.pans[(size_t)i];
            const float yN = 2.0f * freqNorm - 1.0f;
            const float z = it * kZScale * pHeight;
            const float c = frame.cohs[(size_t)i]; // -1..1

            const float w = std::fabs(c) * pColorSat;
            float r, gg, b;
            if (c >= 0.0f)
            {
                r = 0.92f * (1 - w) + 0.18f * w;
                gg = 0.94f * (1 - w) + 0.85f * w;
                b = 0.96f * (1 - w) + 0.40f * w;
            }
            else
            {
                r = 0.92f * (1 - w) + 0.92f * w;
                gg = 0.94f * (1 - w) + 0.30f * w;
                b = 0.96f * (1 - w) + 0.22f * w;
            }

            PointVertex pv;
            if (pView == 2) // RTA: X = freq, Y = level
            {
                pv.pos[0] = yN;
                pv.pos[1] = 2.0f * it - 1.0f;
                pv.pos[2] = 0.0f;
            }
            else if (pView == 1) // Front: X = pan, Y = freq (flat)
            {
                pv.pos[0] = pan;
                pv.pos[1] = yN;
                pv.pos[2] = 0.0f;
            }
            else // 3D
            {
                pv.pos[0] = pan;
                pv.pos[1] = yN;
                pv.pos[2] = z;
            }
            pv.color[0] = r;
            pv.color[1] = gg;
            pv.color[2] = b;
            pv.color[3] = std::min(1.0f, 0.18f + it);
            pv.size = pPointMin + it * (pPointMax - pPointMin);
            pts.push_back(pv);

            if (wantStems && pView == 0) // stems only meaningful in the 3D view
            {
                const float sa = pStemAmount * it;
                LineVertex lo, hi;
                lo.pos[0] = pan;
                lo.pos[1] = yN;
                lo.pos[2] = 0.0f;
                hi.pos[0] = pan;
                hi.pos[1] = yN;
                hi.pos[2] = z;
                lo.color[0] = r;
                lo.color[1] = gg;
                lo.color[2] = b;
                lo.color[3] = sa;
                hi.color[0] = r;
                hi.color[1] = gg;
                hi.color[2] = b;
                hi.color[3] = sa;
                stems.push_back(lo);
                stems.push_back(hi);
            }
        }

        const float pAxisOpacity = s.getRawParameterValue("axisOpacity")->load();
        [renderer setAxisOpacity:pAxisOpacity];
        [renderer setTrailDepth:pTrailDepth decay:pTrailDecay];
        [renderer setStems:stems.data() count:stems.size()];
        [renderer pushPoints:pts.data() count:pts.size()];
        [view setNeedsDisplay:YES];
    }
};

StereoMeterEditor::StereoMeterEditor(StereoMeterProcessor& p)
    : juce::AudioProcessorEditor(p), impl(std::make_unique<Impl>(p))
{
    addAndMakeVisible(impl->nsView);
    addAndMakeVisible(*impl->controls);
    setSize(900, 620);
}

StereoMeterEditor::~StereoMeterEditor() = default;

void StereoMeterEditor::resized()
{
    auto r = getLocalBounds();
    impl->controls->setBounds(r.removeFromRight(216));
    impl->nsView.setBounds(r);
}

} // namespace dcr::builtin

#include "DSP/Builtin/StereoMeterEditor.h"

#include "DSP/Builtin/StereoFreqAnalyzer.h"
#include "DSP/Builtin/StereoMeterProcessor.h"

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
)METAL";
} // namespace

// ---- MTKView subclass owns the orbit camera + mouse interaction. -----------
@interface DCRScatterMTKView : MTKView
@property (nonatomic) float camYaw;
@property (nonatomic) float camPitch;
@property (nonatomic) float camDist;
@end

@implementation DCRScatterMTKView
- (instancetype)initWithFrame:(CGRect)f device:(id<MTLDevice>)d
{
    if (self = [super initWithFrame:f device:d])
    {
        _camYaw = 0.6f;
        _camPitch = -0.35f;
        _camDist = 3.6f;
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
    _camYaw -= (float)e.deltaX * 0.01f; // drag right → scene rotates right
    _camPitch += (float)e.deltaY * 0.01f;
    const float lim = 1.5f;
    _camPitch = std::max(-lim, std::min(lim, _camPitch));
    [self setNeedsDisplay:YES];
}
- (void)scrollWheel:(NSEvent*)e
{
    _camDist *= (1.0f - (float)e.scrollingDeltaY * 0.02f);
    _camDist = std::max(1.4f, std::min(12.0f, _camDist));
    [self setNeedsDisplay:YES];
}
@end

// ---- Renderer: pipelines, trail ring, stems, box, draw. --------------------
@interface DCRScatterRenderer : NSObject <MTKViewDelegate>
- (instancetype)initWithDevice:(id<MTLDevice>)device view:(MTKView*)view;
- (void)pushPoints:(const PointVertex*)pts count:(NSUInteger)count;
- (void)setStems:(const LineVertex*)lines count:(NSUInteger)count;
- (void)setTrailDepth:(int)depth decay:(float)decay;
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

    const simd_float3 target = {0, 0, kZScale * 0.4f};
    const simd_float3 dir = {std::cos(sv.camPitch) * std::sin(sv.camYaw),
                             std::sin(sv.camPitch),
                             std::cos(sv.camPitch) * std::cos(sv.camYaw)};
    const simd_float3 eye = target + sv.camDist * dir;

    Uniforms u;
    u.mvp = matrix_multiply(perspective(1.0f, aspect, 0.05f, 100.0f),
                            lookAt(eye, target, (simd_float3){0, 1, 0}));
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
    explicit StereoControls(juce::AudioProcessorValueTreeState& s)
    {
        struct Def
        {
            const char* id;
            const char* name;
        };
        static const Def defs[kN] = {
            {"floorDb", "Floor (dB)"}, {"ceilDb", "Ceiling (dB)"}, {"highLift", "High lift"}, {"pointMin", "Min size"}, {"pointMax", "Max size"}, {"heightScale", "Height"}, {"smooth", "Smooth"}, {"colorSat", "Colour"}, {"trailDepth", "Trail"}, {"trailDecay", "Trail fade"}, {"stemAmount", "Stems"}};
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
        legend.setText("X = Pan (L <-> R)\nY = Frequency (20 Hz -> 20 kHz)\nZ = Level   colour = phase\n(red anti / green in)\nDrag = orbit   Scroll = zoom",
                       juce::dontSendNotification);
        legend.setJustificationType(juce::Justification::topLeft);
        legend.setFont(juce::FontOptions(10.0f));
        legend.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(legend);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour::fromRGB(18, 18, 22));
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.drawLine(0.5f, 0.0f, 0.5f, (float)getHeight());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8);
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
    static constexpr int kN = 11;
    using Attach = juce::AudioProcessorValueTreeState::SliderAttachment;
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

    id<MTLDevice> device = nil;
    DCRScatterMTKView* view = nil;
    DCRScatterRenderer* renderer = nil;
    juce::NSViewComponent nsView;
    std::unique_ptr<StereoControls> controls;

    explicit Impl(StereoMeterProcessor& p)
        : proc(p),
          analyzer(juce::jmax(8000.0, p.meterSampleRate()), 2048, 256, 20.0f)
    {
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

        nsView.setView((__bridge void*)view);
        controls = std::make_unique<StereoControls>(p.getValueTreeState());
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
        const float pPointMin = s.getRawParameterValue("pointMin")->load();
        const float pPointMax = s.getRawParameterValue("pointMax")->load();
        const float pHeight = s.getRawParameterValue("heightScale")->load();
        const float pSmooth = s.getRawParameterValue("smooth")->load();
        const float pColorSat = s.getRawParameterValue("colorSat")->load();
        const int pTrailDepth = (int)s.getRawParameterValue("trailDepth")->load();
        const float pTrailDecay = s.getRawParameterValue("trailDecay")->load();
        const float pStemAmount = s.getRawParameterValue("stemAmount")->load();
        analyzer.setIntensityRange(pFloor, pCeil);
        analyzer.setSmoothing(pSmooth);

        analyzer.process(winL.data(), winR.data(), frame);

        const int N = (int)frame.ints.size();
        pts.clear();
        pts.reserve((size_t)N);
        stems.clear();
        const bool wantStems = pStemAmount > 0.005f;

        for (int i = 0; i < N; ++i)
        {
            const float intensity = frame.ints[(size_t)i];
            const float freqNorm = N > 1 ? (float)i / (float)(N - 1) : 0.0f; // 0 low → 1 high
            const float it = std::min(1.0f, intensity * (1.0f + pHighLift * freqNorm * 3.0f));
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
            pv.pos[0] = pan;
            pv.pos[1] = yN;
            pv.pos[2] = z;
            pv.color[0] = r;
            pv.color[1] = gg;
            pv.color[2] = b;
            pv.color[3] = std::min(1.0f, 0.18f + it);
            pv.size = pPointMin + it * (pPointMax - pPointMin);
            pts.push_back(pv);

            if (wantStems)
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

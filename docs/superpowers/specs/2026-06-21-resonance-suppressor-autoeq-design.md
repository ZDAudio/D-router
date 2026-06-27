# Resonance Suppressor → Auto-EQ 同级插件 — 设计 (Spec)

**Date:** 2026-06-21
**Branch:** `resonance-suppressor-autoeq`
**Status:** Approved design, pre-implementation

## 目标

把 `ResonanceSuppressorProcessor` 从「使用通用线性编辑器的简单插件」升级为
`SpectralAutoEqProcessor`（"auto eq"）的同级插件:对数频率交互编辑器、可拖拽的
按频率控制曲线、可选 1/N 倍频程分辨率、无锁节点通信、曲线随状态持久化。核心功能
仍是**动态削减共振**(在平滑基线之上探测突出的 bin 并按 attack/release 逐 bin 下压)。

非目标:不改 STFT 引擎;不改延迟/PDC 行为;不重构正在工作的 Auto-EQ;不删
`SpectralCurveEditor.h`(仅停止在 RS 中使用)。

## 总体

- 保留名字 `Resonance Suppressor`、id `builtin:resonance`。
- 继续基于 `SpectralProcessor`(STFT/WOLA)。**FFT 仍 2048(fftOrder 11)、overlap 4**
  → 延迟仍 `setLatencySamples(fftSize)`,不变。
- 公共仓库品牌洁净:名称/注释保持中性。

## 检测逻辑(DSP)

保留现有逐-bin 探测 + 每-bin attack/release 的核心,改两点:

1. **基线宽度由 Resolution 驱动**:`logSmooth(magDb, base, n, strength)` 的 `strength`
   不再写死 1.2,而由 `res` 参数映射(见下)。
2. **阈值变成按频率**:

   ```
   thr(k)      = depth + curveOffset(k)            // curveOffset = 可拖拽曲线插值到 bin
   excess(k)   = magDb(k) − base(k) − thr(k)
   if k < minBin:        targetRed = 0             // minfreq 硬底:低于此频率永不动
   else if excess > 0:   targetRed = −min(maxRed, excess × sharpness)
   else:                 targetRed = 0
   // 每-bin attack/release 平滑(下压更狠用 attack,放松用 release),与现状一致
   r(k)       += (targetRed − r(k)) × ((targetRed < r(k)) ? attCoeff : relCoeff)
   gains(k)    = decibelsToGain(r(k))
   ```

### Resolution → 基线宽度 + 显示细度

`res` 选项 `1/3 · 1/6 · 1/12 · 1/24 oct`(默认 idx 1 = 1/6),同时驱动:

- **logSmooth 基线强度**(声音):`baseStrengthForRes(N)`,初值
  `1/3→0.6, 1/6→1.2, 1/12→2.4, 1/24→4.0`(强度越大基线越紧 → 只削越尖锐的共振)。
  1/3 oct = 基线最宽 → 最激进;1/24 oct = 基线最紧 → 最外科。具体数值留待真机听音微调。
- **显示频段细度**(对数 log 频段,见显示接口)。

## 参数(零 ID 破坏)

| ID | 名称 | 范围 / 默认 | 处理 |
|---|---|---|---|
| `res` | Resolution | choice {1/3,1/6,1/12,1/24 oct}, 默认 1/6 | **新增** |
| `depth` | Depth | 0..12 dB, 默认 4.0 | 保留 |
| `sharpness` | Sharpness | 0.1..2.0, 默认 0.8 | 保留 |
| `reduction` | Max reduction | 1..24 dB, 默认 12.0 | 保留 |
| `attack` | Attack | 1..100 ms (skew 0.5), 默认 8 | 保留 |
| `release` | Release | 10..500 ms (skew 0.4), 默认 120 | 保留 |
| `minfreq` | Min freq | 20..2000 Hz (skew 0.3), 默认 150 | 保留(硬底) |

旧预设照常加载:`res` 取默认、`<CURVE>` 缺失则曲线全 0(=统一阈值,行为≈现状)。

## 可拖拽阈值曲线 + 持久化(照搬 Auto-EQ 模式)

- `kNumNodes = 31`,对数频率网格 20 Hz–20 kHz,复用 `nodeFreq(i)=20·1000^(i/30)`。
- 节点值 = **阈值偏移 dB**,clamp `±18`。拉低=该处阈值更低=更易判为共振并削减;
  拉高=保护该频段;0=用全局 depth。
- **无锁通信**:`std::array<std::atomic<float>, 31> nodes` + `std::atomic<int> curveGen`。
  音频线程只在 `curveGen` 变化时调用 `rebuildCurveFromNodes()` 重建 per-bin
  `curveOffset[]`(用 `nodeLo/nodeFrac` 插值,`buildNodeMap()` 在 prepare 时建好)。
- **持久化**:覆写 `getStateInformation`/`setStateInformation`,在 APVTS XML 上挂
  `<CURVE>` 子节点(属性 n0..n30);加载后 `curveGen.fetch_add(1)` 发布,并在 `replaceState`
  前剥离 `<CURVE>` 子节点(与 Auto-EQ 的 `<TARGET>` 完全一致)。

## 显示接口(供新编辑器)

线性 128 点显示 → Auto-EQ 式**对数显示频段**(直接复用 Auto-EQ 的实现):

- `kMaxDisp = 384`;`dispFreq[] / dispBinLo[] / dispBinHi[] / dispBinCtr[]`,`dispCount`(atomic)。
- `rebuildDisplayBands(numBins, N)`:20 Hz..min(nyq,20kHz) 的 1/N-oct 对数频段;仅在
  `res` 变化时重建(与 cachedResN 比较)。
- `updateDisplay()`(仅 ch0):`dispMag[i]` = 频段内 magDb 均值(用 cumSum),
  `dispBase[i]` = 中心 bin 的 base(基线),`dispGain[i]` = `redDb[0][中心 bin]`。
- 读接口:`getDisplaySize/getDisplayFreq/getDisplayMagDb/getDisplayGainDb` +
  **新增 `getDisplayBaseDb(i)`**(画基线用)。
- 标签接口:`labelFftSize/labelOverlapPct/labelWindow/resolutionN`(无 mode)。

## 编辑器:新建 `ResonanceSuppressorEditor.h`

仿 `SpectralAutoEqEditor`,对数频率图(不模板化共享,避免动到 Auto-EQ):

- 顶部图:蓝填充**频谱** + 浅灰**基线**(`getDisplayBaseDb`) + 青色**实时削减量**
  (`getDisplayGainDb`) + 琥珀色**可编辑阈值曲线 + 节点**。
- 增益轴用 `±24`(削减最深到 −24 不裁切;阈值节点 clamp ±18);频谱轴 −90..0 dBFS;
  频率轴对数,刻度 50/100/200/500/1k/2k/5k/10k。
- 交互(复用 Auto-EQ):垂直拖=设 dB,水平拖=扫刷多个节点,双击=节点归零。
- 信息标签:`Hann · 2048 · 75% · 1/N oct`。
- 下方 `juce::GenericAudioProcessorEditor`(res/depth/sharpness/reduction/attack/release/minfreq),
  经 APVTS 双向同步。
- `~Editor` 中 `stopTimer()`;编辑器生命周期遵循现有约定(由 host/引擎在销毁插件前关闭窗口)。

## 文件改动

1. `Source/DSP/Builtin/ResonanceSuppressorProcessor.h` — 重写。
2. `Source/DSP/Builtin/ResonanceSuppressorEditor.h` — **新建**。
3. `Source/DSP/Builtin/InternalPluginFormat.cpp` — `ResonanceSuppressorProcessor::createEditor()`
   改返回 `new ResonanceSuppressorEditor(*this)`;include 由 `SpectralCurveEditor.h` 换成
   `ResonanceSuppressorEditor.h`(若 `SpectralCurveEditor.h` 不再被任何文件 include,则保留文件、
   仅去掉这一处 include)。
4. `Source/DSP/Builtin/ResonanceMath.h` — **新建**,JUCE-free 确定性数学(可测):
   - `nodeFreq(i)`、节点对数网格插值权重;
   - `targetReductionDb(magDb, baseDb, threshold, sharpness, maxRed)`。
   processor 调用这些函数,作为单一真相来源。
5. `tests/test_main.cpp` — 加 ctest 用例覆盖 `ResonanceMath`。

## RT 安全(不可妥协)

- 音频线程零分配/零锁/零 IO:`curveOffset/base/magDb/cumSum/redDb/disp*` 全部在
  `prepareSpectral()` 预分配并 `.clear()`/复用;`rebuildBandEdges/rebuildDisplayBands/
  rebuildCurveFromNodes` 只在参数/曲线/分辨率**变化时**触发,且不分配(向量已预分配到上界)。
- 曲线读写无锁(atomic + generation),编辑器与音频线程不共享锁。

## 测试与验证(遵守 CLAUDE.md)

- `cmake --build build -j` 必须通过。
- `cmake --build build --target dcorerouter_tests && ctest --test-dir build --output-on-failure`
  必须通过(含新增 `ResonanceMath` 用例)。
- **音频/手感、对数编辑器交互**只能由用户真机听音/操作确认 — 实现完成后如实标注「未验证」。
- 延迟/PDC 行为不变。

## 取舍记录

- **专属编辑器**(非模板共享):不动 Auto-EQ,零回归;代价 ~200 行相似代码。未来可考虑提取共享基类(本次不做)。
- **保留 minfreq**:安全、零破坏;与曲线低端保护略有重叠但语义不同(硬底 vs 软阈值)。
- **Resolution 影响声音**(基线宽度):比"只改显示"更贴合 auto-eq 方向。

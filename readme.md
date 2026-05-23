# D-Core Router

**D-Core Router** is a real-time, low-latency audio routing matrix application for macOS, built in C++20 using the JUCE 8 framework. It acts as a software patchbay, routing audio channels between CoreAudio physical and virtual devices, supporting per-crosspoint gain, sample-rate conversion, plug-in insert chains, and output group master fader controls.

---

## 1. Project Goal
To provide audio professionals (composers, mixing engineers, studio operators) with an extremely reliable, low-latency, and high-performance software audio routing matrix (scaling up to 512x512) that delivers clear real-time monitoring and routing adjustments.

---

## 2. Architecture Overview

### Audio Engine & DSP
- **AudioEngine** (`Source/Engine/`): Coordinates active input/output CoreAudio devices, handles sample rate conversion (SRC) using macOS native AudioToolbox converters, and drives the routing loop.
- **MatrixProcessor** (`Source/Engine/`): Handles low-latency audio routing in a dedicated real-time high-priority thread.
- **PluginHost** & **MultiChannelPluginHost** (`Source/DSP/`): Hosts individual channel and multi-channel AU plugins within output insert slots.

### Routing Data Models
- **RoutingMatrix** (`Source/Routing/`): Core data model representing the grid and gains.
- **OutputGroupManager** (`Source/Routing/`): Manages VCA-like master control groups, linking master faders to output trims.

### UI / Visual Layer
- **LookAndFeel** (`Source/UI/`): Custom skin overriding standard JUCE component rendering (flat, machined hardware console aesthetics).
- **MatrixView** (`Source/UI/`): Scrollable routing canvas displaying input and output channel strips, level meters, and the crosspoint grid.
- **CrosspointGrid** (`Source/UI/`): Single-component high-performance grid renderer capable of drawing 100k+ cells efficiently.
- **OutputGroupPanel** (`Source/UI/`): Bottom/floating rack unit hosting master group faders and AU insert plugin slots.
- **StatusPanel** (`Source/UI/`): Real-time diagnostic console displaying CPU, buffer fills, latency tables, and health logs.

---

## 3. 讨论记录

### 2026-05-23: UI 重构与视觉升级
- **设计方向**：基于第一性原则与 ISTP 逻辑，将 `dcorerouter` 视觉重构为前卫工业风的 **D-Core Router**。
- **改进点**：
  - 引入了 `dcr::LookAndFeel` 全局自定义皮肤，提供扁平金属质感按钮与指示灯、极简旋钮和专业推子；
  - Crosspoint 细化为网格十字物理触点，激活时根据衰减 (Teal) 或增益 (Amber) 进行不同发光色调渲染并标记精确数值；
  - 关卡表 (LevelMeter) 改为 12 段经典硬件 LED 格栅电平表；
  - 组卡片 (Card) 外框增加螺丝钉指示与暗金 (Bypass)/深绿 (Active) 效果插槽；
  - 状态面板重构为荧光绿终端样式。

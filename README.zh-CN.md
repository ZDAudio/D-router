<div align="center">

<img src="Resources/icon.png" width="120" alt="D-Router icon" />

# D-Router

**面向 macOS 的实时、低延迟 NxM 音频路由矩阵。**

*by ZDAudio*

![platform](https://img.shields.io/badge/platform-macOS%2012%2B-black)
![language](https://img.shields.io/badge/C%2B%2B-20-blue)
![status](https://img.shields.io/badge/status-private%20beta-yellow)

[English](README.md) | **简体中文**

</div>

---

D-Router 是一款软件跳线盘 / 路由矩阵，可连接 Mac 上任意组合的音频设备——既包括物理音频
接口，也包括虚拟回环设备——并在它们之间自由路由通道，提供逐交叉点增益、采样率转换、插件
插入链以及联动编组推子。它以 **C++20** 为 macOS 打造，配备手写的实时混音引擎。

> **状态：私有测试版（private beta）。** 预发布软件，正在积极开发中。请勿二次分发。

## 功能亮点

- **NxM 矩阵路由**——单一高性能交叉点网格，即便扩展到数百通道（目标 512×512）依旧顺滑，
  支持逐交叉点增益、静音 / 独奏，以及无突变噪声（zipper noise）的增益平滑。
- **多设备、多时钟**——可聚合任意一组输入与输出设备。每个通道都拥有独立的采样率转换器，
  汇入固定的引擎时钟域；采样率匹配的通路会被旁通以保持透明。
- **实时引擎**——一条专用的、按实时优先级调度的矩阵线程，由设备回调事件驱动，向每通道的
  无锁 SPSC 环形缓冲区供给数据。提供实时的 xrun / 丢帧 / 余量诊断。
- **插件托管**——可在逐通道插入链（3 个槽位）和多通道**编组**链（5 个槽位）中托管插件，
  支持拖拽重排、旁通，以及带崩溃保护的加载 / 卸载。插入效果运行于**推子前**
  （信号 → 插件 → 推子 → 输出）。
- **18 个内置 DSP 插件**（见下）——无需任何外部依赖，即可在任意通道或母线上获得实用的
  处理能力。
- **输出 / 输入编组**——联动的主推子可同时移动每个成员的微调（trim）、编组静音，以及共享
  的多通道插入链。
- **工作流**——逐设备的通道折叠 / 展开、快照保存 / 载入、统一的“缓冲安全”延迟预设、虚拟设备
  自环阻断、原生菜单栏，以及关闭到托盘。

## 内置插件

| | | |
|---|---|---|
| 增益 / 实用工具 | 高通 / 低通滤波器 | 5 段参量均衡 |
| 压缩器 | 噪声门 | 限制器 |
| 混响 | 延迟 | 信号发生器 |
| 颤音 | 立体声宽度 | 齿音消除器 |
| 通道条 | 多段压缩器（4 段） | 电平骑乘器 |
| PPM 表 | 频谱自动均衡 | 共振抑制器 |

其中数个配有自定义可视化编辑器（均衡曲线、压缩器传输曲线 + 增益衰减表、电平骑乘器的增益
历史、频谱曲线、PPM 弹道）。两个频谱插件（自动均衡、共振抑制器）共享同一套 STFT/WOLA
引擎，会引入一个 FFT 帧的延迟。

## 推荐配置 — 捕获 Mac 的音频（BlackHole）

macOS 本身没有内置的方式把系统与应用的声音送入路由器。要充分发挥 D-Router 的能力，最顺手
的做法是将它与 **[BlackHole](https://existential.audio/blackhole/)** 搭配使用——这是一款
面向 macOS 的免费开源虚拟音频驱动。你把 macOS 的输出指向 BlackHole，D-Router 再把它读回来
并路由到任意目的地，同时进行处理、多设备输出与编组控制。

**安装**——通过 Homebrew，或从
[existential.audio](https://existential.audio/blackhole/) 下载安装程序：

```bash
brew install blackhole-2ch      # 立体声 / 常规使用
brew install blackhole-16ch     # 多通道 / 空间音频
```

**选择通道数：**

- **常规（立体声）** → **BlackHole 2ch**。
- **空间音频 / Dolby Atmos**（多通道）→ **BlackHole 16ch**，确保每个通道都能通过。

**连接方式：**

1. **系统设置 → 声音 → 输出** → 选择 **BlackHole**（2ch 或 16ch）。此时系统与应用的声音
   都会流入 BlackHole。
2. 在 **D-Router** 中，将 **BlackHole** 添加为*输入*设备，把你的真实音频接口 / 音箱添加为
   *输出*设备，然后在矩阵中跨设备路由通道——可随意搭配插入效果、编组与逐交叉点增益。

> 由于 D-Router 自身会把 BlackHole → 你的音箱进行路由，你依旧能听到声音。（在初次设置时，
> 可在“音频 MIDI 设置”中创建一个同时包含 BlackHole 和你音箱的 macOS *多输出设备*，作为
> 一个方便的安全保障。）

## 构建

环境要求：**macOS 12+**、**CMake 3.22+** 以及 Xcode 命令行工具。依赖项会通过 CMake
`FetchContent` 自动获取——无需手动配置。

```bash
git clone https://github.com/<your-username>/dcorerouter.git
cd dcorerouter
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

应用会生成在：

```
build/dcorerouter_artefacts/Release/D-Router.app
```

`package.sh` 会对 app 包进行 ad-hoc 重新签名并打包为 zip 以便分发（注意：ad-hoc 签名并非
公证（notarization）——下载该 zip 的用户需要执行一次以下命令来清除 macOS 隔离标记：
`xattr -cr /path/to/D-Router.app`）。

## 架构

```
device callbacks ─► SPSC input rings ─► [matrix thread] ─► SPSC output rings ─► device output
                                              │
              input inserts → input groups → mix (crosspoints) →
              output inserts → output groups → post-fader gain
```

| 模块 | 位置 | 职责 |
|---|---|---|
| 引擎 | `Source/Engine/` | 设备工作线程、逐通道 SRC、实时 `MatrixProcessor`、环形缓冲、设置。 |
| 路由 | `Source/Routing/` | `RoutingMatrix`（网格 + 增益）、输出 / 输入编组管理器（联动推子）。 |
| DSP | `Source/DSP/` | 插件托管、`Builtin/` 内置插件套件 + 内部插件格式。 |
| UI | `Source/UI/` | `MatrixView`、单组件 `CrosspointGrid`、编组面板、状态 / 引擎监视器、对话框、自定义外观（look-and-feel）。 |
| 持久化 | `Source/Persistence/` | 树 / XML 快照、设置、崩溃保护。 |

## 许可

专有软件——© ZDAudio，保留所有权利。详见 [LICENSE](LICENSE)。

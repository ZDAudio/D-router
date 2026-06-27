# 系统默认输出/输入设备选择器 — 设计 (Spec)

**Date:** 2026-06-22
**Status:** Approved design, pre-implementation
**Scope:** macOS only (CoreAudio HAL). Local-only spec — NOT committed to the
public repo (same convention as the auto-update / resonance-suppressor specs).

## 目标

在 **Audio Setup tab** 直接切换 macOS 的**默认输出/输入设备**，免去每次去
System Settings → Sound（或菜单栏声音）里点选声卡的麻烦。

1. OUTPUT DEVICES 面板加一个 **"System Output"** 下拉；INPUT DEVICES 面板加一个
   **"System Input"** 下拉。
2. 下拉列出该方向上**系统里的全部设备**（不只是 D-Router 正在路由的那些）——
   是 macOS Sound 输出/输入选择器的完整替代（例如可在 BlackHole 与扬声器之间秒切）。
3. 选中某设备 → 用 CoreAudio HAL 把它设为 macOS 默认输出/输入设备。
4. 当前默认设备对应的 fader **strip 上显示 ★ 徽标**；若当前默认设备不在 D-Router
   的路由里，则无 strip 加星——下拉里仍正常选中显示。
5. 下拉选项与 ★ **跟踪外部改动**（用户从菜单栏 / Sound 设置改了默认设备）——复用
   现有 8 Hz 轮询 timer，约 ⅛ 秒内同步。
6. 设置系统默认设备**与 D-Router 自身的路由完全独立**：引擎按显式 device ID 打开
   设备，改默认设备**不会**重路由矩阵。（代码注释写明，防止后人误读。）

**非目标 (YAGNI):** 系统提示音输出设备
（`kAudioHardwarePropertyDefaultSystemOutputDevice`，即「警告声」单独出口）；
`AudioObjectAddPropertyListener` 事件监听（改用既有轮询，足够且更简单）；
记忆/自动切换默认设备；Windows。

## 决策（已与用户确认）

| 项 | 决定 |
|---|---|
| UI 形式 | **下拉列全部设备 + 当前默认 strip 上 ★ 徽标** |
| 范围 | **输出 + 输入（镜像）** —— tab 本就有上下两半 |
| 公共 helper | 新模块**自带小副本**（`deviceNameOf` / `hasStreams` / 枚举），**不动**已验证的 `DeviceVolume.cpp`；约 30 行良性重复，如愿可改抽 `CoreAudioDeviceUtil.h` |
| 同步方式 | 复用 `DeviceVolumePanel` 现有 8 Hz timer；列表只在 `rebuild()` 重建，timer 只更新「当前选中项 + ★」 |

## 架构与组件

### 新增 `Source/Engine/SystemAudioDevices.{h,cpp}`

仿 `DeviceVolume.h` 的「JUCE-free 风格」头（用 `juce::String` 但不含 CoreAudio
类型）。**仅消息线程**：每个调用都是到 `coreaudiod` 的 IPC，可能短暂阻塞——
头注释写明，**绝不**从矩阵线程或 CoreAudio IO 回调调用。

```cpp
namespace dcr
{
    struct AudioDeviceRef
    {
        juce::String name;
        unsigned int deviceID = 0; // AudioDeviceID（UInt32）；0 == 未知
    };

    class SystemAudioDevices
    {
    public:
        enum class Scope { Input, Output };

        // 该方向上「带 stream 的」全部设备（即可作为系统默认输出/输入的设备）。
        static juce::Array<AudioDeviceRef> list (Scope);

        // 当前系统默认设备（deviceID==0 / 空名表示无）。
        static AudioDeviceRef getDefault (Scope);

        // 按 AudioDeviceID 设为系统默认设备；成功返回 true。
        static bool setDefault (Scope, unsigned int deviceID);
    };
}
```

实现（`SystemAudioDevices.cpp`，CoreAudio）：
- `list(scope)`：读 `kAudioHardwarePropertyDevices`（system object）→ AudioDeviceID
  数组 → 过滤 `hasStreams(id, scope==Output?Output:Input)` → 读 `deviceNameOf(id)`。
  顺序保持系统返回的原始顺序（与菜单栏一致即可，不额外排序）。
- `getDefault(scope)`：读 `kAudioHardwarePropertyDefaultOutputDevice`（Output）或
  `kAudioHardwarePropertyDefaultInputDevice`（Input），scope `Global`、element
  `Main`，于 `kAudioObjectSystemObject` → 得 AudioDeviceID → `deviceNameOf`。
- `setDefault(scope, id)`：`AudioObjectSetPropertyData(kAudioObjectSystemObject,
  &addr, 0, nullptr, sizeof(AudioDeviceID), &id)`，addr 同上对应 selector。
- 私有匿名命名空间 helper：`deviceNameOf`、`hasStreams`、设备枚举——本文件自带
  小副本（与 `DeviceVolume.cpp` 的等价物重复约 30 行，已在「决策」里说明取舍）。

### 改 `Source/UI/DeviceVolumePanel.{h,cpp}`

**Panel 头部加一行**（在 `title` 之下、`viewport` 之上）：
- `juce::Label defaultLabel`（"System Output" / "System Input"）+
  `juce::ComboBox defaultCombo`。
- 平行数组 `juce::Array<AudioDeviceRef> defaultDevices`，combo item-id = 下标+1
  （JUCE 要求 item-id 非 0），**按 ID 而非名字**匹配选中项，避免重名歧义。

**`rebuild()`**（现已在 tab 打开时调用、且被 `pauseUpdates/resumeUpdates`
包在 reconfigure 之外）：
- 除重建 strips 外，调 `SystemAudioDevices::list(scope)` 填充 combo，
  按 `getDefault(scope)` 选中当前默认项，并刷新各 strip 的 ★。
- **列表只在这里重建**（tab 打开 / 设备增减），timer 不重建列表。

**combo `onChange`**：取选中项的 `deviceID` → `setDefault(scope, id)`；失败则
按真实当前默认值回退选中项（设备恰好被拔掉等）。设置成功后刷新 ★。

**timer `pull()`**（现有 8 Hz）：仅
- 读 `getDefault(scope)`，若与 combo 当前选中不一致则更新选中项；
- 据此更新「哪个 strip 显示 ★」。
- **下拉弹窗打开时跳过**选中项同步（`defaultCombo.isPopupActive()`，仿 fader 的
  `dragging` 守卫），免得抢用户正在操作的弹窗。
- 每 tick 仅 1 次额外 IPC（读默认设备），与现有按 strip 的音量/dB/mute 轮询同量级，
  开销可忽略。

**`Strip` 加 ★ 徽标**：
- 新增 `juce::Label starLabel`（"★"，暖色/金色，tooltip "System default
  output"/"…input"），置于 strip 顶部角落（`nameLabel` 区域一侧）。
- 新增 `void setIsDefault (bool)`：切换 `starLabel` 可见性。
- Panel 用「当前默认设备名」与各 strip 的设备名比对来点亮（名字匹配，沿用 app
  既有的按名模型）。

### 边界情形

- **当前默认设备不在路由里** → 无 strip 加星；combo 仍正常选中显示。
- **设备重名** → ★ 落在第一个同名 strip（与 app 按名模型一致；属罕见角落）。
- **选中途中设备被拔** → `setDefault` 失败，下一次 `pull()` 按真实默认值回退。
- **该方向无设备** → combo 置空并 disable（与现有 `emptyLabel` 一致）。
- **热插拔** → combo 列表在下次 `rebuild()`（tab 打开 / 设备变更）刷新；timer 期间
  只更新选中项，不动列表（避免抢正在打开的弹窗、也省无谓重建）。

## 线程 / 安全 / RT

- 全程**消息线程**。`SystemAudioDevices` 的三个调用都是到 `coreaudiod` 的 IPC，
  可能短暂阻塞——与 `DeviceVolume` 同源、同约束：**绝不**在矩阵线程 / CoreAudio
  IO 回调里调用。非 RT 域，无 RT-safety 约束。
- combo 填充走 `rebuild()`，已被 `pauseUpdates/resumeUpdates` 在 reconfigure 期间
  冻结；`SystemAudioDevices::list()` 本身不依赖引擎状态（直接问 CoreAudio），故即便
  独立调用也安全，但仍统一走既有受保护路径以保持一致。
- 改系统默认设备**不触碰** D-Router 引擎/路由（引擎按显式 ID 开设备），二者是不同
  层；注释写明。

## 测试

- 主体是 **CoreAudio IPC + JUCE UI**，**无法 headless 单测**——与 `DeviceVolume`
  本身一致；`dcorerouter_tests`（ctest）没有值得新增的纯逻辑单元。
- **真机手测**（我会 build + run）：
  1. 从 "System Output" 下拉选另一设备 → 确认 macOS 菜单栏/Sound 设置随之改变、
     且实际系统声音切到该设备。
  2. 从菜单栏/Sound 设置在外部改默认设备 → 确认下拉选中项与 ★ 在 ~⅛ 秒内跟上。
  3. "System Input" 同样验证一遍。
  4. 确认选系统默认**不影响** D-Router 自身矩阵路由。
- **CI**：clang-format 检查会因未格式化而 fail——提交前对改动跑 clang-format。

## 实现顺序（供 plan 参考）

1. `SystemAudioDevices.{h,cpp}` + 加入 `CMakeLists.txt`（`target_sources`）；先把
   CoreAudio 后端做扎实，可临时在某处 log `list()` 验证枚举正确。
2. `DeviceVolumePanel::Strip` 加 ★ 徽标 + `setIsDefault`。
3. `DeviceVolumePanel` 加 combo 行：`rebuild()` 填充 + 选中 + 点亮 ★；`resized()`
   排版（title 行 → combo 行 → viewport）。
4. combo `onChange` → `setDefault` + 回退；timer `pull()` 同步选中项 + ★（弹窗守卫）。
5. clang-format + build + 真机走查全流程。

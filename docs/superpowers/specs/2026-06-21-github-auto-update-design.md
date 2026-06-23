# GitHub 自愿自更新 — 设计 (Spec)

**Date:** 2026-06-21
**Status:** Approved design, pre-implementation
**Scope:** macOS only (the app is macOS/CoreAudio). Local-only spec — NOT committed
to the public repo (same convention as the resonance-suppressor spec).

## 目标

给 D-Router 加一个**自愿、非强制**的自更新功能：

1. 检测 `ZDAudio/D-router` 的 GitHub releases 上是否有比当前运行版本更新的版本。
2. 有则弹一个**可忽略的非阻塞提示**（新旧版本号 + release 说明 + 「Upgrade」/「稍后」）。
3. 用户**主动点 Upgrade** 才下载新版 `D-Router.zip`（带进度，可取消）。
4. 下载完成 → 真正退出 app → 用新版 `.app` 覆盖原位旧版 → 自动重启新版。
5. 不点 Upgrade 就**继续用旧版，完全正常**——不催、不挡、不强制。

**非目标 (YAGNI):** 增量/差分更新；「跳过此版本」；关闭自动检测的设置开关；加密
签名验证（appcast/EdDSA/notarization）；Windows/Linux；引入 Sparkle 或任何新框架。

## 决策（已确认）

| 项 | 决定 |
|---|---|
| 触发 | 启动后静默检测 **+** 菜单「Check for Updates…」手动检测 |
| 版本范围 | **最新 release 优先，含 pre-release**（GitHub `/releases/latest` 跳过 pre-release，故走 `/releases` 自己挑最新） |
| 强制性 | 纯自愿；提示可忽略；旧版不升级照常工作 |
| 升级后 | 覆盖完成后**自动重启**新版 |
| 实现 | 手写（GitHub API + `juce::URL`/`juce::JSON` + 脱离进程 shell 脚本），零新依赖 |

## 架构与组件（新增 `Source/Update/`）

### `Version.h` — JUCE-free 版本逻辑（可单测）
- 解析 tag：`v?MAJOR.MINOR.PATCH(-PRERELEASE)?` → `{int major, minor, patch; std::string prerelease}`。
- 比较规则：先比 major.minor.patch（数值）；相等时**有 prerelease < 无 prerelease**
  （`0.2.0-beta` < `0.2.0`）；两边都有 prerelease 则按字符串比较。
- `isNewer(candidate, current)`：candidate 严格大于 current 才返回 true。
- **健壮性**：任一侧解析失败 → 返回「不更新」（绝不因解析失败而误弹更新）。
- 纯逻辑、无 JUCE → 加入 `dcorerouter_tests`（ctest）。

### `UpdateChecker` — 拉取并比较
- 在**后台线程**（`juce::Thread` 或一次性 job）执行，绝不阻塞消息线程。
- `juce::URL ("https://api.github.com/repos/ZDAudio/D-router/releases")`，
  `withParameter` `per_page=20`；`WebInputStream` 带 header：
  `User-Agent: D-Router-Updater`、`Accept: application/vnd.github+json`；连接超时 ~10s。
- `juce::JSON::parse` → 遍历 releases 数组：跳过 `draft==true`；按 `published_at`
  降序取**第一条**（**含 prerelease**）作为候选版本。
- 在该 release 的 `assets` 里找 `name == "D-Router.zip"` → 取 `browser_download_url`
  与 `size`。无此资产则视为「无可用更新」。
- 结果 `ReleaseInfo { String tag, version, name, notes; juce::URL zipUrl; int64 zipSize; }`
  经 `juce::MessageManager::callAsync` 回主线程交给回调。
- 失败（无网络/超时/解析失败/无资产）：自动检测时**静默**；手动检测时回报
  「无法检查」或「已是最新」。
- 公共仓库**免 token**；启动 + 手动检测远在未认证 60 次/小时限额内。

### `UpdateInstaller` — 下载 + 自替换
- `downloadAsync(ReleaseInfo, onProgress, onDone)`：后台线程把 zip 下到
  `~/Library/Caches/D-Router/update/D-Router.zip`，回报进度；按 `zipSize` 校验完整性。
- 解压：`ditto -x -k <zip> <stagingDir>` → 在 staging 找到新 `D-Router.app`。
- 定位当前 bundle：`juce::File::getSpecialLocation (juce::File::currentApplicationFile)`
  （macOS 上即 `.app`）。
- **退出前预检**（见下「边界」）：父目录可写、非 App Translocation。
- 写脱离进程 swap 脚本（见下）→ `chmod +x` → 以脱离方式启动
  （`/bin/sh -c "nohup bash <script> >/dev/null 2>&1 &"`，确保父进程退出后仍存活）
  → 调用真正的 `quit()`（不是隐藏到托盘）。

### `UpdatePrompt` — 非阻塞提示（仿 `MainComponent::showAboutDialog`）
- `juce::DialogWindow::LaunchOptions` + `launchAsync()`（模态less，可关闭忽略）。
- 内容：当前版本 → 新版本、release 说明（可滚动）、`[Upgrade]` `[稍后]`。
- 点 Upgrade：原地切换为进度条 + 「Downloading… X%」+ `[取消]`；完成后短暂
  「Restarting…」即触发退出+替换。

### 接入点
- `Source/Main.cpp`：`DcoreRouterApp` 持有 `UpdateChecker`；`initialise()` 末尾起一个
  一次性 `Timer`（~3s），到点静默 `checkAsync`；有更新则 `UpdatePrompt`。
- `Source/MainComponent.cpp`：Apple（应用名）菜单在「About D-Router」旁加
  「Check for Updates…」（新 id，如 `kMenuCheckUpdatesId = 1501`）→ 手动检测：
  显示检测中 → 弹提示或「已是最新版本」。

## 自替换脚本（关键）+ 边界

macOS 无法可靠覆盖**运行中**的 `.app`，故用脱离进程脚本，等本进程退出后再换：

```bash
#!/bin/bash
PID=<oldpid>; OLD="<old .app 绝对路径>"; NEW="<staging 里的新 .app>"
STAGING="<staging dir>"; ZIP="<下载的 zip>"
while kill -0 "$PID" 2>/dev/null; do sleep 0.2; done   # 等旧进程完全退出
sleep 0.3
xattr -cr "$NEW"                                        # 清下载隔离标记
if ditto "$NEW" "$OLD.new"; then                        # 先拷到旁边（旧版此刻仍完好）
  rm -rf "$OLD" && mv "$OLD.new" "$OLD" && xattr -cr "$OLD"
fi
open "$OLD"                                             # 重启（替换成功=新版，失败=旧版仍在）
rm -rf "$STAGING" "$ZIP" "$0"                           # 清理自身与暂存
```

**已处理边界：**
- **目标不可写**（如无权限的 `/Applications`）：退出前预检父目录可写性；不可写则
  **不退出**，改为打开 GitHub releases 页 + 提示手动安装，旧版保持运行。
- **App Translocation**（未签名 app 被 Gatekeeper 从随机只读路径运行）：`currentApplicationFile`
  路径含 `/AppTranslocation/` 时检测到 → 提示「请先把 D-Router 拖到 /Applications 再更新」，
  不执行替换。
- **下载不全/中断**：按 `zipSize` 校验；不符则清理、保留旧版、报错。
- **替换失败回退**：脚本先 `ditto` 到 `$OLD.new` 成功后才删旧版；失败则旧版原封不动，
  `open` 重开旧版——任何路径下总有一个可用的 app。

## 线程 / 安全 / RT

- 全部在**消息线程 + 一个后台网络线程**；**完全不触碰音频/矩阵线程**（非 RT 域，
  无 RT-safety 约束）。UI 更新一律经 `MessageManager::callAsync`。
- 退出走真正的 `quit()` → `shutdown()` 正常释放音频设备、保存状态；脚本等 PID 退出
  后才替换，故设备已释放、无文件占用。
- 传输：HTTPS（`api.github.com` / `objects.githubusercontent.com`）+ 大小校验。
  **无加密签名验证**——与本 app 当前 ad-hoc/未公证的信任模型一致（收件人本就信任该
  zip）。引入真正校验需签名基建，超出范围（与现有手动分发相同的已知缺口）。

## 版本单一来源（顺带修）

`Main.cpp::getApplicationVersion()` 现硬编码 `"0.1.0"`，而 About 对话框用
`JUCE_APPLICATION_VERSION_STRING`（来自 CMake `project VERSION`）。统一成
`getApplicationVersion()` 返回 `JUCE_APPLICATION_VERSION_STRING`，使**版本只有一个来源**
（CMake），保证更新器比较的「当前版本」永远正确、且与发布 tag 对齐。

## 依赖前提

每个 GitHub release 必须附带 `package.sh` 产出的 `D-Router.zip` 资产；更新器据此
名称定位下载。（正好接上待办的发布流程：发布时附上该 zip。）

## 测试

- **单测（ctest，`dcorerouter_tests`）**：`Version` 比较器 —
  `1.2.0 < 1.10.0`、`0.2.0-beta < 0.2.0`、`v` 前缀容忍、相等不更新、非法输入→不更新、
  `isNewer` 各组合。
- **真机手测**（JUCE/网络/文件替换无法 headless）：指向一个测试 release，跑通
  检测→提示→下载→替换→重启；swap 脚本可拿两个 app 副本单独验证；验证
  「忽略提示后旧版照常用」「目标不可写时的降级路径」。

## 实现顺序（供 plan 参考）

1. `Version.h` + ctest（先把可测的纯逻辑做扎实）。
2. 版本单一来源（`getApplicationVersion`）。
3. `UpdateChecker`（拉取/解析/比较）+ 手动菜单项（先只 log/弹提示，不下载）。
4. `UpdatePrompt` UI。
5. `UpdateInstaller`（下载+进度）。
6. swap 脚本 + 退出/重启 + 边界处理。
7. 启动静默检测接线。
8. 真机走查全流程。

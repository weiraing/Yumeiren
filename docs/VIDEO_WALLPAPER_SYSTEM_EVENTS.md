# 视频壁纸系统事件处理（阶段 4 交付物）

对应实现：`src/mainwindow.cpp`（nativeEvent 消息泵）、`src/videowallpaper.cpp`（evaluateSuspend 心跳）、`src/platform/windows/desktopmount.cpp`（Win32 探测）。所有事件最终汇入状态机的 `evaluateSuspend()`（1s 心跳）与 `scheduleRelayout()/scheduleMountFix()`，事件源与处理入口一一对应如下。

## 1. 事件来源 → 处理入口 → 状态转换 → 资源操作

| 事件 | 来源 | 入口 | 状态转换 | 资源操作 |
|---|---|---|---|---|
| 显示器关闭/打开 | `WM_POWERBROADCAST` + `PBT_POWERSETTINGCHANGE`(GUID_MONITOR_POWER_ON)，构造时 `RegisterPowerSettingNotification` 注册 | `MainWindow::nativeEvent` → `VideoWallpaper::setMonitorOn(bool)` | monitorOn=false → `SuspendMonitorOff`；恢复后自动续播 | 暂停时停止解码泵送；持续 180s 触发 `longSuspendRelease()` 整管卸载 |
| 系统睡眠 | `PBT_APMSUSPEND` | 同上（按 monitorOn=false 处理） | 同熄屏 | 同上（睡眠期间管线已被释放或暂停） |
| 系统唤醒 | `PBT_APMRESUMEAUTOMATIC` / `PBT_APMRESUMESUSPEND` | 同上（monitorOn=true） | 挂起原因消失 → evaluateSuspend 自动续播；若长挂起已释放则重建管线并跳回 `m_resumePosMs` | 重建路径=playIndex→ensureOutputs（无多余创建） |
| 锁屏/解锁 | `OpenInputDesktop` 失败=锁定（desktopmount.cpp） | evaluateSuspend 心跳（1s） | `SuspendLocked` | 同熄屏；解锁后 reasons 清零自动恢复 |
| 前台全屏应用 | `GetForegroundWindow`+物理像素比对（±2px 容差，排除自身与 explorer） | evaluateSuspend 心跳 | `SuspendFullscreen`（开关 `video/pauseFullscreen`，默认开） | 暂停；退出全屏 1s 内恢复 |
| 桌面被完全遮挡 | 前台窗口覆盖主屏工作区 | evaluateSuspend 心跳 | `SuspendCovered`（仅主屏模式） | 同上 |
| 电池/电源 | `GetSystemPowerStatus`（ACLineStatus） | evaluateSuspend 心跳 | `SuspendBattery`（开关默认关） | 同上 |
| 分辨率变化 / DPI 变化 | `QScreen::geometryChanged`（DPI 变化会连发，防抖 600ms 合并） | `scheduleRelayout()` | Rebuilding(瞬态) | `layoutOutputs()`（teardown+重建）+ playIndex(m_index) |
| 显示器热插拔 | `QGuiApplication::screenAdded/Removed` | 同上 | 同上 | 同上 |
| Explorer 重启 / 挂载失联 | `isWindowMounted`/`hasRealWorker` 检查（心跳内） | `scheduleMountFix()`（节流 10s） | 保持播放状态 | `remountOutputs()`：仅重挂载，不重建解码管线 |
| 多屏主副屏切换 | UI 下拉 → `setScreenMode` | 值不变直接返回 | Rebuilding | layoutOutputs 重建；MirrorAll 每屏一套输出 |

## 2. 验证矩阵与实测结果

| 场景 | 预期 | 实测 | 证据 |
|---|---|---|---|
| 播放中熄屏 | 暂停，不持续解码 | ✅ **真实事件验证**：无人值守批次中系统空闲触发熄屏，实测暂停→30s 回收→180s 整管释放（Private 365→93MB、GPU→0、句柄回落） | `tools/perf/results/inv_reload.csv` t≈25–207s（docs/perf/memory-attribution.md 实验五"意外收获"） |
| 熄屏后回亮恢复 | 自动续播 | ✅ 同一事件序列：AutoStart 请求播放后管线重建并出画 | 同上（t≈333s 起 GPU 恢复） |
| 长挂起释放后重建不叠加 | 重建后 ≤ 首次基线 | ✅ 275MB < 首次 365MB | 同上 |
| 睡眠/唤醒 | 走熄屏同路径（PBT_APMSUSPEND→monitorOn=false） | ⚠️ 推断（与熄屏同一代码路径，该路径已被真实事件验证）；主动睡眠/唤醒需人工确认一次 | 代码路径 mainwindow.cpp:1511-1527 |
| 播放中锁屏 / 解锁恢复 | 暂停 / 恢复，不重复创建 | ⚠️ 探测逻辑实证（基线 S6 系列验证 OpenInputDesktop 判定），自动化锁屏需输入密码，留人工清单 | 手工清单 tests/video_wallpaper/README.md |
| 快速重复锁屏解锁 | 幂等（reasons 位掩码去重，无重复创建） | ⚠️ 代码审查：evaluateSuspend 每 1s 重算 reasons，播放中重复暂停/恢复不创建对象；留人工 | 同上 |
| Explorer 重启 | ≤10s 重挂载 | ⚠️ 挂载健康检查已实证（上一阶段行为验证 63s+）；真实重启 explorer 留人工（侵入用户会话） | 手工清单 |
| 拔出/接入副显示器、改主屏分辨率、改缩放 | 重建布局，无黑屏/残留 | ⚠️ 单屏机器无法复现多屏场景（见 MULTI_MONITOR_VIDEO_RESOURCE_ANALYSIS.md）；单屏改分辨率/缩放走同一 scheduleRelayout 路径，编译运行无回归 | 手工清单（双屏） |
| 全屏应用暂停/恢复 | GPU→0，退出后 <1s 恢复 | ✅ 上一阶段 S6'' 实测 | docs/perf/perf-comparison.md |

## 3. 已知限制

1. **Progman 兜底挂载**在部分 Win11 构建上不被 DWM 合成——检测到兜底状态时由 10s 节流持续重查，真正的 WorkerW 出现后自动迁入（desktopmount.cpp 注释）。
2. `ensureWorker` 在 GUI线程上轮询（首次最坏 ~3s、复查 ~0.7s）——上一阶段已压缩（重试 30→10、超时 1s→300ms）。
3. 睡眠唤醒后若 FFmpeg/D3D 设备被系统重置，首次重建可能多花 1–2s（与正常起播同量级）；无自动重试必要，人工验证清单覆盖。
4. 双屏相关场景在本机（单显示器）无法自动化，统一列为人工清单项。

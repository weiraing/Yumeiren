## 视频壁纸功能回归报告

本文对应优化任务书第二十章要求的 `video_performance_regression.md`：逐项回答「这次改动有没有动到既有功能」。判定口径：

- **通过** = 本机有实测证据，且行为与改动前一致；
- **不受影响** = 相关代码路径未被本任务改动，且有可核对的佐证；
- **未测** = 本机没有条件跑，不借用其他机器或历史数字。

数据来源 `new-build/perf-results/`（`r_*.csv` + 同名 `.log`）。采样与分段口径见 `video_performance_test.md` §1、§3，长稳分段结论见其 §9 与 `video_resource_lifecycle.md` §11.5。

## 0. 回归范围：本任务改了哪些代码

| 文件 | 改动 | 可能的功能外溢面 |
|---|---|---|
| [src/platform/windows/desktopmount.cpp](C:\Users\rain\Documents\ExplorerBg\Yumeiren\src\platform\windows\desktopmount.cpp:242) / `.h` | 新增 `distinctCoreAffinity()`；`applyProcessAffinityLimit()` 由「前 N 个逻辑号」改为「N 个不同物理核各取一个逻辑号」，失败回退旧掩码 | **进程级** CPU 亲和性，理论上作用于本进程全部模块 |
| [src/main.cpp](C:\Users\rain\Documents\ExplorerBg\Yumeiren\src\main.cpp:39) | 限核生效时的日志措辞与注释 | 无 |
| [src/videowallpaper.cpp](C:\Users\rain\Documents\ExplorerBg\Yumeiren\src\videowallpaper.cpp:190) | 新增 15 行 `YUMEIREN_AUTO_PAUSE_MS` 环境变量钩子（仅供自动化暂停/恢复取证） | 不设置该变量时不创建定时器，零开销；生产环境无人设置 |
| [src/mainwindow.cpp](C:\Users\rain\Documents\ExplorerBg\Yumeiren\src\mainwindow.cpp:1074) | 「资源友好模式」tooltip 文案补一句物理核说明 | 仅 UI 文本 |
| `CMakeLists.txt` | `SinkProbe` 目标改为「探针源码存在才创建」 | 构建目标，不改产品二进制 |

工作区里另有 `src/core/CachePaths.*`、`engine.*`、`appinfo.cpp`、`videodiag.cpp`、`mainwindow.cpp`（缩略图落点、`stale` 状态灯、`reportDllMigration()`）的改动，属于上一个交付项「缓存目录迁移」，本任务未触碰；本文只在 §7、§8 复核它们与视频壁纸的交界处，完整回归见 `cache_migration_test.md`。

## 1. 图片壁纸回归

| 检查项 | 结论 | 证据 |
|---|---|---|
| 图片处理 / 背景生成代码 | 不受影响 | `git diff --name-only -- src` 命中 11 个文件，不含 `imageprocess.cpp` / `imageprocess.h`（图片解码、缩放、背景图生成的唯一实现），也不含 `Engine::applyImage` 所在的注册/挂载分支 |
| 图片壁纸缩略图链路 | 不受影响（本任务） | `MainWindow::galleryThumbPath()` 的改动来自缓存迁移任务；本任务未改缩略图键名、尺寸与失效逻辑 |
| 共享面：进程亲和性 | 通过（推理 + 构建） | 逻辑核**数量**不变（仍是 4），只换掩码形状：旧 `0b1111` = 物理核 0、1 各占 2 个 SMT 线程；新掩码 = 物理核 0/1/2/3 各 1 个线程。`QThread::idealThreadCount` 不变 → 任何按核数分支的代码（含 Qt 图像线程池）行为一致，而可用吞吐上升 |
| 图片壁纸应用 / 取消 / 换图端到端 | **未测** | 该流程需要 Explorer Shell Hook 重新注册（管理员权限、会重启 Explorer），本机当前状态为「图片 未启用、特效 未启用」（15:48:55.262 Engine 自检日志）。刻意不做：一旦打断 Explorer 就会污染同时在跑的视频测量，且现场不可复现 |
| 主界面启动 / 退出冒烟 | 通过 | 15:48:54.365 一次人工启动（pid 13928，diag=off，`wasPlaying=false` 未起播视频），主窗口 1 132 ms 内完成首次显示；15:50:53.933 正常退出，`players=0/0 widgets=0/0 audios=0/0`，`exit=0`，退出收口 12 ms |

## 2. 视频壁纸回归（起播 / 暂停 / 停止 / 退出）

| 检查项 | 结论 | 实测证据 |
|---|---|---|
| 配置驱动的自动起播 | 通过 | `r_soak30_1080p30` 会话 12:52:40.823 读到 `keys=43 wasPlaying=true`，随后 2 423 条快照 `state=1` 恒定，无一次落到错误态 |
| 暂停 / 恢复 ×100 以上 | 通过 | `r_pause100`（12:37:13.510 pid 12376 → 12:43:00.9）实测 **345 次** `pauseResume()` 切换（每 1 s 一次，快照 `state` 在 1/2 间交替），`players=1/1 widgets=1/1 audios=1/1` 全程不变，**0 条 `[ERROR]`、0 条 `[WARN]`**；线程 93 → 83、句柄 2 967 → 2 954、Private 全段均值 307.0（276.9–410.7，drift +6.2）→ 无累积 |
| 暂停期间是否仍在渲染 | 通过 | `v_pause_cycle` 暂停段 GPU 总占用 ≤ 0.06 %、3D ≈ 0、`VideoDecode` 无样本；`r_pause100` 暂停区间 cpu 掉到 0–0.04 → 暂停是真停，不是遮窗口 |
| 停止后是否真释放 | 通过 | `r_reload`：40 s 时 `stopAll()`，Private 332 → **96.9**（46.4 s 处），线程 → 18，句柄 → 1 444，GPU 0.00；60 s 重启回到 263–332；退出 `players=2/2 widgets=2/2 audios=2/2`（两条管线都建过、也都没了） |
| 停止后的残留水位 | 通过（含已知残留） | `r_stopresidual`：停止后 2 s 194.9 → 90 s 93.0 → 130 s **92.9** MB，线程 92 → 6，句柄 2 964 → 1 408。相对 `b_idle` 69.3 仍有 **+23.6 MB** 残留，属 DLL / 堆级，无播放器对象残留（`players=1/1`），与改动前同源、本任务未新增 |
| 退出收口 | 通过 | 6 条回归用例全部 `CloseMainWindow()` 正常关窗、`exit=0`；收口耗时 96 ms（soak30）/ 325 ms（soak10）/ 12 ms（15:48 人工启动） |
| 播放期不新增定时器 | 通过 | `videowallpaper.cpp` 仅新增「显式设置环境变量时才创建」的 toggle 定时器；`evaluateSuspend`(1 000 ms) 与 `trimMemory`(30 000 ms) 两个既有空转定时器本任务未改，其成本见 `video_performance_audit.md` §3 |

### 2.1 真实发布二进制 + 用户片源的人工操作（16:49 会话）

上面所有数字都来自脚本驱动的 `YumeirenTest.exe`。16:49:05 用户在本机手工启动了**发布目标** `Yumeiren.exe`（pid 9908，`diag=on`），并手动点了起播 / 换曲 / 停止，这条会话不在任何矩阵里，属于意外的免费对照，记录如下：

| 观测 | 值 |
|---|---|
| 新掩码是否生效 | `16:49:05.866 资源友好模式: 进程已限制到 4 个逻辑核(尽量分属不同物理核)`（本任务改动的唯一运行期日志差异） |
| 冷启动 | 主窗口首次显示 2 338 ms，配置加载 2 ms，`Hook DLL 目录` 自检正常打印 |
| 换源 | `session=1 → 2 → 3`，三条真实素材：`s3_2160p60_av1.mp4`（4K60 AV1）→ `s3_1080p30_mpeg4.mp4` → `凡人修仙传 紫灵.mp4`（4K60 H.264 约 101 Mbps） |
| 播放器复用 | 三次换源期间**无**卸载事件；首次 `stopAll` 记 `n=1 累计 players=1/1`，第二次记 `n=1 累计 players=2/2` → 换源复用管线，停止 + 重新起播才新建，计数收支相抵 |
| 稳定性 | 194 行日志 / 102 条快照，**0 条 `[ERROR]`、0 条 `[WARN]`**，末态 `UI状态 已停止`，进程仍在正常运行 |
| 配置回写 | 起播后 `wasPlaying` 由 `false` 变 `true`（键数仍 43），见 §7 |

## 3. 单视频循环回归

素材 `f_1080p30.mp4` / `f_2160p60.mp4`，均 10 s，`PlayMode=0`（单视频无限循环），`setLoops(Infinite)` 未改。

| 用例 | 时长 | 循环回绕 | 错误 | 退出 |
|---|---|---|---|---|
| `r_soak30_1080p30` | 40.4 min（uptime 2 423 786 ms） | **242 次** | 0 | `players=1/1 widgets=1/1 audios=1/1` |
| `r_soak10_4k60` | 14.5 min（uptime 869 044 ms） | **86 次** | 0 | 同上 |

- 328 次回绕期间：快照 `finished=1` 出现 **0 次**（回绕由多媒体后端内部完成，应用层未参与，因此不会走「停止 → 重建」路径）；播放器对象指针全程唯一（`0x1a78f504b40` / `0x23c84d64740`）。
- 循环边界不抬水位：soak30 重绑前 445 s 内 44 次回绕，Private 恒在 274.9–357.0、Dedicated 恒 149.1（详见 `video_performance_test.md` §9.3）。
- **未测**：循环边界是否出现黑帧 / 桌面背景闪现。这属于逐帧呈现观测，本机未装 PresentMon，也未做屏幕采样；只有「回绕期间 `state` 不变、无 WARN/ERROR」这一间接证据。
- **未覆盖**：5 s / 1 min / 10 min 素材的分档循环（本轮只有 10 s 一档）、有音轨循环（§5）。

## 4. 视频切换回归

`r_switch`：播放列表写入 `f_1080p15 / f_1080p24 / f_1080p60` 三条同分辨率不同帧率素材，`PlayMode=1`（列表循环），故意排除分辨率变量。

| 指标 | 实测 | 说明 |
|---|---|---|
| 换源次数 | **23**（`session=1…24`，`play index=1/3 → 3/3` 完整 8 圈） | 首条 12:46:08.904，末条 12:50:03.089 |
| 播放器数量 | `players` 全程 `1/1` | 复用同一实例，未每次切换新建 |
| 播放器指针 | 唯一 `0x1b5dc6a4900` | 同上，指针级证据 |
| 显存池 | Dedicated 136.5–149.6，均值 **140.8** | 会话内无重绑，是 1080p 干净稳态值（此点亦是 `video_performance_test.md` §9.1 勘误的反证） |
| 内存漂移 | Private drift **−2.5 MB**（27 点） | 23 次换源无累积 |
| 线程 / 句柄 | 88.7 均值 / 2 968 均值 | 与单视频播放同档 |
| 错误 | 0 | 含首尾帧定位、进度回调、`resumePos=-1` 正常 |

**部分覆盖**：「删除视频」「修改播放列表」通过每例改写 `config/.ini` 播放列表间接覆盖（换源前必经一次重载），未在 UI 上点删；**未测**：跨分辨率切换（本轮三条同为 1080p）与切换过程中的黑帧观测。

跨分辨率与跨编码的换源改由 §2.1 那条人工会话覆盖：4K60 AV1 → 1080p30 MPEG-4 → 4K60 H.264 原片，三次起播共用一条管线、零错误，与「切换复用播放器」的设计一致。仍未测的是 UI 内点删视频，以及切换瞬间的黑帧观测。

## 5. 音频回归

| 检查项 | 结论 | 证据 |
|---|---|---|
| 音频策略代码 | 不受影响 | 本任务在 `videowallpaper.cpp` 只新增暂停钩子。`applyAudioPolicy()`（[:1089](C:\Users\rain\Documents\ExplorerBg\Yumeiren\src\videowallpaper.cpp:1089) `setActiveAudioTrack(carriesAudio && m_volume > 0 ? 0 : -1)`）、`setVolume()`（:1067）、副本输出静音（:328 / :715 / :716）逐行未变 |
| 音量 0 时不占音轨 | 通过（沿用既有实现） | 默认 `volume=0` → 轨道置 `-1`；本轮全部用例 Private / 句柄水位与历史无音频对照组一致 |
| 音频对象生命周期 | 通过 | 6 条回归用例退出收口一律 `audios=N/N`（正常 1/1，`r_reload` 停止 + 重启为 2/2），无未释放计数 |
| 音量调节 / 静音恢复 | **未测** | 本轮回归素材（`f_*.mp4` 阶梯）全部故意不带音轨，避免音频变量污染帧率阶梯；含 AAC 的 `s9_1080p30_aac.mp4` 未编进本轮矩阵。有音轨播放的实际出声、音量条拖动、静音→恢复需人工复听验证 |
| 多屏音频唯一性 | **未测** | 见 §6，本机单屏 |

## 6. 多屏回归

**本机无法实测，不给任何数字。** 这台机器只有一块 2560×1600 内置屏，全部日志里 `screens()` 恒为 1，无 HDMI / DP 输出可接。

可核对的代码事实（本任务未触碰）：

| 位置 | 内容 | 多屏含义 |
|---|---|---|
| `videowallpaper.cpp:208` / `:307` | 遍历 `QGuiApplication::screens()` 建立输出 | 每屏一条输出 |
| `:499` | `MirrorAll: one player per screen, first carries the audio` | 镜像模式下播放器数量 = 屏数 |
| `:264` / `:355` / `:871` | 只有首个输出推进列表与报错 | 副本不重复推进播放列表 |

本任务改的是「用哪几个逻辑核」，不改管线数量、不改每屏缓冲大小，因此不引入新的多屏风险；但镜像模式下每屏各一套解码管线，意味着限核省下的线程数会被屏数部分抵消，这条推断待双机实测。同样**未测**：插拔显示器、改主显示器、不同 DPI / 竖屏、显示器休眠唤醒（`scheduleRelayout()` 全量重建路径本任务未改）。

## 7. 配置读写回归

| 检查项 | 结论 | 证据 |
|---|---|---|
| 配置文件位置 | 不受影响 | 仍为 `<程序目录>/config/.ini`：日志 `path=C:/Users/rain/Documents/ExplorerBg/Yumeiren/new-build/config/.ini`。本任务未改键名、路径或迁移逻辑（任务书禁止项「修改配置文件路径」） |
| 读取 | 通过 | 15:48:54.366 `配置读取: exists=1 size=733 keys=43 wasPlaying=false`；播种会话 12:52:40.823 `size=815 keys=43 wasPlaying=true` → 续播分支按预期生效（§2 第 1 行） |
| 写入不丢键 | 通过 | 跨全部用例与两次人工启动，`keys` 恒为 **43**；内容不同（967 B / 815 B / 733 B）而键数不变 |
| `video/affinityLimit` 开关 | 通过 | `main.cpp` 仍按 `AppConfig::instance().value(ConfigKeys::Video::AffinityLimit, true)` 读取；`v_nolimit_1080p30` / `v_nolimit_4k60` 通过写 `affinityLimit=false` 关闭限核，线程 101.4 / 102.0 vs 开启时 90.4 / 91.4 → 配置项实测有效 |
| UI 勾选「资源友好模式」 | 通过（文案除外） | 仅 tooltip 文案变化（`mainwindow.cpp:1074`），控件、存储键、重启生效语义不变 |
| 用户现场还原 | 通过 | 测量结束后 `new-build/config/.ini` 从 `tools/perf/user_config_original.ini` 还原：逐行 `Compare-Object` 差异 **0**，两文件 SHA256 同为 `818969FB6EBC…`（967 B） |

## 8. 缓存路径回归

| 检查项 | 结论 | 证据 |
|---|---|---|
| 本任务是否改缓存路径 | 未改 | 本任务源码改动（§0 表）不含任何路径拼接；`.cache` 相关改动全部属于上一交付项 |
| 四个缓存子目录 | 通过 | `new-build/.cache/` 下实测存在 `logs`、`media`、`temp`、`thumbnails` |
| 诊断日志落点 | 通过 | 实测 `new-build/.cache/logs/videowallpaper.log`；超 1 MB 轮转到 `.log.old`（本轮切点 13:45:20，旧段 1 048 693 B） |
| 旧位置 | 通过 | `Test-Path %LOCALAPPDATA%\Yumeiren` = **False** |
| 缓存增长 | 通过（有量级） | 40.4 min 长稳产生日志约 922 KB（含轮转），按 `DEBUG` 级只在 `YUMEIREN_DIAG=on` 时开启；生产默认 `diag=off` 时同一会话日志仅 163 行 |
| `<程序目录>\dll` 未创建 | 非本任务引入 | 13:33:08.901 起每条日志都打印 `Hook DLL 目录 …\new-build\dll`，但该目录 `Test-Path` = False：`Engine::ensureDataDirs()` 只在注册 / 应用入口调用，不在启动路径，且本机图片与特效均未启用 → 既有行为 |
| 只读安装目录（`C:\Program Files`） | **未测** | `CachePaths::ensureDirectories()` 失败时报错不回退 AppData（上一交付项实现），本机未做只读目录安装验证 |

## 9. 发布包回归

| 检查项 | 结论 | 证据 |
|---|---|---|
| Qt 组件依赖 | 不变 | `find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)` 未改动；两个产品目标的 `target_link_libraries` 未新增组件（`CMakeLists.txt` 的 diff 只有源文件列表加 `src/core/CachePaths.cpp` 与 `SinkProbe` 条件化） |
| Win32 依赖 | 不变 | 新逻辑用 `GetProcessAffinityMask` / `SetProcessAffinityMask` / `GetLogicalProcessorInformation`，均为 `kernel32` 既有导出，与同文件已用的 `dwmapi` / `psapi` 一样不需要额外部署件 |
| 多媒体后端插件 | 不变 | 未新增 / 删除 FFmpeg、WMF 插件；默认后端仍是 FFmpeg（`v_wmf_*` 只用环境变量临时切换） |
| 构建 | 通过 | `cmake --build new-build --target Yumeiren YumeirenTest` → `ninja: no work to do`（无待编译改动）；`Yumeiren.exe` 1 385 183 B、`YumeirenTest.exe` 1 383 117 B，同为 12:15:49 |
| 构建目标健壮性 | 通过 | `tools/` 被 `.gitignore` 忽略，干净克隆时 `SinkProbe` 源码不存在 → 该目标跳过，产品目标照常配置；此前会在 `qt_add_executable` 阶段报错 |
| windeployqt 产物清单 | **未重跑** | 本轮未执行部署脚本，故不声明产物 diff；依据上面两行「零新增依赖」推断产物清单不变，属**推断**而非实测 |
| 新增运行时开关 | 无 | 未加配置文件项；`YUMEIREN_AUTO_PAUSE_MS` 是仅测试用的环境变量，生产不设置 |

## 10. 结论汇总

| 回归项 | 结论 |
|---|---|
| 图片壁纸代码 | 不受影响（应用流程未测） |
| 视频播放 / 暂停 / 停止 / 退出 | 通过 |
| 单视频循环（328 次） | 通过（黑帧未观测） |
| 视频切换（23 次） | 通过 |
| 音频对象生命周期 | 通过（有音轨出声未测） |
| 多屏 | 未测（本机单屏） |
| 配置读写 | 通过 |
| 缓存路径 | 通过（只读目录未测） |
| 发布包依赖 | 通过（windeployqt 未重跑） |

一句话：**没有任何一条回归证据显示既有功能被本次改动破坏**；未测项集中在「需要第二块屏」「需要重启 Explorer」「需要人耳复听」三类本机不可复现的场景。

## 11. 遗留与复测建议

1. 有音轨素材的出声 / 音量 / 静音回归：`s9_1080p30_aac.mp4` 编进 `run_regression.ps1` 的 short 组，人工复听 30 s。
2. 双屏镜像 + 扩展各跑一次 `r_soak30_1080p30`，比对 `players/widgets/audios` 与屏数的关系，替换 §6 的推断。
3. 帧级采集（PresentMon）后补做循环边界与切换边界的黑帧 / 丢帧观测，同时定性 `video_performance_test.md` §9.4 的 4K60 掉帧推断与 490 s 提交内存台阶。
4. 桌面窗口重绑的触发者（4 次全落在 :00/:30、跨 3 个进程会话、explorer 未重启）仍未定位；在定位前，任何跨重绑的均值都不可当作稳态引用。
5. 交付时注意 `.gitignore` 忽略了 `dist/ tools/ new-build scripts tests docs .cache/`，本文与 `tools/perf/` 工装需 `git add -f docs tools` 才会入库。

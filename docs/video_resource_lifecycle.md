# 视频壁纸资源生命周期（Yumeiren）

对象：`VideoWallpaper`（`src/videowallpaper.cpp`，下文行号均以该文件为准）。窗口/挂载与系统探测在 `src/platform/windows/desktopmount.cpp`（命名空间 `fbswin`）。全部视频对象只在 **GUI 线程**创建与销毁，关键入口有 `VW_ASSERT_GUI()` 断言（`:34`）。

## 1. 受管对象清单

| 对象 | 所有权 | 数量规则 |
|---|---|---|
| `QMediaPlayer` | `VideoOutput::player`，父对象为单例 | 每个输出 1 个 |
| `QAudioOutput` | `VideoOutput::audio`，父对象为单例 | 每个输出 1 个 |
| `QVideoWidget`（原生 HWND + 呈现面） | `VideoOutput::widget`，无父对象，由 `m_outputs` 独占 | 每个输出 1 个 |
| 心跳定时器 ×2 | 单例子对象 | 常驻，只在播放期间 start |
| 探针对象三件套 | 单例成员 | 仅 `YUMEIREN_PROBE_STAGE` 时创建 |

「输出」数量由显示模式决定（`:492-502`）：`PrimaryScreen` = 1 个；`StretchAll` = 1 个（几何为所有屏幕并集）；`MirrorAll` = 每个屏幕 1 个，**只有第 0 个**承载音频与状态推进。

## 2. 创建路径

```text
startPlaying()  :898   复位状态 → ensureHeartbeatTimers() → playIndex()
playIndex()     :656   m_started=true → ensureOutputs()
ensureOutputs() :637   已有输出 → remountOutputs()（轻量，不重建管线）
                       无输出   → layoutOutputs()
layoutOutputs() :298   teardownOutputs() → fbswin::ensureWorker() → 按模式 makeOutput()
makeOutput      :311   new QVideoWidget → new QMediaPlayer → new QAudioOutput
                       setAudioOutput() + setVideoOutput() + 4 组信号连接 → show → mountBehindIcons()
```

要点：
- 每创建一个输出，`m_playersCreated/m_widgetsCreated/m_audiosCreated` 各 +1，并写一条 Debug 级 `创建输出: players=N/M …`（`:320-327`）。这四个计数器是「有没有泄漏对象」的直接证据，诊断窗口可见。
- `mountBehindIcons()` 在 `fbswin` 的**工作线程**里做 `SendMessageTimeout`（防 explorer 挂死拖死 GUI），GUI 线程不等待。
- 音频策略与帧率上限在 `metaDataChanged` 时二次断言（`:414-423`），因为后端会在媒体加载时把轨道选择重置回默认。

## 3. 销毁路径

`teardownOutputs()`（`:589`）是唯一销毁口：
1. `victims.swap(m_outputs)` —— 先把整张表换出，重入时看到空表，避免同一批对象被二次 `deleteLater`；
2. `player->disconnect(this)` —— 先断信号，清理路径上不再有回调进来；
3. `player->stop()`；
4. `fbswin::unmountWindow(widget)` → `widget->hide()` → 三者 `deleteLater()`；
5. 退出清理模式（`m_shuttingDown`）额外 `sendPostedEvents(DeferredDelete)` **就地兑现**，顺序固定 player → audio → widget。

为什么必须异步删：`teardownOutputs()` 可能被播放器信号链间接触发（`EndOfMedia → nextTrack → playIndex → ensureOutputs → layoutOutputs → teardownOutputs`），同步 `delete` 会析构正在发信号的 sender。唯一例外是退出时事件循环已停，`deleteLater` 永不兑现，所以先 `disconnect` 再就地兑现。

`stopAll()`（`:960`）：停心跳定时器 → 逐个 `player->stop()` → `teardownOutputs()` → 状态字段复位 → 若开启「智能释放内存」立即 `trimMemory()`（不等下个 30s 周期）→ 发「已停止」。

## 4. 窗口失效与重挂载

`remountOutputs()`（`:505`）每拍只在必要时做事：
- 若 `IsWindow(winId())` 为假（桌面宿主窗口被销毁，连带干掉挂在下面的原生窗口），**更换一个全新 `QVideoWidget`** 并 `setVideoOutput(nw)`。原因写在注释里：旧的 D3D 交换链随旧窗口失效，播放器继续往旧表面送帧而新窗口永远黑屏，光重建原生窗口不够。旧 widget 走 `deleteLater`，其 sink 由 `setVideoOutput` 切换时释放。
  **归因勘误**：日志文案写的是「(explorer 重启)」，本机不成立。40 分钟浸泡期间共 4 次重绑（12:00:03.484 / 12:30:04.523 / 13:00:07.384 / 13:30:05.546，见 `video_performance_test.md` §9.2），跨越 3 个不同的应用进程会话，且 `explorer.exe`（PID 17396）自 10:01:04 起全程未重启 → 触发者是与本进程无关的**机器级、整点/半点周期事件**，不是 explorer 重启。本机未能定位到具体来源（计划任务、幻灯片、显示重置等均已排除），属未确认项；措辞待后续单独修，本次不改代码。
- 几何按 `logicalRect × devicePixelRatioF()` 换算成物理像素比对，位置正确就不动。
- `scheduleMountFix()`（`:558`）节流 10s，但**探测不到 WorkerW 时不重置节流**，让下一拍（1s）立刻重试，把 explorer 重启后的黑屏从最长 10s 压到约 2s。
- 重绑的一次性代价（soak30 实测，见 §11.5）：Dedicated 由 149.1~180.3（均值 150.5）换到 261.0~303.9（均值 262.5），Private 由 274.9~357.0（均值 305.2）换到 389.8~467.5（均值 425.2），**一次性 +112 MB 显存 / +120 MB 提交内存**；4K60（WMF 对照用例）方向相反，567.5→438/456.1。即重绑**换掉了一整套解码/呈现缓冲池**，而不是单调增长，所以不能拿跨过重绑的均值当稳态。
- `scheduleRelayout()`（`:572`）响应 `QScreen::geometryChanged` / `screenAdded` / `screenRemoved`，600ms 防抖合并成一次重建（DPI 变化会连发多个信号）。

## 5. 音频输出生命周期

- 创建即 `setMuted(true)`（`:328`），避免起播瞬间漏出一帧声音。
- `applyAudioPolicy()`（`:1085`）：只有第 0 个输出且 `m_volume > 0` 才 `setActiveAudioTrack(0)`，否则 `-1`。**音量 0（壁纸默认态）时音频轨根本不选**，FFmpeg 的音频解码线程、重采样器、音频设备占用全部省掉。历史 A/B：同素材有音频容器差异 ≤5 MB，但解码线程与设备占用是真省下来的。
- `setVolume()`（`:1067`）改音量时对第 0 个输出重新调用 `applyAudioPolicy`，其余输出恒 `muted` + `volume=0`。
- 销毁与 player 同批 `deleteLater`。

## 6. 视频切换流程

`playIndex(index, resumePos)`（`:656`）：
1. `session++`，换曲时清零重试额度与看门狗；
2. `ensureOutputs()`；
3. 比对每个输出的 `player->source()` 与目标 URL，**全等则走 sameSource 快路径**：只 `setPosition(0)`，不 `setSource`；否则 `setSource(url)` 并先播报「第 N 个 打开中…」；
4. 无论快路径还是换源，都重新断言 `applyLoopPolicy()` + `applyPlaybackRate()` + 音量/静音；
5. `play()`，最后 `emitTrackState()`。

sameSource 快路径是内存/显存的关键保护：**换 `source` 会让 FFmpeg 销毁并重建解复用器与解码器**，单视频循环若每圈换源就是每圈一次全量重建（实测每圈多一次 1-2s 的探测与一波内存抖动）。

真换源（列表循环/随机）：`EndOfMedia`（`:358`，仅首个输出）→ `nextTrack()`（`:764`）→ 跳过失败名单取下一条 → `playIndex()`。这里**不销毁任何对象**，只换 source；`teardownOutputs()` 不在切换路径上。

错误链路（`:447-483`、`:872`）：`errorOccurred` → 第 1 次 500ms、第 2 次 1500ms 后原地重试（重试前 `setSource(QUrl())` 强制后端重开）→ 仍失败进 `m_deadTracks` 并跳下一曲 → 全员失败才 `stopAll()`。坏曲目不再参与轮换，避免「每圈解码器重建」的乒乓循环。

## 7. 循环流程

- 单循环模式，或列表只剩一条素材 → `isSeamlessLoop()` 为真（`:725`）→ `applyLoopPolicy()` 下 `setLoops(QMediaPlayer::Infinite)`（`:1098`），由后端自己回绕，**应用层一个信号都不接**，无黑帧、无解码器重建。
- 兜底一：后端不遵守无限循环（时长未知的流/个别后端）时，心跳发现 `shouldPlay && !wasPlaying` 且 `atMediaEnd()` → `setPosition(0)` 再 `play()`（`:1187-1193`）。
- 兜底二（看门狗，`:1198-1222`）：进度连续 3 拍（约 3s）纹丝不动 → `restartSingleLoop()` 原地回绕。只读写 `m_watchPosMs/m_watchStalls`，不新建对象。
- 列表循环/随机需要切源，走 §6 的 `nextTrack()`。
- 整表播完：`finishPlaylist()`（`:821`）**刻意不 `stop()`** —— 停止会清空呈现面，用户看到「桌面直接变黑」；EndOfMedia 状态下解码器已自行停机，末帧留在表面上，进程回到近空闲。

## 8. 空闲与挂起时的资源释放

`evaluateSuspend()` 每秒一拍（`:1133`），汇总五类挂起原因（全屏/被遮挡/锁屏/显示器关闭/电池）。实测（`v_pause_cycle`，5s 采样、每 20s 切一次暂停，`new-build/perf-results/v_pause_cycle.csv`）：

| 状态 | CPU % | GPU 引擎合计 % | 3D % | Working Set (MB) | Private (MB) |
|---|---|---|---|---|---|
| 播放段 | 0.17-0.23 | 23.8-29.7 | 13.3-19.2 | 293-357 | 270-355 |
| 暂停段 | 0.00-0.04 | **0.00-0.06** | ≈0 | **23-27** | 269-277 |

结论：**暂停后没有任何后台渲染与解码**，GPU 与 CPU 直接归零，不是「降到低位」。但 `Private`（提交内存）停在 270 MB 不动 —— 解码管线与呈现表面还在，暂停 2s 后 `trimMemory()` 只把**工作集**从 350 MB 压到 23 MB（页被换出），不还提交内存。

真正归还提交内存靠 `longSuspendRelease()`（`:1343`）：挂起持续超过 `kLongSuspendReleaseMs = 180000`（可用 `YUMEIREN_LONG_SUSPEND_MS` 覆盖）就 `teardownOutputs()` + `trimMemory()`，记录 `m_resumePosMs`；恢复时由心跳重建管线并在 `LoadedMedia` 里 `setPosition()` 跳回（`:366-370`），代价约 2s。列表已正常播完时**不释放**（播完状态会拦住重建分支，卸载后壁纸回不来）。

## 9. 退出流程

单例是函数内 static，析构由 CRT `atexit` 链在 `main()` 返回之后跑，那时 `~QApplication` 已完成，任何 `QWidget` 调用都会踩到 `qApp == nullptr`（历史上就是 `c0000005` 退出崩溃）。因此退出收口是显式的：

```text
main() 末尾 → VideoWallpaper::shutdown()   :99   单例不存在时什么都不做（绝不为了清理而构造）
            → shutdownNow()                :109   m_shutdownDone 幂等 / m_shuttingDown 短路一切回调
              stopHeartbeatTimers()
              QCoreApplication::removePostedEvents(this, QEvent::Timer)   取消未触发的重试/跳曲/重布局/裁剪
              stopAll()                                复用统一停播收口（含 flushNow 的就地销毁）
              探针三件套 delete                        同样必须赶在 qApp 析构前
            → 日志「退出清理完成: players=N/M widgets=N/M audios=N/M」，三对计数相等即无对象残留
```

`~VideoWallpaper()`（`:87`）只作兜底：`!qApp || m_shutdownDone` 时直接返回。

实测（`run_case.ps1` 用 `CloseMainWindow()` 正常关窗，非强杀）：停播/退出后线程回到 19、句柄 1307、Private 69 MB（`b_idle`），与从未播放过的空闲态同一水平；历史数据点「`stopAll()` 30s 后 Private 95 MB、GPU 显存归零」见 `docs/perf/perf-baseline.md`（HEAD 版本）。

## 10. 生命周期上的已知约束

- `isLiveOutput()` 用指针值做存活校验，理论上存在「旧播放器地址被新播放器复用」的误判窗口；退出清理期间一律判为不存活，风险仅在运行中。
- `QMediaPlayer` 的 `deleteLater` 由后端自己决定线程回收时机，因此「停播后线程数回落」不是瞬时的（实测停止到下一次心跳内完成）。
- `MirrorAll` 下每屏一套解码管线（N 个 player + N 个交换链），内存/显存随屏数线性；本任务书的多屏项在本机无法验证（只有一块屏），见 `video_performance_test.md`。
- `StretchAll` 只 1 个输出，但窗口横跨全部虚拟桌面，呈现面尺寸随之放大。

## 11. 生命周期实测取证（回归套件 short 组）

用例都跑在优化后的构建上，`-Diag` 开启，配置在每例结束后自动还原。

### 11.1 暂停/恢复 ×345（`r_pause100`，`YUMEIREN_AUTO_PAUSE_MS=1000`，1080p30，346 s）

| 项 | 结果 |
|---|---|
| 「自动切换暂停」日志条数 | **345**（即 345 次暂停/恢复切换，约 172 个完整往返） |
| 播放器创建/销毁累计 | `players=1/1 widgets=1/1 audios=1/1` |
| 线程数走势 | 93 → 83（**单调下降并收敛**，不随切换次数累积） |
| 句柄走势 | 2967 → 2954（无增长） |
| Private 走势 | 每段在 276~282 MB 之间往复，末段 281.3 MB ≈ 首段 279.5 MB |
| 错误日志 | 0 条 |

结论：暂停/恢复不新建任何对象，也没有资源累积。暂停段 GPU 三引擎归零（见 §8 分段表），恢复后回到同一稳态。

### 11.2 停止→重载（`r_reload`，40 s 停 / 20 s 后重启，1080p30，186 s）

- 第 1 次播放稳态：Private 330.2 / 331.7 / 276.3 / 276.2 MB，线程 92。
- `stopAll()` 之后（42.6 s ~ 58.1 s 四点）：Private **195.9 → 96.9 MB**，Working Set 30.3 → 30.2 MB，线程 **18**，句柄 **1444**，GPU 合计 **0.00%**。
- 重启后（69 s 起）回到 Private 263~332 MB、线程 88 → 82。
- 全程 `players=2/2`（两轮各建一次、各销一次），错误 0 条。

**同一秒级口径看，重载完全回落到接近空闲水平，未出现只升不降的台阶。**

### 11.3 停止后残留（`r_stopresidual`，40 s 停后观察 130 s）

| 量 | 停前 | 停后 2 s | 停后 90 s | 停后 130 s |
|---|---|---|---|---|
| Private (MB) | 328.6 | 194.9 | 93.0 | 92.9 |
| Working Set (MB) | 353.6 | 29.1 | 29.4 | 29.6 |
| 线程 | 92 | 19 | 11 | 6 |
| 句柄 | 2964 | 1446 | 1418 | 1408 |
| GPU 合计 % | 27.3 | 0.00 | 0.00 | 0.00 |

残留 92.9 MB / 6 线程 / 1408 句柄，且**随时间继续下降**（线程 19→6、句柄 1446→1408），是后端线程池自行退出，不是应用持有对象。对比 `b_idle`（从未播放过：Private 69.3 MB / 19 线程 / 1307 句柄），停止后的提交内存高出约 23 MB，属已加载 DLL 与堆碎片级别，**未见播放器级残留**。

### 11.4 视频切换（`r_switch`，3 条素材 + 列表循环，233 s）

- 曲目切换 **23 次**，三条素材均出现（`第 1/2/3 个 播放中`），周期约 10 s（素材时长）。
- 播放器创建/销毁累计 **1/1**：切换只 `setSource`，**不重建管线**，与设计一致。
- 错误 0 条。
- Dedicated 显存在 29 个样本里稳定于 **136.5–149.6（均值 140.8 MB）**，Shared 均值 94.9 MB → 切换后回到同一稳态，不逐次抬高。
  **勘误**：本节原写「与基线完全对上（149.1 / 190.2 / 149.0）」属误判。`r_switch` 会话（12:46:08.901 起）内**没有**任何窗口重绑事件，Dedicated 从未出现过 190.2；基线 `b_1080p24` 的 190.2 是「重绑前 7 点 + 重绑后 4 点」的均值伪影（实测阶跃 149.1→241.5，见 §11.5 与 `video_performance_test.md` §9.1）。24fps 素材的真实稳态池与 1080p 其余各档同为 149.x。

### 11.5 窗口重绑（soak30 自然发生 2 次，1080p30，40.4 min）

`r_soak30_1080p30` 的会话内出现两次重绑，日志与 CSV 逐点对齐：

| 序号 | 日志时刻 | 会话内 elapsed | Dedicated 前后 | Private 前后 |
|---|---|---|---|---|
| 1 | 13:00:07.384 | ≈ 447 s | 149.1 → 261.5（453.2 s 处首个受影响样本） | 330.4 → 448.1 |
| 2 | 13:30:05.546 | ≈ 2244 s | 261.5 → 261.6（**无可测变化**，已在新池上） | 441 → 450.5 → 442.1 |

按重绑切分整段（阈值 elapsed 445 s）：

| 段 | 样本 | Dedicated MB | Private MB |
|---|---|---|---|
| 重绑前 | 22 | 149.1–180.3（均值 150.5） | 274.9–357.0（均值 305.2） |
| 重绑后 | 102 | 261.0–303.9（均值 262.5） | 389.8–467.5（均值 425.2） |

取证结论：

1. 重绑**不是泄漏**：第二次重绑没有再抬一层，重绑后 33 分钟里 Dedicated 稳定在 261.5/261.6 两个值，Private 在 390~465 往复，句柄 2974→2918、线程 94→78 全程单调下降。
2. 重绑的稳态代价是**一次性、量级 +112 MB 显存 / +120 MB 提交内存**，方向与素材相关（4K60 反而下降），符合「换了一套新池」而不是「多加一套池」。
3. `m_widgetsCreated` 计数器不覆盖 `remountOutputs()` 里新建的 `QVideoWidget`，因此诊断日志里的 `widgets=1/1` **不能**证明窗口未被重建；这条可观测性缺口记在 `video_performance_audit.md` §3 与 §9.1，本任务未改。

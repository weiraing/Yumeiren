# 视频壁纸状态机（阶段 1 交付物）

- 对应实现：`src/videowallpaper.{h,cpp}`（单例 `VideoWallpaper`，全部逻辑运行于 GUI 线程）。
- 性质说明：当前实现是**标志位组合式状态机**（m_started / m_manualPaused / m_suspendReasons / m_outputs 空非空 / m_errorStreak），不是单一枚举值。本文档给出与代码一一对应的**推导态**命名与转换图；为满足阶段 7 诊断需要，阶段 2 引入 `m_playbackSessionId`（会话 ID）与对象计数，阶段 3 引入 `m_fileRetries`（单文件重试计数）。**未引入新的枚举状态机**——现有语义已被两阶段实验验证（内存归因、重载复基线、熄屏长挂起释放），重写为枚举机属于高风险无收益改动。

## 1. 状态定义（推导态 ↔ 代码标志）

| 推导态 | 判定条件 | 用户可见文本 |
|---|---|---|
| Uninitialized | `!m_started && m_outputs.isEmpty()` | （未启动） |
| Loading | playIndex 已发出、等待 LoadedMedia | "第 N 个 打开中…" |
| Playing | 任一 player 处于 PlayingState 且 `m_suspendReasons==0 && !m_manualPaused` | "第 N 个 播放中" |
| Paused(手动) | `m_manualPaused && m_suspendReasons==0` | "已暂停" |
| Suspended(自动) | `m_suspendReasons != 0`（全屏/遮挡/锁屏/熄屏/电池位掩码） | "检测到全屏应用…已自动暂停" 等 |
| SuspendedReleased | `m_started && m_outputs.isEmpty()`（长挂起 ≥180s 释放后，等待恢复） | "暂停较久，已释放壁纸资源…" |
| Error(计数中) | `m_errorStreak > 0`（连续失败曲目数），未达列表长度 | "第 N 个无法播放(原因)…" |
| Stopped(终态) | `stopAll()` 后：`!m_started && m_outputs.isEmpty() && m_index==-1` | "已停止" |
| NoMedia | `m_playlist.isEmpty()` | "播放列表为空…" |
| Rebuilding(瞬态) | `m_relayoutPending`（DPI/几何防抖 600ms 内）或 setScreenMode 重建中 | （沿用当前文本） |

## 2. 状态转换图

```text
Uninitialized ──startPlaying(playlist非空)──▶ playIndex ──▶ ensureOutputs ─┬─outputs空─▶ layoutOutputs(makeOutput×K)─┐
      ▲                                                                    └─outputs有─▶ remountOutputs(轻量)         ▼
      │                                                                                                        Loading
      │                                                                                              (LoadedMedia→应用音轨/帧率/提示)
      │                                     ┌── mediaStatusChanged(EndOfMedia) ◀──────────┐                    │
      │                                     │           (autoLoop: nextTrack→playIndex)   │                    ▼
      │                                     │                                             └─────────────▶ Playing ◀─┐
      │        stopAll()                    │                                                                │     │
      │  ┌──────────────────────────────────┘                                           playbackState(Playing)│  evaluateSuspend(1s):
      │  ▼                                                                              (m_errorStreak=0)    │  reasons>0→pause()
  Stopped ◀── 所有曲目连续失败(m_errorStreak≥列表长度) ◀── errorOccurred ─── 有限重试(阶段3) ◀──┘              │  reasons==0→play()
      │                                                       │失败跳过(advanceOnError)                      │
      │                                                       ▼                                              │
      │                                                  nextTrack → playIndex                               │
      │                                                                                                      │
      └── Uninitialized；长挂起：Suspended ──(m_suspendClock≥180s)──▶ longSuspendRelease ─▶ SuspendedReleased
                                                        (teardownOutputs；m_resumePosMs 记进度)
                                                        SuspendedReleased ──(原因消失)──▶ playIndex(带resumePos) ─▶ Loading

  Rebuilding: QScreen geometry/added/removed → scheduleRelayout(防抖600ms) → layoutOutputs → playIndex(m_index)
  挂载修复:   mountIsStale/无真实WorkerW → scheduleMountFix(节流10s) → remountOutputs（不重建管线）
```

## 3. 各状态允许的操作（4.3 检查单逐项）

| 操作 | 入口函数（均在 GUI 线程） | 允许来源态 | 幂等性 |
|---|---|---|---|
| 初始化播放器/视频窗口/音频输出 | `layoutOutputs()`→`makeOutput` | outputs 为空 | 是：`ensureOutputs` 非空仅轻量重挂载 |
| 设置媒体源 | `playIndex()`（同源走 setPosition(0)，异源 setSource） | 任意 m_started 态 | 同源路径不重建解码器 |
| 开始播放 | `startPlaying()` / `evaluateSuspend` 恢复分支 | Uninitialized/SuspendedReleased/Paused | startPlaying 重复调用转 evaluateSuspend，不重复创建 |
| 暂停/恢复 | `pauseResume()`；自动挂起经 `evaluateSuspend` | Playing/Paused/Suspended | 手动暂停优先级高于自动恢复 |
| 停止播放 | `stopAll()`（UI 取消/全坏停播/清空列表/析构） | 任意 | 是：空 outputs 时 teardown 为 no-op |
| 清空媒体源 | `clearPlaylist()`→stopAll | 任意 | 同上 |
| 销毁播放器/窗口 | `teardownOutputs()`：stop→unmount→hide→deleteLater(widget→player→audio) | 任意 | clear() 后二次调用为 no-op |
| 长时间挂起释放 | `longSuspendRelease()`（evaluateSuspend 内计时触发） | Suspended 且暂停持续 ≥180s | 仅触发一次（释放后 outputs 空） |
| 从挂起恢复 | `evaluateSuspend` 恢复分支（5s 节流重建） | SuspendedReleased | 恢复后 reasons==0 才 play |
| 切换视频 | `nextTrack()`→`playIndex` | Playing/Error | 同上 |
| 切换显示器模式 | `setScreenMode()` | 任意；值不变直接返回 | 是 |
| 关闭动态壁纸 | UI 取消按钮 → `stopAll()` | 任意 | 是 |
| 应用退出 | `~VideoWallpaper()`→`stopAll()` | 任意 | 见风险点 R1 |
| 播放失败 | `errorOccurred`→有限重试→`advanceOnError()` | Loading/Playing | 重试有 500/1500ms 退避与次数上限（阶段 3） |
| 媒体加载失败/解码失败 | 同上（errorOccurred 统一收口）；无视频轨文件在 LoadedMedia 检出（阶段 3） | Loading | 同上 |

## 4. 所有权关系

| 对象 | 创建点 | 所有者 | 释放点 |
|---|---|---|---|
| QVideoWidget | makeOutput | `m_outputs` 列表独占持有（无 Qt 父对象，顶层窗口） | teardownOutputs→deleteLater |
| QMediaPlayer | makeOutput（父=VideoWallpaper 单例） | 双持有：父对象 + m_outputs；deleteLater 析构时自动脱离父列表 | teardownOutputs→deleteLater |
| QAudioOutput | makeOutput（父=单例） | 同上 | teardownOutputs→deleteLater |
| QTimer（心跳/回收） | 构造函数（父=this） | 单例 | 随单例析构 |
| 探针对象（阶段2实验） | runProbeStage（父=单例，独立于 m_outputs） | 单例 | 进程退出（仅自动化测试用） |

释放顺序（实测验证正确性，**不得机械调整**）：`player->stop()` → `fbswin::unmountWindow` → `widget->hide()` → 依次 `deleteLater`(widget→player→audio) → `m_outputs.clear()`。deleteLater 为刻意的异步删除：teardown 可能被 EndOfMedia 信号链触发，同步 delete 会析构正在发信号的 sender（use-after-free）。资源释放实证：停止后 Private 397→95MB、GPU→0、句柄/线程回落（docs/perf/memory-attribution.md 实验五）。

## 5. 异常状态处理方式

- 单文件失败：errorOccurred → 记日志(Warning) → 有限重试（500ms/1500ms 两次，阶段 3）→ 仍失败则跳下一曲并 `++m_errorStreak`；仅首个输出参与推进（MirrorAll 防重复报错）。
- 全部失败：`m_errorStreak >= m_playlist.size()` → 状态提示 + `stopAll()`（终态，无重试循环）。
- 用户主动停止 / 系统挂起：不记为错误（stopAll 与 Suspended 均走 Info 级状态日志）。
- 解锁/唤醒/熄屏恢复：evaluateSuspend 心跳自动恢复；长挂起走重建路径。
- Explorer 重启：1s 心跳检测挂载失联 → 10s 节流重查 WorkerW → remount。

## 6. 现有代码风险点（扫描结论，含处置）

| 编号 | 风险 | 等级 | 处置 |
|---|---|---|---|
| R1 | `~VideoWallpaper()`→stopAll 的 deleteLater 在事件循环退出后不执行 | 极低（进程退出 OS 兜底回收） | 已知可接受；状态机文档明示 |
| R2 | makeOutput 信号 lambda 按值捕获 `out`，teardown 与实际删除之间的窗口内可能收到陈旧信号 | 低 | 已有双重守卫：QObject 析构自动断连 + `out.player == m_outputs.first().player` 检查；阶段 2 增加会话 ID 便于诊断观察 |
| R3 | 无音频轨（纯音频）文件会"正常播放"但画面黑屏 | 低 | 阶段 3：LoadedMedia 时检查 `hasVideo()`，无视频轨按不可用素材自动跳过 |
| R4 | 错误仅立即跳过，瞬态失败（文件被占用等）无重试 | 低 | 阶段 3：有限重试 + 退避 |
| R5 | 重复 setPlaylist 时 m_index 依赖文件名匹配 | 极低 | 已实现（保持当前曲目指针） |

## 7. 本阶段修改的文件与函数

- 仅新增本文档；阶段 2/3 的代码变更分别记录于其自身章节与 git 提交历史。核心文件：`src/videowallpaper.h`（新增 m_playbackSessionId、m_fileRetries、对象计数、m_lastHintRes）、`src/videowallpaper.cpp`（playIndex/makeOutput/teardownOutputs/errorOccurred/mediaStatusChanged/metaDataChanged）、新增 `src/videodiag.{h,cpp}`（阶段 7）。

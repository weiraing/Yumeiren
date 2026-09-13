# 视频壁纸诊断模式（阶段 7 交付物）

对应实现：`src/videodiag.{h,cpp}`（统一日志接口）、`src/videowallpaper.cpp`（关键状态转换埋点）、`src/main.cpp`（尽早初始化）。

## 1. 使用方式

```text
诊断模式开关：
  环境变量  YUMEIREN_DIAG=1            （优先）
  注册表    HKCU\Software\Yumeiren\Yumeiren\video\diag = true
资源采样：  YUMEIREN_DIAG_SAMPLE_MS=<毫秒>（默认 0=关闭；下限 1000ms）
日志位置：  %LOCALAPPDATA%\Yumeiren\logs\videowallpaper.log（1MB 滚动，保留一份 .old）
```

- 未开启诊断模式时，Info/Warning/Error 照常写文件（低频，不影响播放）；Debug 与资源采样仅在诊断模式输出。
- 不输出完整路径：媒体仅记录文件名（任务书 6.4 隐私要求）。

## 2. 日志等级与埋点清单

| 等级 | 事件 |
|---|---|
| Error | （预留：当前致命错误均以 Warning+状态文本呈现，停播即终态） |
| Warning | 媒体错误（session/文件名/分类/后端 detail）、曲目不可播（fails/dead 计数）、挂载点获取失败 |
| Info | 启动与 diag 开关状态、单例初始化、每次起播/换曲/重试（session=N index=I/K file=xxx resumePos）、teardown（n=对象数 + 创建/销毁累计）、stopAll、长挂起释放（resumePos）、恢复播放、自动挂起原因位掩码 |
| Debug（仅诊断模式） | 对象创建计数（players/widgets/audios created/destroyed）、资源采样（ws/private/threads/handles，间隔可配） |

`session=N` 为播放会话 ID（`m_playbackSessionId`，每次起播/换曲/重试自增），配合对象创建/销毁累计计数，可直接回答任务书 10.2 要求的"播放器创建次数/销毁次数、视频窗口创建次数/销毁次数、音频输出创建次数/销毁次数"。

## 3. 资源采样说明

- 进程内采样：Working Set、Private Bytes（`GetProcessMemoryInfo`）、线程数（Toolhelp 快照）、句柄数（`GetProcessHandleCount`）。
- GPU 专用/共享显存与 GPU 引擎利用率来自系统性能计数器，进程内获取代价高，由外部采样器 `tools/perf/sample2.ps1` 覆盖（回归测试脚本自动调用）。
- 采样默认关闭、下限 1s、单行日志，实测对播放无可测影响（采样运行帧率/资源与未采样一致）。

## 4. 实测样例（YUMEIREN_DIAG=1, SAMPLE_MS=5000, 播放 1080p30 45s）

```text
15:01:55.897 [I] ==== 视频壁纸模块启动 (diag=on) ====
15:01:55.900 [I] session=1 play index=1/1 file=inv_1080p30.mp4 resumePos=-1
15:01:56.142 [D] 创建输出: players=1/0 widgets=1/0 audios=1/0
15:01:57.231 [I] 恢复播放: reasons=0 manualPaused=0
15:02:00.944 [D] sample t=0s ws=383MB private=347MB threads=107 handles=3051
...
15:02:40.918 [D] sample t=39s ws=366MB private=370MB threads=106 handles=3046
```

## 5. 已知限制

1. 日志为单文件顺序写（1MB 滚动），无多文件归档；长時间诊断需外部收集。
2. Error 级暂无独立事件（当前架构下错误以有限重试+状态文本收敛，最重为"整体停播"Warning）。
3. 采样不包含 GPU 计数器（见上，外部采样器负责）。

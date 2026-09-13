# 视频壁纸自动化回归测试记录（阶段 8 交付物）

- 套件：`tests/video_wallpaper/run_tests.ps1`（采集器 `collect_metrics.ps1` → `tools/perf/sample2.ps1`；说明见同目录 README.md）。
- 最终运行：**7/7 通过**（本报告末尾汇总表）。运行环境：单屏 2560×1600@150%，Qt 6.10.2 MinGW，YumeirenTest（HEAD，含阶段 2/3/7 代码）。

## 1. 自动化用例结果（最终一轮）

| 用例 | 结果 | 关键读数 |
|---|---|---|
| T01 基础播放 1080p30 | ✅ | GPU 专用 232MB、线程 104.6（解码管线正常） |
| T02 连续切换（5 短片 3min） | ✅ | 诊断日志 session 换曲 **39 次**，GPU 176MB，无堆积 |
| T03 停止→30s→重载 | ✅ | 停止期 Private 74.7MB；重载后 GPU 231MB（复基线） |
| T04 全坏文件 | ✅ | 后段 GPU=0（有限重试→整体停播，无死循环） |
| T05 坏+好跳过 | ✅ | 后段 GPU 231MB（自动跳过坏曲） |
| T06 无视频轨+好 | ✅ | 后段 GPU 231MB、Private 393MB（跳过后稳定播放，无乒乓爬升） |
| T07 空闲基线 | ✅ | Private 50.4MB、GPU=0 |

## 2. 套件建设过程中发现并修复的真实缺陷

回归套件的价值在调测过程中即已兑现——以下缺陷均由自动化用例暴露，逐项修复并用同一用例回归验证：

| # | 缺陷 | 暴露用例 | 根因与修复 |
|---|---|---|---|
| 1 | 挂起窗口内重试被永久放弃，管线滞留"已加载无事件"僵局（GPU 恒 44MB） | T04/T05 | 重试 lambda 原先在 `m_suspendReasons!=0` 时直接 return；修复：重试照常清源重开，最终播停交由 evaluateSuspend 心跳裁决 |
| 2 | 正常视频在 LoadedMedia 时刻被误判"没有视频轨"（T06 中 inv_1080p30 被跳过） | T06 | LoadedMedia 时 `hasVideo()`/分辨率元数据尚未就绪；修复：检测延迟 2s + 首输出/同源守卫 + pending 去重 |
| 3 | 坏素材每轮轮换被重新加载：乒乓循环（12s 解码器重建周期 + Private 爬升 485→563MB），且第 2 轮 `setSource` 触发后端对特定媒体的重载**阻塞**（GUI 静默 ~80s） | T06 | 跳过一次即入失败名单（原地 3 次重试已覆盖瞬态故障），坏素材永不参与第二轮轮换；全部入名单才停播 |
| 4 | 错误态播放器对同一 source 不会再次发 `errorOccurred`，同源重试无效 | T04/T05 | 重试前 `setSource(QUrl())` 清空强制后端重开 |

环境侧（非产品缺陷）：后台任务控制台覆盖工作区触发"桌面遮挡"自动暂停干扰测试（套件改用 `-PauseFullscreen false`）；`run_awake.ps1` 缺 `-WasPlaying` 参数；PowerShell 5.1 对无 BOM UTF-8 中文注释按 ANSI 解码破坏语法（改写为带 BOM）。

## 3. 测试期间的数据文件

- `tools/perf/results/rt_t01..rt_t07.{csv,log}`：CSV 指标 + 每用例诊断日志（含 session/错误分类/对象计数，见 `docs/VIDEO_WALLPAPER_DIAGNOSTIC_MODE.md`）。
- 阶段 3 修复过程数据：`t_all_broken*.csv`、`t_retry_skip*.csv`、`t_audio_only*.csv`、`t05_repro.csv`（保留作为缺陷证据链）。

## 4. 手工测试清单（未自动化，见 README）

锁屏/解锁 ×20、睡眠/唤醒、Explorer 重启、双屏系列（M2/M3/M5/M6）、快速重复锁屏解锁——本机为单屏且锁屏/睡眠需改变系统全局状态；熄屏→长挂起释放→恢复链路已在第二阶段真实事件中验证（`docs/perf/memory-attribution.md` 实验五）。

## 5. 已知限制

1. 换曲次数统计依赖诊断日志（`YUMEIREN_DIAG=1`），套件自动设置。
2. 阈值型断言（GPU>100MB 等）在显著更慢的机器上可能需要调整。
3. 无多屏用例（硬件限制），脚本就绪待接入双屏。

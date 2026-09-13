# 视频壁纸自动化回归测试（阶段 8）

## 运行方式

```powershell
# 前置：已构建 build/YumeirenTest.exe；测试素材位于 build/media/video（见下）
powershell -NoProfile -ExecutionPolicy Bypass -File tests\video_wallpaper\run_tests.ps1
```

- 每个用例通过 HKCU `video/*` 设置与 `tools/perf/run_awake.ps1` 启动无管理员版 `YumeirenTest.exe`（与性能基线同一自动化通道），采样器 `collect_metrics.ps1`（→ `tools/perf/sample2.ps1`）每 5s 记录 WS/Private/CPU/句柄/线程/GPU 专用+共享。
- 运行期间自动设置 `YUMEIREN_DIAG=1`：T02 用诊断日志中的 `session=N play` 行统计换曲次数；其余用例的日志可在 `%LOCALAPPDATA%\Yumeiren\logs\videowallpaper.log` 复盘。
- 结果 CSV 输出到 `tools/perf/results/rt_t*.csv`。

## 测试素材

| 文件 | 说明 |
|---|---|
| `build/media/video/inv/inv_1080p30.mp4` | 1080p30 H.264 正常片（12s，循环） |
| `build/media/video/inv/bad_truncated.mp4` | 截断损坏（前 256KB） |
| `build/media/video/inv/bad_fake.mp4` | 伪 mp4（文本内容） |
| `build/media/video/inv/audio_only.mp4` | 仅音频轨 |
| `build/media/video/perf/perf_switch1-5.mp4` | 5 个 5s 切换短片 |

## 自动化用例与通过标准

| 用例 | 场景 | 通过标准 |
|---|---|---|
| T01 | 1080p30 播放 70s | 30s 后 GPU 专用显存均值 >100MB、线程 >50（正常解码管线） |
| T02 | 5 短片循环 3min | 诊断日志换曲（session 行）≥30 且 GPU >100MB（切换无资源堆积） |
| T03 | 播放 60s→stopAll→30s→重载 60s | 停止期 Private <130MB 且重载后 GPU >100MB（释放+复基线） |
| T04 | 两个坏文件 60s | 30s 后 GPU=0（有限重试→失败名单→整体停播，无死循环） |
| T05 | 坏文件+好文件 70s | 35s 后 GPU >100MB（自动跳过坏曲） |
| T06 | 无视频轨+好文件 90s | 45s 后 GPU >100MB 且 Private <470MB（跳过无轨素材、无乒乓爬升） |
| T07 | 空闲（不播放）55s | Private <130MB 且 GPU=0 |

任一用例失败：检查对应 CSV 时间线 + `videowallpaper.log`（含 session/错误分类/对象计数）。

## 手工测试清单（无法自动化）

锁屏/解锁（需输入密码）、睡眠/唤醒（改变系统全局状态）、Explorer 重启（侵入 shell）、双屏系列（拔插/主副切换/分辨率/DPI，见 `docs/VIDEO_WALLPAPER_SYSTEM_EVENTS.md` 验证矩阵与 `docs/MULTI_MONITOR_VIDEO_RESOURCE_ANALYSIS.md`）。逐项记录：操作步骤/预期/实际/是否黑屏卡顿/播放器创建销毁计数（诊断日志）/Private/GPU。

## 已知限制

- 单屏机器：多屏用例（M2/M3/M5/M6）待接双屏硬件。
- 未引入测试框架：纯 PowerShell + CSV 断言，符合任务书"不为测试阶段引入过重框架"。

# 视频壁纸下一阶段优化与工程化：最终验收报告

任务书：`视频壁纸模块下一阶段优化与工程化任务书`。执行记录（git）：
`状态机文档` → `a2c808f 阶段2/3/6` → `7418132 阶段7` → `73457e1 阶段8` → 本报告（阶段9）。
验证基线：`build/YumeirenTest.exe` Release（Ninja）与 Debug（cmake-build-debug，重新配置后 13/13 全量编译通过）双构建通过；自动化回归 **7/7 通过**；10 分钟浸泡零增长。

---

## 1. 修改前的问题（阶段 0 扫描结论）

代码整体已具备单实例复用/幂等/统一 teardown/挂起状态机（前两阶段成果），扫描确认的缺口：
① 无会话 ID 与对象创建/销毁计数，故障无法追溯；② 错误仅"立即跳曲"，瞬态故障（文件被占用等）无重试；③ 纯音频素材会"正常播放"但画面黑屏且不跳过；④ 诊断能力为零（无日志、无采样）；⑤ 回归测试不存在；⑥ 六份工程文档缺失；⑦ 多屏资源规律无数据、无文档。

## 2. 修改的文件

| 文件 | 变更 |
|---|---|
| `src/videowallpaper.h` | 新增 `m_playbackSessionId`、对象计数 ×6、`m_trackFails/m_deadTracks/m_fileRetries/m_noVideoCheckPending/m_lastHintRes`、`handleUnplayable()` |
| `src/videowallpaper.cpp` | 错误分类 `mediaErrorText()`；有限重试（500/1500ms + 清源重开）；失败名单；无视频轨延迟检测（2s，同源守卫）；高分辨率提示（metaDataChanged）；诊断埋点（起播/teardown/stopAll/长挂起/挂起恢复/错误） |
| `src/videodiag.{h,cpp}` | 新增：分级日志（E/W/I/D）、1MB 滚动、诊断开关（env/注册表）、可选资源采样 |
| `src/main.cpp` | `videodiag::init()` 尽早初始化（幂等） |
| `CMakeLists.txt` | 源列表加 videodiag.cpp；链接 psapi；SinkProbe（前一阶段） |
| `tests/video_wallpaper/` | 新增 run_tests.ps1 / collect_metrics.ps1 / README.md |
| `tools/perf/run_awake.ps1` 等 | 实验与测试基建（显示器保活、参数透传） |
| `docs/` | 新增 7 份文档（见 §15 交付清单） |

**未改动**：`src/platform/windows/desktopmount.cpp`（挂载/DPI/全屏检测——前两阶段实测验证过的精妙逻辑）、图片壁纸模块（engine/imageprocess）、ExplorerBgTool/ExplorerBlurMica DLL。

## 3. 修改的函数（videowallpaper.cpp）

`makeOutput`（计数+新信号处理）、`teardownOutputs`（计数+日志）、`playIndex`（会话 ID/重试额度/日志）、`nextTrack`（失败名单轮换+随机模式守卫）、`handleUnplayable`（新增）、`stopAll/startPlaying/setPlaylist`（失败名单生命周期）、构造函数（采样/日志）、errorOccurred/mediaStatusChanged/playbackStateChanged/metaDataChanged 连接体（重试/无视频轨/提示）、`mediaErrorText`（新增）。

## 4. 每项修改的原因

1. **会话 ID + 对象计数**：任务书 10.2 要求可追溯（创建/销毁次数），诊断与回归断言的基础。
2. **错误分类**：6.1 要求区分十余种情形；Qt 6.10 错误码映射 + 后端 detail 透传。
3. **有限重试 + 清源重开**：6.5 要求有界重试；实测教训——错误态播放器对同一 source 不再发 errorOccurred，必须清源。
4. **跳过一次即入失败名单**：实测坏素材轮转重试造成乒乓循环（12s 解码器重建 + Private 485→563MB 爬升），且第 2 轮 `setSource` 触发后端对特定媒体**重载阻塞**（GUI 静默 ~80s）；瞬态故障由原地重试覆盖。
5. **无视频轨延迟检测**：实测 LoadedMedia 时刻正常视频的 hasVideo/分辨率未就绪会被误判；延迟 2s + 同源守卫后无误判。
6. **重试不受挂起影响**：实测挂起窗口内重试被放弃会留下僵局；改为照常换源、播停由心跳裁决。
7. **诊断模式**：任务书第七阶段全部要求（分级/开关/采样/滚动/隐私——只记文件名）。
8. **分辨率提示**：6/9 阶段结论（88MB/百万像素）转化为用户可见提示，不强制转码。

## 5–8. 架构与路径不变性

- **视频播放架构未变**：QMediaPlayer → QVideoWidget → Qt RHI/D3D11（任务书红线）。
- **硬件解码路径未变**：d3d11va 全程生效（浸泡 CPU 0.2%、GPU 24.9%）。
- **未增加任何 CPU 帧转换**：视频模块 QVideoFrame/toImage 命中仍为 0。
- **未增加应用层帧缓存**：唯一"队列"仍是后端内部帧池。

## 9. 播放器创建/销毁统计（诊断计数器，回归实测）

T03（停止→重载）：创建 players/widgets/audios = 1/1/1，销毁 1/1/1（teardown n=1）；T02 连续 39 次换曲创建/销毁比不变（计数器只增于重建，换曲复用）；T04 全坏停播后计数归零对齐。诊断日志样例：`创建输出: players=1/0 widgets=1/0 audios=1/0`、`teardown n=1 累计 players=1/1 ...`。

## 10. 单屏与多屏资源数据

- 单屏（实测，`docs/perf/memory-attribution.md` + 本阶段 rt_*）：空闲 50–67MB；1080p30 405MB（Private）+ 238/203MB（GPU 专用/共享）；4K30 944MB；原片 4K60 957–1257MB 包络。
- 多屏：M2/M3/M5/M6 **因单显示器硬件无法执行**（脚本就绪）；MirrorAll 架构推断为线性增长（每屏一套解码管线），方案 C（单播放器多输出）被 Qt API 限制 + QVideoSink A/B 数据双重否定——详见 `docs/MULTI_MONITOR_VIDEO_RESOURCE_ANALYSIS.md`。

## 11. 锁屏/唤醒/Explorer 重启测试结果

| 场景 | 状态 |
|---|---|
| 熄屏→暂停→180s 长挂起释放→恢复 | ✅ 真实事件验证（第二阶段 inv_reload.csv + 本阶段 rt_soak10min.csv v1：0x10 挂起→370→94MB→GPU 0，释放后重建路径就绪） |
| 全屏/遮挡暂停恢复 | ✅ S6''（前阶段）+ 本阶段 0x10 事件复现恢复 |
| 锁屏/解锁、睡眠/唤醒、Explorer 重启、双屏 | ⚠️ 探测代码实证但场景需人工（单屏+需系统级操作）——清单见 tests/video_wallpaper/README.md，机制分析见 docs/VIDEO_WALLPAPER_SYSTEM_EVENTS.md |

## 12. 连续切换和重复启动测试结果

- **连续切换**：T02 5 短片 3 分钟自动换曲 **39 次**，GPU/句柄/线程稳定（历史 S5 34 次结论保持）。
- **重复启停**：T03 播放 60s→stopAll（Private 74.7MB、GPU 0）→30s→重载（GPU 231MB 复基线）；第二阶段重载实验与 34 次切换零累积结论一致。
- **10 分钟浸泡**（rt_soak10min_v2.csv）：Private 均值 405MB [373~455]，**线性趋势 +0.03 MB/min（零增长）**；GPU 专用 238MB（斜率 -0.06）；CPU 0.2%；句柄 ~3021、线程 ~96 稳定。
- 一次被 0x10 遮挡挂起打断的浸泡（v1）反而完整演示了怠速链路：暂停 → 180s 长挂起整管释放（370→94MB、GPU→0、resumePos 保留）。

## 13. 已知问题（截至本报告）

1. 1080p/4K 播放内存为 Qt FFmpeg 后端帧池固有成本（405MB / ~1GB），公开 API 无法压缩；QVideoSink 方向已被 A/B 数据否定。
2. 双屏相关场景（多屏资源、拔插、主副切换）未经实测——待硬件。
3. 锁屏/睡眠/Explorer 重启场景需人工执行清单（代码路径已实证或与已验证路径同源）。
4. FFmpeg 后端对特定媒体（无视频轨）重载存在阻塞风险——已通过失败名单规避触发路径，但根因在后端。
5. "播放结束"后停在最后画面（设计如此）；无进度条/时长 UI（未在任务范围）。
6. 回归断言阈值按本机标定（GPU>100MB 等），慢速机器可能需调整。

## 14. 后续是否值得继续底层重构

**不值得。** 依据：① 内存归因已量化（后端帧池，应用层无浪费）；② QVideoSink A/B 实测更差（内存 +20~28%、CPU ×5.5）；③ 本阶段全部稳定性/可诊断性目标在不动架构的前提下达成；④ 剩余内存优化唯一路径是自研解码管线（违反任务书 3.1/13 且收益不确定）。建议维持现状，仅在出现真实用户场景（如多屏高负载、4K 普及）时重新评估。

## 15. 交付清单核对

```text
docs/
├── VIDEO_WALLPAPER_STATE_MACHINE.md          ✅ 阶段1
├── VIDEO_WALLPAPER_ERROR_HANDLING.md         ✅ 阶段3
├── VIDEO_WALLPAPER_SYSTEM_EVENTS.md          ✅ 阶段4
├── MULTI_MONITOR_VIDEO_RESOURCE_ANALYSIS.md  ✅ 阶段5
├── VIDEO_MEDIA_COMPATIBILITY_POLICY.md       ✅ 阶段6
├── VIDEO_WALLPAPER_DIAGNOSTIC_MODE.md        ✅ 阶段7
├── VIDEO_WALLPAPER_REGRESSION_TESTS.md       ✅ 阶段8
└── VIDEO_WALLPAPER_PHASE_NEXT_FINAL_REPORT.md ✅ 本文档（阶段9）
tests/video_wallpaper/{README.md, run_tests.ps1, collect_metrics.ps1} ✅
```

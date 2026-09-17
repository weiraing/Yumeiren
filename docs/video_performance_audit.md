# 视频壁纸性能审计（Yumeiren）

审计对象：本次优化前的架构（HEAD 代码）+ 本机实测。数据出处见 `video_performance_baseline.md`、`video_resource_lifecycle.md`、`video_gpu_memory_analysis.md`；本文只写结构、瓶颈与判断，不重复推导过程。

## 1. 当前视频播放架构

```text
MainWindow（UI，只发指令不碰帧）
   └── VideoWallpaper（进程内单例，QObject，函数内静态）
         ├── m_playlist QStringList + m_index + m_mode（单循环/列表循环/随机）
         ├── m_outputs：QList<VideoOutput>，每条 = { QVideoWidget, QMediaPlayer, QAudioOutput }
         └── 状态机：m_started / m_manualPaused / m_suspendReasons / m_playbackFinished / m_shuttingDown
                     ↓
            WorkerW（fbswin::ensureWorker / mountWindow）
                     ↓
      Qt Multimedia 6.10.2 FFmpeg 后端 → d3d11va 硬解 → QRhi/D3D11 呈现
```

要点：

1. 一个播放输出就是一个屏幕目标，三件套（player/audio/widget）一起生、一起死，全部在 GUI 线程创建（`VW_ASSERT_GUI()`，`videowallpaper.cpp:300`）。
2. 帧数据不进应用：没有 `QVideoSink` 回调、没有 `toImage()`、没有自绘 `paintEvent`，grep 证据见 §5.3。
3. 应用层唯一的「帧率旋钮」是 `m_targetFps`（默认 24），实现方式是 `setPlaybackRate`，语义是**慢动作**而不是丢帧渲染（`applyPlaybackRate`，`videowallpaper.cpp:1050`）。
4. 空闲时进程完全静默：心跳定时器只在 `m_started` 期间运行（`ensureHeartbeatTimers` / `stopHeartbeatTimers`，`:225`/`:239`）。

## 2. 播放器对象生命周期

| 环节 | 位置 | 行为 |
|---|---|---|
| 创建 | `layoutOutputs()` → `makeOutput()`（`:311-336`） | `new QMediaPlayer(this)` + `new QAudioOutput(this)` + `new QVideoWidget`，计数器 `m_playersCreated` |
| 绑定 | 同上 `:329-330` | `setAudioOutput` + `setVideoOutput(widget)`，音频初始静音 |
| 换曲 | `playIndex()`（`:656`） | 复用同一个播放器，只 `setSource`，不重建对象 |
| 销毁 | `teardownOutputs()`（`:589`） | 先 `swap` 出整表（防重入二次 `deleteLater`）→ `player->stop()` → `deleteLater` ×3；退出路径下 `sendPostedEvents(DeferredDelete)` 就地兑现 |
| 计数核对 | `:627-633` | 每次卸载打印 `players=创建/销毁`，等值即无泄漏 |

实测（`r_stopresidual`，播放 40 s 后 `stopAll`）：线程 93 → 19 → 6，Private 311 → 95 → 92.9 MB，句柄 2990 → 1418，GPU 三引擎归零。**管线确实完整释放**，残留只剩进程本体。

## 3. 视频输出（窗口）生命周期

1. `QVideoWidget` 以 top-level（`Frameless | Tool | TransparentForInput`）创建，随后挂到 WorkerW；剥离 layered 样式是刻意为之（`desktopmount.cpp:128-130`，Win11 DWM 不合成跨进程分层子窗口）。
2. 窗口失效（挂到 WorkerW 下的子 HWND 被销毁）→ `scheduleMountFix()` 节流 10 s（探测失败时不重置节流，下个 1 s 心跳即重试，把黑屏从最长 10 s 压到约 2 s）→ `remountOutputs()`。这条路径**只换窗口不换播放器**：新建 `QVideoWidget` 后 `player->setVideoOutput(nw)` 重绑，旧 sink 随旧窗口释放（`:518-530`），因为实测旧窗口的 D3D11 交换链会随句柄一起失效，只重建原生窗口会永远收不到画面。

   勘误（2026-09-15）：日志文案「壁纸窗口原生句柄已失效(explorer 重启)」里的括号归因**不成立**。本机 `explorer.exe` 为 PID 17396、启动于 10:01:04，全天未重启，而日志共出现 4 次该事件（12:00:03.484 / 12:30:04.523 / 13:00:07.384 / 13:30:05.546，分属 3 个不同的 Yumeiren 会话），且全部落在整点/半点。→ 触发者是与本进程无关的机器级周期事件，具体来源本机不可考；不要按「Explorer 重启」理解这条路径的触发频率。
3. 分辨率/DPI/热插拔变化 → `scheduleRelayout()` 防抖 600 ms（`:572-587`）→ `layoutOutputs()`，而它**先 `teardownOutputs()` 再全量重建**，随后 `playIndex()` 不带续播位置，等于**从 0 重新起播**。

结构性风险：第 3 条的重建是「重排一次 = 把解码管线拆了重装」，代价是瞬时把提交内存翻倍级别地重开一次。基线里 `b_1080p24` 的 `private_drift = +107.5 MB`（min 270.2 / max 425.4）就是这类重建窗口的形状——旧池未回收、新池已开。正常播放期不触发，但多屏拔线、改缩放、切 `screenMode` 都会触发；`r_switch` 回归进一步确认切换/重建会把 Private 推到 347 MB 峰值（稳态 262~338）。

## 4. 多屏渲染架构

`ScreenMode`（[videowallpaper.h](/C:/Users/rain/Documents/ExplorerBg/Yumeiren/src/videowallpaper.h:23)）：

| 模式 | 输出数 | 解码器数 | 资源关系 |
|---|---|---|---|
| `PrimaryScreen`（默认） | 1 | 1 | 即基线口径 |
| `StretchAll` | 1 | 1 | 单窗口横跨虚拟桌面，呈现像素量按覆盖面积放大 |
| `MirrorAll` | N | **N** | 每屏一份完整三件套，同素材不同步呈现 |

**MirrorAll 是所有资源上最大的乘数**：解码、呈现、帧池、线程全部近似 ×N。代码已有两处配套保护：只有首个输出推进播放列表（防每屏各报一次 `EndOfMedia` 跳两格），只有首个输出调用 `handleUnplayable`。

本机单屏，**多屏线性外推未经实测**，见 `video_performance_regression.md` §6「多屏回归」与 §11 复测建议。

## 5. 视频帧处理链路

### 5.1 数据通路

```text
文件 → QMediaPlayer(FFmpeg) → d3d11va 硬解(GPU VideoDecode 引擎)
     → NV12 纹理留在显存 → QVideoWidget 内置 QRhi/D3D11 呈现器
       (3D 引擎做 NV12→RGB 色彩转换 + 缩放到窗口尺寸)
     → 交换链呈现到 WorkerW 下的子窗口 → DWM 合成到 2560×1600@165Hz
```

### 5.2 帧率旋钮的真实语义

`m_targetFps` 默认 **24**，实现是 `setPlaybackRate`（[videowallpaper.cpp](/C:/Users/rain/Documents/ExplorerBg/Yumeiren/src/videowallpaper.cpp:1050)）：高于 24fps 的素材**按比例放慢播放**，不是丢帧到 24。它确实减少呈现帧数从而省资源，代价是动作变慢。本文所有基线/回归一律 `TargetFps=0`（跟随原生帧率），避免把这个旋钮混进结论。

### 5.3 应用层零帧介入（grep 结论）

在 `src/` 全量搜 `toImage(`、`fromImage`、`QPainter`、`QPixmap`、`QVideoSink`、`setWindowOpacity`、`WA_TranslucentBackground`，命中情况：

| 命中 | 位置 | 是否在视频逐帧路径 |
|---|---|---|
| `QPainter` ×5 | `imageprocess.cpp:124/146/216` 图片壁纸缩放与圆角 | 否（图片处理，一次性） |
| `QPainterPath`、`QPixmap::fromImage` | `mainwindow.cpp:1528/1865` UI 预览与绘制 | 否（UI 操作时一次） |
| `layered` | `desktopmount.cpp:128-130` 注释 | 否（代码是**主动剥离** layered 样式） |
| `setVideoOutput` | `videowallpaper.cpp:330/526/1321` | 是，但只绑内置 `QVideoWidget`，无自定义 sink |

`videowallpaper.cpp` 里没有任何帧回调、帧拷贝、图像转换。**「逐帧 toImage」这一类反模式在本模块不存在，不是优化点。**

## 6. GPU 资源可能产生的位置

| 来源 | 归属引擎 | 实测 | 随什么变化 |
|---|---|---|---|
| 硬解（H.264/AV1/MPEG-4 → NV12） | VideoDecode | 1080p30 10.1%，4K60 61.4% | 帧数 × 源像素 |
| NV12→RGB 色彩转换 + 缩放 + 提交 | 3D | 1080p30 15.4%，4K60 76.7~88.3% | **呈现帧数 × 源像素**，与内容无关 |
| 解码器参考帧/输出帧纹理 | Dedicated 显存 | 1080p 149 MB，4K 434 MB，AV1 4K 910 MB | 源分辨率与编码格式，**与帧率无关** |
| 帧的系统内存映射 | Shared | Dedicated 的 0.7~0.96 倍 | 同上 |
| 每输出的 D3D11 device/交换链 | 3D | 单屏未单独拆分 | 输出数（MirrorAll ×N） |
| GPU 内拷贝 | Copy | FFmpeg 后端**无该引擎实例**；WMF 3.3~11.3% | 换后端才会出现 |

关键实测结论（推导见 `video_gpu_memory_analysis.md`）：3D 在 4K60 已饱和（三种素材都停在 88%），单位帧成本 1080p 0.51% vs 4K 1.92%（≈像素比 4.0），重复帧照付全价。**应用层没有任何一处「白花的 GPU 开销」可省**，剩下的都是「少呈现几帧 / 换小源」的取舍。

## 7. 内存缓存可能产生的位置

| 位置 | 是否本模块可控 | 证据 |
|---|---|---|
| FFmpeg/d3d11va 解码帧池与参考帧队列 | 否（后端内部），随可用核数变化 | 限 4 核后 Dedicated -37%、Private -25% |
| 每输出的音频/视频缓冲、Demux 队列 | 否 | 换曲复用同一播放器，不重开池（`playIndex:656`） |
| Qt 平台/图形后端资源 | 否 | 窗口失效重绑时旧 sink 释放 |
| 应用自建缓存 | **无** | `videowallpaper.*` 无帧缓存、无缩略图缓存、无预加载；`m_playlist` 只是 `QStringList` |
| 应用对象残留 | 需验证 | 创建/销毁计数器 + 停止后残留实测 |
| 线程栈 | 间接 | 播放期 90 上下，停止后收敛到 6；减少线程数会减少提交内存，量级未单独测 |

一句话：**本模块自己不缓存任何东西**，占用几乎全部来自后端帧池，因此「能控的杠杆」只有三个——可用核数（影响池深度）、输出数与源规格（影响池大小）、是否驻留管线（挂起/停止时卸载）。

## 8. 定时器与线程清单

### 8.1 定时器（本模块）

| 定时器 | 间隔 | 动作 | 何时在跑 |
|---|---|---|---|
| `m_fullscreenTimer` | 1000 ms | `evaluateSuspend()` 评估挂起原因 + 单循环进度看门狗 | 仅 `m_started` 期间（`ensureHeartbeatTimers:225`） |
| `m_reclaimTimer` | 30000 ms | `trimMemory()`，且只在 `m_outputs.isEmpty() \|\| m_manualPaused \|\| m_suspendReasons != 0` 时执行（`:169-177`） | 仅 `m_started` 期间 |
| `scheduleRelayout` 防抖 | 600 ms 一次性 | `layoutOutputs()` + 重新起播 | 事件驱动 |
| `scheduleMountFix` | 节流 10 s | `remountOutputs()` | 事件驱动 |
| 长挂起释放 | 180 s 阈值（`YUMEIREN_LONG_SUSPEND_MS` 可覆盖） | `longSuspendRelease()` → 卸载管线 | 挂起期间 |
| 错误/重试推进 | 200 ms、2000 ms 一次性 | `advanceOnError` 等 | 异常路径 |
| `videodiag` 采样器 | 诊断模式才启动 | 写日志 | 正常运行为 no-op |
| 测试钩子 | `YUMEIREN_AUTO_STOP_MS` / `_START_MS` / `_PAUSE_MS` | 驱动 stop/pause 循环 | 只在环境变量 >0 时创建，正常运行零开销 |

停止后 `stopHeartbeatTimers()` 收掉两个心跳定时器，实测残留 6 个线程、GPU 三引擎归零、CPU 0.00~0.02%，**没有空转轮询**。

### 8.2 线程

本模块**不创建任何工作线程**：`videowallpaper.*` 里只有 `VW_ASSERT_GUI()` 断言在 GUI 线程，无 `QThread`/`std::thread`/`QtConcurrent` 派生活跃对象（`QThreadPool` 只出现在 `mainwindow.cpp:1389` 的缩略图生成，与视频路径无关）。播放期的 84~92 个线程全部来自 Qt 平台线程池 + FFmpeg 解码线程 + 音频后端。

| 场景 | 线程数 | 说明 |
|---|---|---|
| 空闲（未播放） | 19 | 进程本体 |
| 播放 1080p30，限 4 物理核 | 90.4 | 基线/验证一致 |
| 播放 1080p30，不限核 | 101.4 | 多出的 11 个即解码线程 |
| 播放 1080p30，强制软解 | 58 | 无 D3D11va 时 FFmpeg 不再开硬件解码线程 |
| 播放 4K60，WMF 后端 | 114.2 | 比 FFmpeg 多 25% |
| `stopAll()` 之后 130 s | 6 | 完整收敛，无残留线程 |

## 9. 已发现的高风险代码

| # | 位置 | 问题 | 状态 |
|---|---|---|---|
| R1 | `desktopmount.cpp:276` 旧 `applyProcessAffinityLimit()` | 掩码 `(1<<4)-1` 在 SMT 机器上覆盖的是 **2 个物理核的 4 个逻辑线程**，与注释「4 核」不符；硬解场景下解码/呈现线程挤在同两个物理核 | **本次已修**，详见 `video_performance_optimization.md` |
| R2 | `videowallpaper.cpp:572-587` `scheduleRelayout()` | 任何几何/DPI 变化都走「拆了重装」全量重建，且 `playIndex()` 不带续播位置 → 播放进度丢失 + 帧池重开峰值 | 未改（触发频率低，改动会牵扯重绑时序） |
| R3 | `videowallpaper.cpp:311` `makeOutput()` | MirrorAll 下每屏一份完整解码管线，无帧率/分辨率补偿；两屏 4K 即两份 4K 成本 | 未改（属产品能力，需多屏实测） |
| R4 | 呈现链路 | 4K60 下 3D 引擎饱和（87.6~88.3%），大概率丢帧，用户侧表现为“卡”而不是“费” | 未改（需换呈现方式，已被历史 A/B 否决） |
| R5 | `videowallpaper.cpp:1291` `trimMemory()` | 实现是 `SetProcessWorkingSetSize(-1)`，**只把工作集换出，不归还提交内存**；暂停段实测 Private 停在 270 MB 不动 | 未改（行为正确，但 UI 文案上“释放内存”容易误解） |
| R6 | 长挂起阈值 180 s | 锁屏/熄屏后最长 3 分钟仍驻留完整管线与显存 | 未改（阈值可用环境变量调） |
| R7 | `videowallpaper.cpp:184-204` 测试钩子 | `YUMEIREN_AUTO_*` 三个环境变量入口编进正式构建 | 保留（不设置变量时零开销，且是本次回归的唯一取证手段） |

没有发现的问题（排除项）：无重入销毁（`teardownOutputs` 先 `swap` 整表）、无退出期 `deleteLater` 不兑现（`flushNow` 路径）、无信号回调访问悬空输出（`isLiveOutput` 只比指针）、无逐帧图像转换、无透明/分层窗口、无播放期空转定时器、静音时不选音轨。

## 10. 优化建议与优先级

| 优先级 | 建议 | 预期收益 | 代价/风险 | 本次是否实施 |
|---|---|---|---|---|
| P0 | 修正亲和性掩码为 N 个不同物理核 | 保留限核带来的内存/显存收益（-25%~-41%），同时把可用算力翻倍（2 物理核→4 物理核） | 几乎无：仍是用户可见的开关，可回退 | **是** |
| P1 | 4K 以上素材默认降帧/降分辨率引导 | 3D 呈本是最大单项，直接砍一半 | 改变用户观感（慢动词语义），属产品决策 | 否（仅建议） |
| P1 | `scheduleRelayout` 改为「复用播放器、只重挂窗口 + 改几何」 | 消除重建峰值（+100 MB 级）与进度丢失 | 需要处理 DPR 变化下交换链重建，回归面大 | 否 |
| P2 | MirrorAll 每屏降规格（非主屏降 fps/分辨率） | 多屏成本近线性下降 | 视觉一致性受损；本机无法验证 | 否 |
| P2 | 缩短长挂起释放阈值（180 s → 30 s） | 锁屏期更快归还显存 | 恢复时重新起播延迟变长 | 否 |
| P3 | `trimMemory()` 文案改为「整理内存」或补充说明 | 只影响用户预期 | 无 | 否（属 UI 文案，超出本任务边界） |
| P3 | 引入 PresentMon/帧间隔探针 | 能把 R4 从推断变确认 | 需外部依赖 | 否 |
| 否决 | 关硬解、换 WMF、自绘 sink、逐帧缩放、双播放器接力 | — | 均有量化反证（见 GPU 文档 §2/§6、历史 A/B） | 否 |

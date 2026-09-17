# 视频壁纸优化改动说明（Yumeiren）

对应任务：`Qt-C++ 视频壁纸模块内存与 GPU 占用优化`。本文只写**实际落地的改动**与其依据；评估后决定不做的方向集中在 §7，避免被误读为已实施。

## 1. 改动清单

| 文件 | 类/函数 | 性质 |
|---|---|---|
| [src/platform/windows/desktopmount.cpp](/C:/Users/rain/Documents/ExplorerBg/Yumeiren/src/platform/windows/desktopmount.cpp:245) | 新增文件级静态 `distinctCoreAffinity(int)`；改写 `fbswin::applyProcessAffinityLimit(int)` | 产品行为修正（核心改动） |
| [src/platform/windows/desktopmount.h](/C:/Users/rain/Documents/ExplorerBg/Yumeiren/src/platform/windows/desktopmount.h:56) | 无签名变化 | 注释与实现语义对齐 |
| [src/main.cpp](/C:/Users/rain/Documents/ExplorerBg/Yumeiren/src/main.cpp:44) | `main()` | 注释 + 诊断日志文案 |
| [src/mainwindow.cpp](/C:/Users/rain/Documents/ExplorerBg/Yumeiren/src/mainwindow.cpp:1074) | 「资源友好模式」复选框 tooltip | 仅文案 |
| [src/videowallpaper.cpp](/C:/Users/rain/Documents/ExplorerBg/Yumeiren/src/videowallpaper.cpp:193) | 构造函数新增 `YUMEIREN_AUTO_PAUSE_MS` 测试钩子 | 仅测试路径，默认不创建 |
| `tools/perf/*.ps1`（未跟踪目录） | 采样工装 | 不参与构建 |

`git diff --stat` 中其余 `src/` 文件（`appinfo.cpp`、`engine.*`、`mainwindow.h`、`tooltipstyle.cpp`、`videodiag.cpp`、`src/core/`）属于**上一个已交付任务「缓存目录迁移」**，与本任务无关，本任务未触碰。

## 2. 修改前存在什么问题

「资源友好模式」的实现是把进程亲和性设为前 N 个逻辑号：

```cpp
const DWORD_PTR mask = (1ULL << maxCores) - 1;   // 改动前，maxCores = 4
```

在开启 SMT 的机器上，Windows 把同一物理核的两个线程**相邻编号**（本机枚举顺序已实测确认：逻辑 0/1 同属物理核 0，2/3 同属物理核 1）。于是：

- 掩码 `0b1111` = 2 个物理核 + 它们的 2 个 SMT 兄弟，而配置项、注释、UI 文案、日志全部写着「限制到 4 个核心」。
- 直接后果不是内存，而是**算力被腰斩**：FFmpeg 硬解的位流解析/CABAC 与 D3D11 命令提交都是 CPU 密集且可并行，全挤在 2 个物理核的同一组执行端口和同一份 L1/L2 上。
- 后果表现为呈现饱和：4K60 的 3D 引擎占用稳定在 87.6%，而按 1080p 阶梯外推需要 ~116%，判定为丢帧（推导见 `video_gpu_memory_analysis.md` §3）。

**「限制核数」本身带来的资源收益是真实的**（三方对照：不限核 Private 406 MB / 显存 237.4 MB / 线程 101.4；限核 314 MB / 149.1 MB / 91），本次没有动它。问题只在掩码选错了核。

## 3. 修改后如何工作

`distinctCoreAffinity(maxCores)`：

1. `GetProcessAffinityMask` 取当前进程可用掩码（尊重系统/其他工具已做的限制，绝不越权）。
2. 两阶段调用 `GetLogicalProcessorInformation`：先取长度，再取全表。
3. 只遍历 `RelationProcessorCore` 条目——**每个条目就是一个物理核**，其 `ProcessorMask` 是它的 SMT 兄弟集合。
4. 对每个物理核取「该核内编号最小且尚未被选中」的逻辑号（`inCore & (~inCore + 1)` 是取最低置位位），凑够 `maxCores` 个即停。
5. 任一环节失败（API 不可用、掩码为空、物理核数不足）返回 `0`，调用方**回退到旧行为** `(1 << N) - 1`。

本机结果：旧 `0b1111`（物理核 0、1）→ 新 `0x55`（逻辑 0/2/4/6 = 物理核 0/1/2/3）。

`applyProcessAffinityLimit()` 的对外契约完全不变：同样的签名、同样的「逻辑核总数 ≤ N 时不设限并返回 false」、同样的调用点（`QApplication` 构造之后、任何播放器/线程创建之前），所以新建的全部线程仍然继承掩码。

## 4. 为什么这能降低资源占用

诚实的回答是：**这一项改动本身不降低占用数字，它消除的是「用吞吐换占用」这笔隐性代价**，并把上一轮遗留的饱和问题解决掉。

| 指标（1080p30，90 s 稳态） | 旧掩码（2 物理核） | 新掩码（4 物理核） | 变化 |
|---|---|---|---|
| Private (MB) | 313.9 | 305.8 | -2.6%（噪声级） |
| 线程 | 91.0 | 90.4 | 持平 |
| 句柄 | 2971.8 | 2960.9 | 持平 |
| Dedicated (MB) | 149.1 | 149.1 | 持平 |
| CPU % | 0.20 | 0.30 | 略升（可用核多了，调度更满） |
| GPU 合计 % | 25.5 | 27.6 | +2.1 |

| 指标（4K60，90 s 稳态） | 旧掩码 | 新掩码 | 变化 |
|---|---|---|---|
| Private (MB) | 673.2 | 667.4 | 持平 |
| Dedicated (MB) | 433.8 | 406.0 | -6.4% |
| 3D % | **87.6** | **76.7** | **-12.4%** |
| VideoDecode % | 61.4 | 64.8 | +3.4 |

机制：物理核从 2 → 4，每个核带来独立 L1/L2 与独立执行端口。解码线程不再互相抢同一份 L1，CPU→GPU 的命令提交能真正并行，于是**同一份工作在更短时间内提交完**：4K60 的 3D 占用从饱和区（87.6%）退到 76.7%，把余量还给 DWM 合成，掉帧风险下降；VideoDecode 反而略升，说明解码器不再被 CPU 侧卡住、单位时间真解了更多帧。

真正「降占用」的那一笔仍然属于既有的限核开关本身（B→C 对照：不限核 → 限核，Private -25%~-32%、Dedicated -37%~-41%、线程 -10%），机制是 FFmpeg 按 `QThread::idealThreadCount()`（受亲和性掩码影响）创建解码线程，每个解码线程各带输入/输出帧缓冲，核少 → 线程少 → 帧池浅。本次改动没有削弱这一笔，同时把它原本偷走的算力还了回来。

## 5. 兼容性与影响面

| 场景 | 行为 |
|---|---|
| 无 SMT 的机器（每物理核 1 逻辑号） | 选出的掩码与旧的「前 N 个逻辑号」完全一致，无变化 |
| 逻辑核总数 ≤ 4 | 提前返回 false，不设限（与改动前相同） |
| `GetLogicalProcessorInformation` 失败 / 物理核不足 N | 回退 `(1 << N) - 1`，即旧行为 |
| 系统策略/其他工具已缩小进程亲和性 | 只在该可用掩码内挑选，绝不扩权 |
| 关闭「资源友好模式」（`video/affinityLimit=false`） | 完全不走这段代码，行为不变 |
| 配置项、键名、默认值 | 未改，无迁移 |
| 线程安全 | 只在 `main()` 里调用一次，无新状态、无新成员、无堆分配（仅一个 `std::vector` 局部缓冲） |
| 图片壁纸、UI、缓存/配置路径 | 未触碰（改动文件清单见 §1） |

唯一用户可感知的差异是两处文案：诊断日志与 tooltip 都改成「4 个逻辑核（尽量分属不同物理核）」，与实现一致。

## 6. 配套的非产品改动（只为取证）

| 项 | 位置 | 说明 |
|---|---|---|
| `YUMEIREN_AUTO_PAUSE_MS` | `videowallpaper.cpp:193-204` | 每 N ms 调一次 `pauseResume()` 并写日志。用于「暂停/恢复 ×100」与「暂停期间是否仍在渲染」两项取证——暂停只有 UI 入口，外部无法稳定驱动。不设置该变量时不创建定时器，零开销 |
| `make_fps_ladder.ps1` | `tools/perf/` | 从同一原片派生 15/24/30/60/60dup/still/4K30/4K60 八档素材，唯一变量是帧率 |
| `run_case.ps1` / `run_matrix.ps1` | `tools/perf/` | 播种 `[video]` 配置 → 启动 → 5 s 采样 → `CloseMainWindow()` 正常关窗 → 还原配置 |
| `run_regression.ps1` | `tools/perf/` | 六个功能回归用例（短程组 + 浸泡组） |
| `agg.ps1` / `cpu_topology.ps1` | `tools/perf/` | 稳态聚合与 SMT 拓扑探测 |

`tools/`、`new-build/`、`docs/` 在 `.gitignore` 内，工装与这些文档不会随 `git add .` 进入提交，需要 `git add -f`。

## 7. 评估后决定不做的方向（含量化理由）

| 方向 | 结论 | 依据 |
|---|---|---|
| 关闭硬件解码换 GPU 数字 | **否决** | CPU 0.20% → 0.90%（×4.5），与「限核」是同一笔钱；任务书也明确禁止 |
| 切 WMF 后端 | **否决** | 1080p 全面更差（Private +13.6%、句柄 +36%）；4K60 数字好看但线程 +25%、句柄 +50%，且 3D/解码呈此消彼长，无证据表明真省 |
| 自绘 video sink 做「上传前缩放」 | **不采用** | 历史 A/B 实测内存 +28%、CPU ×5.5；Qt 也未提供该 API |
| 逐帧 `toImage()` / `QPixmap` 排查 | **无需处理** | 该反模式在本模块不存在（grep 见 `video_performance_audit.md` §5.3） |
| 降帧渲染（真丢帧） | **本次不做** | 现有 `m_targetFps` 是 `setPlaybackRate` 慢动词语义；改成丢帧需要自绘呈现，风险高于收益。列为 P1 建议 |
| 长挂起时不释放显存 / 缩短 180 s 阈值 | **保持现状** | 暂停期实测已归零（见 `video_performance_test.md`），阈值属体验取舍 |
| 重写播放器、双播放器接力、截图掩盖黑帧、改配置/缓存路径 | **未做** | 任务书禁止项，本次改动清单里没有一条涉及 |

顺带记录一个必须澄清的测量事实：`trimMemory()` 的实现是 `SetProcessWorkingSetSize(-1)`，它只把**工作集**换出，**不归还提交内存**。实测暂停段 Working Set 从 293 MB 掉到 2.9 MB，而 Private 稳定在 269~277 MB 不动。因此「Working Set 很小」不等于「内存已释放」，判断回收必须看 Private 与 GPU 显存计数器。

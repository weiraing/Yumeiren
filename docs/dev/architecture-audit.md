# 架构审计报告

## 1. 模块依赖关系

```
main.cpp
  ├── AppConfig (配置加载)
  ├── CachePaths (缓存目录)
  ├── VideoWallpaper::shutdown() (退出收口)
  ├── MainWindow (主窗口)
  └── fbswin:: (单实例锁、亲和性)

MainWindow
  ├── Engine (系统后端)
  ├── ImageProcess (图片处理)
  ├── KanbanController (看板娘控制器)
  ├── SystemTrayController (系统托盘)
  ├── VideoWallpaper (动态壁纸)
  ├── ApplicationRuntimeState (运行状态)
  └── ApplicationShutdown (退出收口)

VideoWallpaper
  ├── AppConfig / ConfigKeys (配置)
  ├── Diagnostics (诊断)
  ├── Engine (路径)
  ├── fbswin:: (桌面挂载)
  └── ApplicationRuntimeState (状态上报)

KanbanController
  ├── KanbanStateMachine (状态机)
  ├── KanbanAnimationClock (动画时钟)
  ├── KanbanModelManager (模型扫描)
  ├── KanbanWindow (窗口)
  ├── PlaceholderRenderer / Live2DRenderer (渲染器)
  ├── AppConfig / ConfigKeys (配置)
  └── Diagnostics (诊断)

SystemTrayController
  ├── ApplicationRuntimeState (状态读取)
  ├── ApplicationShutdown (退出)
  ├── VideoWallpaper (播放控制)
  └── KanbanController (看板娘控制)

Engine
  ├── AppInfo (产品信息)
  ├── CachePaths (路径)
  └── Diagnostics (诊断)
```

## 2. 依赖方向评估

| 规则 | 状态 | 说明 |
|------|------|------|
| UI → 平台层 | PASS | MainWindow → Engine → fbswin::（间接） |
| 平台层 → UI | PASS | fbswin:: 无 UI 依赖 |
| 配置 → MainWindow | PASS | AppConfig 无 UI 依赖 |
| 托盘 → 播放器内部 | PASS | SystemTrayController 调用 VideoWallpaper 公开接口 |
| 看板娘 → 视频壁纸内部 | PASS | 无直接依赖 |
| 控制器暴露底层资源 | PASS | KanbanController 通过 KanbanRenderer 抽象 |

## 3. 模块边界评估

### 3.1 平台适配层（fbswin::）

- 职责：WorkerW 挂载、全屏检测、锁屏检测、电池检测、进程亲和性、单实例锁
- 边界：清晰，所有 Win32 API 调用集中在此
- 问题：无

### 3.2 配置层（AppConfig + ConfigKeys）

- 职责：INI 读写、校验、迁移、延迟保存
- 边界：清晰，业务代码只通过 ConfigKeys 访问
- 问题：无

### 3.3 壁纸模块（VideoWallpaper）

- 职责：播放列表、播放器、多屏输出、挂起策略、错误恢复
- 边界：清晰，通过公开接口被 MainWindow 和 SystemTrayController 调用
- 问题：VideoWallpaperOutput.cpp 和 VideoWallpaperSettings.cpp 是同一类的实现拆分，不是独立模块

### 3.4 看板娘模块（kanban/）

- 职责：状态机、动画时钟、模型管理、渲染器抽象、窗口
- 边界：清晰，KanbanController 是唯一组装者
- 问题：无

### 3.5 系统托盘（SystemTrayController）

- 职责：图标、菜单、状态刷新
- 边界：清晰，调用控制器公开接口
- 问题：无

### 3.6 应用生命周期（app/）

- 职责：产品信息、运行状态、退出收口
- 边界：清晰
- 问题：无

## 4. 已识别的架构改进点

### 4.1 Engine::restartExplorer() 阻塞 GUI 线程

**问题：** `restartExplorer()` 使用 `QThread::msleep` 循环等待 Explorer 进程结束，最长阻塞 5.6 秒。

**影响：** 用户点击「应用」按钮时，如果 Explorer 正在重启，主窗口会冻结。

**建议：** 移到后台线程，或使用 QProcess 异步等待。但这是低频操作（仅在应用图片/效果时触发），风险高于收益。

**决策：** 保持现状。仅在注释中说明阻塞原因。

### 4.2 缩略图缓存无容量上限

**问题：** `CachePaths::thumbnails()` 目录下的文件无清理策略，磁盘使用无限增长。

**影响：** 长期使用后，缩略图可能占用数百 MB 磁盘空间。

**建议：** 添加 LRU 清理或启动时清理超过 N 天的缩略图。

**决策：** 低优先级。不涉及运行时性能，仅磁盘占用。

### 4.3 boxBlur 临时缓冲区过大

**问题：** `ImageProcess::boxBlur()` 为 4K 图分配 ~132MB 临时 `std::vector`。

**影响：** 图片设置预览时可能短暂占用大量内存。

**建议：** 改为行级处理的 in-place separable blur。

**决策：** 低优先级。不在渲染循环中调用，仅设置预览使用。

## 5. 架构优势总结

1. **严格的 GUI 线程模型** — 无跨线程 QWidget 操作
2. **集中式状态管理** — ApplicationRuntimeState 是唯一状态源
3. **统一退出收口** — 四条退出路径执行同一清理序列
4. **渲染器抽象** — Live2D 和 Placeholder 零耦合共存
5. **缓存可移植性** — 全部在 <exe_dir>/.cache，无 AppData 依赖
6. **诊断基础设施** — 启动计时、生命周期日志、资源采样
7. **模块边界清晰** — 依赖方向单一，无循环依赖

## 6. 结论

**架构质量评级：A**

代码库架构成熟，模块边界清晰，依赖方向正确。无需大规模重构。改进点均为低优先级，可在后续迭代中逐步处理。

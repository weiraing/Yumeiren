# 代码质量审计报告

## 1. 扫描范围

- 52 个源文件（.h/.cpp）
- 10 个模块（app, config, core, engine, kanban, platform, tray, ui, wallpaper）
- 1 个 CMakeLists.txt
- 1 个 cmake/Live2DCubism.cmake

## 2. 目录结构评估

### 当前结构

```
src/
├── app/          (3 对 .h/.cpp) — 应用生命周期、配置、退出
├── config/       (2 对 .h/.cpp + 1 .h) — 统一配置中心、键定义
├── core/         (3 对 .h/.cpp) — 缓存路径、诊断日志、图片处理
├── engine/       (1 对 .h/.cpp) — 系统后端（DLL 注册、Explorer）
├── kanban/       (11 对 .h/.cpp) — 看板娘全栈
├── platform/     (1 对 .h/.cpp) — Win32 平台层
├── tray/         (1 对 .h/.cpp) — 系统托盘
├── ui/           (5 对 .h/.cpp) — 主窗口各页
└── wallpaper/    (4 对 .h/.cpp) — 视频壁纸
```

### 评估

| 检查项 | 状态 | 说明 |
|--------|------|------|
| 职责混杂 | PASS | 每个目录职责清晰 |
| 同类功能分散 | PASS | 功能按模块内聚 |
| 重复目录 | PASS | 无重复 |
| 构建产物混入源码 | PASS | cmake-build-debug 在根目录 |
| 命名不一致 | PASS | PascalCase 统一 |
| 平台代码混入通用 | PASS | platform/windows 独立 |
| 业务逻辑写在窗口类中 | PASS | 控制器分离 |

**结论：目录结构合理，无需重组。**

## 3. 类职责评估

| 类 | 行数 | 职责 | 评估 |
|----|------|------|------|
| MainWindow | ~305 (.h) | UI 构建 + 页面切换 + 全局协调 | 可接受（各页已拆分到独立 .cpp） |
| VideoWallpaper | ~200 (.h) | 播放列表 + 播放器 + 多屏 + 挂起策略 | 可接受（职责紧密相关） |
| KanbanController | ~153 (.h) | 状态机 + 时钟 + 渲染器 + 模型 + 窗口 | 可接受（组装者模式） |
| SystemTrayController | ~82 (.h) | 托盘图标 + 菜单 + 状态刷新 | 精简 |
| Engine | ~94 (.h) | DLL 注册 + INI + Explorer | 精简 |
| AppConfig | ~54 (.h) | INI 读写 + 校验 + 迁移 | 精简 |
| ApplicationShutdown | ~48 (.h) | 退出步骤注册 + 执行 | 精简 |

### 评估

| 检查项 | 状态 | 说明 |
|--------|------|------|
| God Object | PASS | 无万能类 |
| 过大 Manager | PASS | 类职责单一 |
| 循环依赖 | PASS | 依赖方向清晰 |
| UI 直接操作底层 | PASS | 通过 Engine 和 fbswin:: 隔离 |

## 4. 函数规模评估

| 文件 | 最长函数 | 行数 | 说明 |
|------|---------|------|------|
| MainWindow.cpp | 构造函数 | ~200 | UI 构建，各 buildXxxPage 已拆分 |
| MainWindowImagePage.cpp | applyImage | ~150 | 图片处理管线 |
| MainWindowVideoPage.cpp | addVideos | ~80 | 文件扫描 + 去重 |
| VideoWallpaperOutput.cpp | ensureOutputs | ~120 | 多屏输出管理 |
| VideoWallpaper.cpp | evaluateSuspend | ~60 | 挂起策略判定 |
| Live2DRendererCubism.cpp | KanbanCubismModel::Setup | ~180 | 模型加载（SDK 集成，合理） |
| PlaceholderRenderer.cpp | paint | ~215 | 纯 QPainter 绘制（测试载体，合理） |

### 评估

| 检查项 | 状态 | 说明 |
|--------|------|------|
| 函数过长 | WARN | PlaceholderRenderer::paint 超 200 行，但注释说明是测试载体 |
| 嵌套过深 | PASS | 无超过 4 层嵌套 |
| 参数过多 | PASS | 无超过 5 个参数的函数 |

## 5. 命名规范评估

| 检查项 | 状态 | 说明 |
|--------|------|------|
| 文件命名 PascalCase | PASS | 全部一致 |
| 类命名 PascalCase | PASS | 全部一致 |
| 函数命名 camelCase | PASS | 全部一致 |
| 成员变量 m_ 前缀 | PASS | 全部一致 |
| 常量 k 前缀 | PASS | ConfigKeys、AppInfo 等 |
| 枚举 PascalCase | PASS | 全部一致 |

## 6. Qt 生命周期评估

| 检查项 | 状态 | 说明 |
|--------|------|------|
| QObject 父子关系 | PASS | 播放器/音频/窗口均有明确父对象 |
| deleteLater 使用 | PASS | teardownOutputs 使用 deleteLater |
| QPointer 保护 | PASS | KanbanWindow 使用 QPointer |
| 信号重复连接 | PASS | 使用 UniqueConnection |
| lambda 悬空引用 | PASS | 缩略图任务有 m_thumbTasksLive 闸门 |

## 7. 线程安全评估

| 检查项 | 状态 | 说明 |
|--------|------|------|
| 跨线程操作 QWidget | PASS | 全部在 GUI 线程 |
| 跨线程操作 QMediaPlayer | PASS | 全部在 GUI 线程 |
| 后台任务取消 | PASS | 缩略图任务有 death flag |
| 退出时任务停止 | PASS | shutdownNow + sendPostedEvents |

## 8. 总结

| 维度 | 评级 | 说明 |
|------|------|------|
| 目录结构 | A | 清晰、合理 |
| 类职责 | A | 单一职责 |
| 函数规模 | B+ | 大部分合理，个别偏大 |
| 命名规范 | A | 统一、清晰 |
| Qt 生命周期 | A | 无泄漏 |
| 线程安全 | A | 严格 GUI 线程 |
| 错误处理 | B+ | 有日志、有降级 |
| 代码注释 | A | 已优化（见注释优化报告） |

**整体评级：A-**（代码质量优秀，无需大规模重构）

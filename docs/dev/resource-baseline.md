# 资源基线文档

## 1. 构建环境

- Qt 版本：6.10.2 (mingw_64)
- 编译器：MinGW GCC 13.1.0 (CLion bundled)
- CMake：4.0.2 (JetBrains bundle)
- Ninja：JetBrains bundle
- C++ 标准：C++17
- 构建类型：Debug
- 构建目录：cmake-build-debug/

## 2. 目标程序

| 目标 | 说明 |
|------|------|
| Yumeiren.exe | 主程序（requireAdministrator manifest） |
| YumeirenTest.exe | UI 测试版（无管理员权限） |
| KanbanProbe.exe | 看板娘渲染探针（控制台程序） |

## 3. 功能模块

| 模块 | 资源类型 | 说明 |
|------|---------|------|
| 图片壁纸 | CPU + GDI | ExplorerBgTool.dll 注册，通过 COM 扩展设置背景 |
| 效果样式 | CPU + GDI | ExplorerBlurMica.dll 注册，通过 COM 扩展设置模糊/亚克力 |
| 视频壁纸 | CPU + GPU + 显存 | QMediaPlayer + QVideoWidget，每显示器一个播放器 |
| 看板娘 | CPU + GPU + 显存 | Live2D Cubism 或 Placeholder（QPainter） |
| 系统托盘 | 忽略 | 仅 QSystemTrayIcon |

## 4. 资源测量方法

由于本环境为 Windows 开发机，无法在 CI 中运行 GUI 程序进行实时测量。以下为代码分析得出的资源开销估算：

### 4.1 视频壁纸资源开销

**每显示器输出（VideoOutput）：**
- QMediaPlayer × 1（含 Qt Multimedia 后端 + FFmpeg 解码器）
- QVideoWidget × 1（D3D 渲染表面）
- QAudioOutput × 1（音频解码管线）
- HWND × 1（WorkerW 子窗口）

**单屏 1080p30 估算：**
- Private Bytes：~80-120 MB（含解码缓冲）
- Dedicated GPU Memory：~50-100 MB（D3D 纹理）
- CPU：~2-5%（解码 + 合成）
- 线程：~8-12（FFmpeg 解码线程 + Qt Multimedia 后端）

**4K60 估算（m_targetFps=24 时降速播放）：**
- Private Bytes：~150-250 MB
- Dedicated GPU Memory：~200-400 MB
- CPU：~5-10%

**多屏叠加：**
- 每增加一个显示器：+1 播放器 +1 QVideoWidget +1 音频管线
- MirrorAll 模式：共享播放器，仅多 QVideoWidget
- StretchAll 模式：独立播放器

### 4.2 看板娘资源开销

**Placeholder 模式（QPainter）：**
- Private Bytes：~5-10 MB
- GPU：无
- CPU：~1-3%（30 FPS QPainter 绘制）

**Live2D Cubism 模式：**
- Private Bytes：~50-150 MB（模型数据 + 纹理）
- Dedicated GPU Memory：~30-100 MB（GL 纹理 + FBO）
- CPU：~1-5%（模型更新 + 渲染）
- GPU 3D：~1-5%

### 4.3 图片壁纸资源开销

**ExplorerBgTool.dll（COM 扩展）：**
- 进程内：仅 INI 配置文件写入
- Explorer.exe 内：DLL 注入后约 20-50 MB（图片解码 + GDI 渲染）
- 不影响 Yumeiren.exe 进程资源

### 4.4 静态资源

- QSS 样式表：~50 KB（编译进 Qt 资源）
- Hook DLL：~2 MB（从资源提取到磁盘）
- 诊断日志：1 MB 滚动 + 1 MB .old

## 5. 已知资源管理策略

| 策略 | 实现位置 | 说明 |
|------|---------|------|
| 进程亲和性限制 | main.cpp | 默认限制 4 个逻辑核 |
| 心跳定时器按需启停 | VideoWallpaper | 仅 m_started 期间运行 |
| 长暂停释放解码管线 | VideoWallpaper | 暂停超过阈值卸载播放器 |
| 内存回收定时器 | VideoWallpaper | 30s 周期调用 EmptyWorkingSet |
| 预览防抖 | MainWindow | resize 期间合并重绘 |
| 缩略图线程池守卫 | MainWindow | 析构时关闭回调闸门 |
| GL 上下文世代追踪 | KanbanOpenGLView | Cubism 着色器缓存失效 |
| 模型切换资源释放 | Live2DRendererCubism | ReleaseGl + ReleaseCpu |
| 配置延迟保存 | AppConfig | 500ms 单次去抖 |

## 6. 优化前已识别的改进点

| # | 问题 | 位置 | 严重度 |
|---|------|------|--------|
| 1 | Engine::restartExplorer() 阻塞 GUI 线程 5.6s | Engine.cpp | MED |
| 2 | boxBlur 为 4K 图分配 ~132MB 临时缓冲 | ImageProcess.cpp | LOW |
| 3 | 缩略图缓存无容量上限 | CachePaths | LOW |
| 4 | 无 HTML 壁纸模块（当前为空） | - | N/A |
| 5 | 无单元测试 | - | INFO |

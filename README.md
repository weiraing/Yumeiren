# 虞美人 (Yumeiren) · FolderBgStudio

[![构建并发布](https://github.com/<你的用户名>/FolderBgStudio/actions/workflows/release.yml/badge.svg)](https://github.com/<你的用户名>/FolderBgStudio/actions/workflows/release.yml)
![平台](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-blue)
![框架](https://img.shields.io/badge/Qt-6.x-green)
![构建](https://img.shields.io/badge/CMake-3.21+-informational)

**虞美人** 是一款面向 Windows 10 / 11 的桌面美化工具,把三件事做进了一个应用:

1. **文件资源管理器背景美化** —— 给文件夹窗口换上自定义图片背景,叠加 Blur / Acrylic / Mica 系统级窗口效果;
2. **视频动态壁纸** —— 把视频挂载到桌面图标层之后播放,带完整的播放列表、智能暂停与资源优化;
3. **看板娘(桌面宠物)** —— 用 Live2D Cubism 模型驱动的无边框透明小窗,常驻桌面、可拖动、可互动。

> 应用在任务栏 / 窗口标题中显示为「Yumeiren」(进程描述为「虞美人」),仓库与旧版本沿用了 **FolderBgStudio** 这一名称,首次运行会自动迁移旧版设置。

---

## ✨ 功能特性

### 🖼 文件夹美化 —— 图片背景

- 一键把任意图片设为**所有文件资源管理器窗口**的背景,支持预设图库(自动扫描 `data/image` 目录,含子目录)与自定义图片/目录,图库带后台缩略图。
- 实时参数调节:旋转、缩放、亮度、对比度、模糊、不透明度。
- 7 种显示位置:填充 / 居中 / 拉伸 / 四角。
- 可选**扩展到文件打开、保存对话框**的背景。
- 可勾选「叠加全窗口效果」:图片覆盖文件列表区,模糊/亚克力覆盖整个窗口,合成完整背景。

### 🎨 文件夹美化 —— 效果样式

- 基于 [ExplorerBlurMica](https://github.com/Maplespe/ExplorerBlurMica) 2.0.1,为资源管理器窗口添加 **Blur / Acrylic / Mica / MicaAlt** 系统级背景效果,亮暗色模式自适应。
- 亮色 / 暗色两套颜色与透明度独立可调。
- 细节开关:清除地址栏背景、清除工具栏背景、清除 WinUI 控件背景、显示分隔线。

### 🎬 动态壁纸 —— 视频壁纸

- 播放列表管理(添加文件 / 扫描 `media/video` 目录)、播放 / 暂停 / 停止、双击立即切换曲目,音量可调。
- 三种互斥播放模式(启动按钮下方"模式"行单选):
  - **单循环**(默认):只播列表里选中的那一个视频,播完从头再来;
  - **列表循环**:按列表顺序一个接一个播,播完最后一个回到第一个,周而复始;
  - **随机**:随机挑一个视频作为壁纸,播完再随机挑下一个,周而复始。
- 视频渲染在**桌面图标层之后**(WorkerW 挂载),不遮挡图标与文件。
- 多屏模式:仅主屏 / 拉伸所有屏幕 / 镜像所有屏幕,支持显示器拓扑变化后自动重挂载。
- **智能暂停**:前台全屏应用、系统锁定、显示器熄屏、电池供电、桌面被完全遮挡时自动挂起,恢复后自动续播。
- **资源友好**:
  - 帧率上限默认 24 fps(高帧率素材以慢动作播放),4K60 素材内存/显存约 **-40%**;
  - 「限制逻辑核」默认开启(限前 4 个逻辑核),实测 1080p30 内存约 **-27%**、显存约 **-36%**;
  - 长时间挂起自动卸载解码管线,恢复时跳回暂停位置;
  - 走 QVideoWidget GPU 渲染路径,播放期间内存平稳。
- **可靠性**:单视频无缝循环、坏曲目自动跳过并进入失败名单、资源管理器重启后自动修复挂载、单实例守卫(重复启动唤起已有窗口)。
- 支持开机自启;内置诊断模式(`YUMEIREN_DIAG=1`)输出分阶段日志,便于排查问题。

### 🎎 看板娘(桌面宠物)

- 基于 **Live2D Cubism Native SDK 5 R.5(Core 6.0.1)** 渲染,无边框 + 逐像素透明的置顶小窗,不进任务栏、不抢焦点;关闭即隐藏,反复启停不重建窗口。
- **完整还原模型能力**:待机动作循环、点击/悬停反馈、视线跟随鼠标、表情切换、物理与姿态(`Physics` / `Pose` / `EyeBlink` / `Breath` / `Look` 全部挂到模型上)。
- **命中区联动**:点角色的**头部**给表情反馈(随机且避开当前这张),点**身体**播 `TapBody` 一类的动作组 —— 命中区名字对不上时退化为表情,保证「点了必有反应」。
- **三个入口都能切表情**:右键菜单「切换表情」、设置页按钮、以及点击头部。模型没有表情文件时入口自动置灰并说明原因,不做成死按钮。
- 模型管理:自动扫描 `<程序目录>/data/models`,每个子目录一个模型,带校验(贴图/动作组/表情计数),可一键切换或随机换下一个可用模型。
- 缩放(滚轮/滑块)、不透明度、帧率上限、窗口置顶、鼠标穿透、允许点击互动,全部即时生效并落盘。
- **降级路径**:Live2D 不可用时自动切到内置的 QPainter 占位角色(会呼吸、眨眼、跟随视线,也有表情),界面行为不因后端不同而缩水。
- 与视频壁纸一致:主界面隐藏时暂停动画、隐藏即停时钟,长时间不使用不空转。

### 🌐 动态网页壁纸

🚧 功能开发中,当前版本为界面预览。

---

## 📦 安装使用

1. 到 [Releases](../../releases) 下载 `Yumeiren-portable-vX.X.X-win64.zip`,解压到任意目录(免安装,Qt 运行时已内置)。
2. 运行 `Yumeiren.exe`,同意 UAC 管理员授权(写入配置、注册 DLL、重启资源管理器均需要)。
3. 在「文件夹美化」页选择图片与效果,点击**应用**,打开任意文件夹查看效果;不想要了点击**恢复**即可完全卸载。

> ⚠️ 程序需以管理员身份运行;杀软若误报属于此类 shell 注入工具的常见情况,可添加信任。

### 常见问题

| 现象 | 处理方式 |
| --- | --- |
| Windows 大版本更新后背景消失 | 重新点击「应用」即可 |
| 资源管理器窗口无法打开 | 按住 **ESC** 键点击资源管理器可跳过背景加载,然后在本工具点击「恢复」 |
| 「主页 / 图库」页看不到背景 | 属正常现象,进入任意文件夹查看 |

---

## 🛠 从源码构建

### 环境要求

- Windows 10 / 11 x64
- CMake ≥ 3.21
- Qt 6(需 **Widgets、Multimedia、MultimediaWidgets、OpenGL、OpenGLWidgets** 模块;CI 使用 6.10.2 + MSVC 2022 64 位,MinGW 亦可)
- MSVC 2022 或 MinGW-w64 工具链
- 支持 OpenGL 3.3 及以上的显卡驱动(看板娘用;不可用时自动降级为软件占位渲染)

### 获取第三方 DLL

仓库出于许可与体积考虑**不提交**这两个 DLL,构建前请从上游项目下载并放入 `resources/dlls/`(文件名保持一致):

| 文件 | 来源 |
| --- | --- |
| `resources/dlls/ExplorerBgTool.dll` | [Maplespe/explorerTool](https://github.com/Maplespe/explorerTool) · [SuryaMajumdar/ExplorerBgTool](https://github.com/SuryaMajumdar/ExplorerBgTool) |
| `resources/dlls/ExplorerBlurMica.dll` | [Maplespe/ExplorerBlurMica](https://github.com/Maplespe/ExplorerBlurMica)(官方 2.0.1) |

### Live2D 看板娘(可选能力)

看板娘依赖 Live2D Cubism Native SDK。仓库把 SDK 源码放在 `third_party/` 下**原样保留、不做任何修改**——集成只发生在 `src/kanban/Live2DRendererCubism.cpp` 这一层,第三方代码始终可以直接用上游版本整体替换。

```
third_party/
├── cubism/            # Cubism Native SDK 5 R.5(Framework + Core + 官方 OpenGL 后端)
└── glew/              # GLEW 2.3.1(CubismRenderer_OpenGLES2 在 Windows 上硬依赖)
```

CMake 默认会**探测**这两个目录:存在就自动打开 `YUMEIREN_WITH_LIVE2D`,不存在就退回占位渲染器,不阻塞构建。也可显式指定:

```bash
cmake -S . -B build -DYUMEIREN_WITH_LIVE2D=ON
```

构建期需要额外注意的两点(都已由 CMake 自动处理,这里写明原因,便于排障):

- **运行期着色器**:Cubism 在第一次绘制时从磁盘读 `FrameworkShaders/*.vert|frag`,所以 CMake 会把 `third_party/cubism/Framework/src/Rendering/OpenGL/Shader/` 整个拷到 `<可执行文件目录>/FrameworkShaders`。缺了这些文件,表现是模型完全不出图且没有任何报错。
- **`QT_DISABLE_SHADER_DISK_CACHE=1`**:`main()` 里在构造 `QApplication` 之前设的。Qt 的 GL 上屏路径会初始化一个「已编译着色器二进制磁盘缓存」,而它的加载是**持锁做文件读写**的;读盘一旦被卡住(权限受限、网络盘、杀软或沙箱拦截),这把锁就永不释放,主线程随后在 `QPlatformBackingStore::rhiFlush` 里等它——整个 GUI 线程死锁,且一行日志都不会有。关掉它只是让 Qt 每次重新编译自己那几个上屏着色器(毫秒级)。

看板娘出问题时的第一手工具:

| 工具 | 用途 |
| --- | --- |
| `KanbanProbe --frames 60 --model <model3.json> --out x.png` | 把同一条渲染链路搬到离屏上下文里跑,输出覆盖率、`glGetError`、一张 PNG,并做**表情通道自检**(切表情后逐像素比对,给出「动画漂移解释不了的变化」像素数) |
| `tools/winprobe/grab_screen.py <png> [x y w h]` | 抓屏(纯 ctypes + GDI),看板娘是逐像素透明置顶窗,只有合成后的画面能证明「模型真的出来了」 |
| `tools/winprobe/check_alive.py` | 用 `SendMessageTimeout(WM_NULL, SMTO_ABORTIFHUNG)` 判断 GUI 线程是否卡死 |
| `tools/winprobe/enum_windows.py` | 枚举顶层窗口(可见性 + 矩形),看板娘没有标题,尺寸是它唯一的稳定特征 |
| `tools/winprobe/kanban_click_probe.py` | 启动应用 → 找到看板娘窗口 → 向头部/身体各发一次左键 → 回读日志里的命中结果 |
| `tools/winprobe/kanban_menu_probe.py` | 发一次右键 → 抓下弹出菜单(验证菜单项是否漏了、是否该置灰) |
| `tools/winprobe/kanban_page_probe.py` | 启动应用 → 点侧边栏进「看板娘」页 → 抓图(验证按钮是否存在、可用性是否正确) |

以上工具都只依赖 Python 标准库与 `ctypes`,不需要 `pip install` 任何东西。注意 `tools/` 被 `.gitignore` 排除(未入库),干净检出里没有这些脚本。

### 编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target Yumeiren
```

部署运行时(生成免安装目录):

```bash
windeployqt --release --no-translations --compiler-runtime build/Release/Yumeiren.exe
```

### 构建目标

| 目标 | 说明 |
| --- | --- |
| `Yumeiren` | 正式版,内嵌 `requireAdministrator` 清单与版本资源 |
| `YumeirenTest` | 无提权清单,供自动化 UI 测试使用 |
| `KanbanProbe` | 看板娘(Live2D)渲染管线自检探针(非产品组件,源码不在仓库时自动跳过) |
| `SinkProbe` | QVideoSink 内存归因实验探针(非产品组件) |

### CI 自动发布

`.github/workflows/release.yml` 会在推送 `v*` 标签时自动:安装 Qt 6.10 → CMake 编译 → `windeployqt` 组装便携版 zip → 创建 GitHub Release 并上传。也可在 Actions 页面手动触发(仅上传 Artifact)。

---

## 📁 配置与数据目录

| 内容 | 位置 |
| --- | --- |
| 应用设置(统一配置中心,便携式 INI) | `<程序目录>/config/.ini` |
| 缓存(图库缩略图、处理后背景图、诊断日志、临时文件) | `<程序目录>/.cache` ([说明](docs/cache_directory.md)) |
| Hook DLL 及其 `config.ini`(不是缓存,勿随缓存清理) | `<程序目录>/dll` |
| 看板娘 Live2D 模型(每个模型一个子目录,内含 `*.model3.json`) | `<程序目录>/data/models` |
| Cubism 运行期着色器(构建时从 `third_party` 拷入,勿手工删) | `<程序目录>/FrameworkShaders` |
| 开机自启 | 注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 值 `Yumeiren` |

配置中心带启动校验修复与 500ms 延迟批量保存;旧版(`FolderBgStudio` / `YuMeiren`)的注册表配置、背景缓存与自启项会在首次运行时一次性迁移,详见 [docs/CONFIG_PERSISTENCE.md](docs/CONFIG_PERSISTENCE.md)。

Hook DLL 的绝对路径登记在 `HKLM` 的 CLSID 里。从旧版(`%LOCALAPPDATA%\Yumeiren\dll`)升级后,注册表仍指向旧路径,
软件会在日志区提示"旧目录注册(需重新应用)":点一次「应用图片背景」或「应用特效」(需同意 UAC)即可完成迁移注册。
旧的 `%LOCALAPPDATA%\Yumeiren\dll` 不会被删除,确认功能正常后可手工清理。

---

## 📚 设计文档

视频壁纸子系统的设计、测试与性能归因文档位于 [`docs/`](docs/):

- [视频壁纸状态机](docs/VIDEO_WALLPAPER_STATE_MACHINE.md) · [错误处理](docs/VIDEO_WALLPAPER_ERROR_HANDLING.md) · [系统事件](docs/VIDEO_WALLPAPER_SYSTEM_EVENTS.md) · [媒体兼容性策略](docs/VIDEO_MEDIA_COMPATIBILITY_POLICY.md) · [循环边界抖动分析](docs/VIDEO_LOOP_BOUNDARY_JITTER_ANALYSIS.md)
- [资源配置优化](docs/RESOURCE_OPTIMIZATION.md) · [多显示器资源分析](docs/MULTI_MONITOR_VIDEO_RESOURCE_ANALYSIS.md) · [性能基线与对比](docs/perf/perf-comparison.md)
- [配置持久化](docs/CONFIG_PERSISTENCE.md) · [诊断模式](docs/VIDEO_WALLPAPER_DIAGNOSTIC_MODE.md) · [回归测试](docs/VIDEO_WALLPAPER_REGRESSION_TESTS.md)

技术栈:C++17 · Qt 6 Widgets/Multimedia · Win32(DWM / WorkerW / Shell 扩展注册),UI 与系统引擎(`Engine`)分层。

---

## 🙏 致谢与许可

本工具整合了以下开源项目的能力,仅调用其官方 DLL 并生成配置:

| 项目 | 许可 | 用途 |
| --- | --- | --- |
| [SuryaMajumdar/ExplorerBgTool](https://github.com/SuryaMajumdar/ExplorerBgTool) | MIT | 图片背景 |
| [Maplespe/explorerTool](https://github.com/Maplespe/explorerTool) | MIT | 图片背景 |
| [Maplespe/ExplorerBlurMica](https://github.com/Maplespe/ExplorerBlurMica) | LGPL-3.0 | Blur / Acrylic / Mica 效果 |
| [Live2D Cubism Native SDK](https://www.live2d.com/en/sdk/download/native/) | [Live2D Proprietary Software License](https://www.live2d.com/en/sdk/license/) | 看板娘渲染(Core 6.0.1) |
| [GLEW](https://github.com/nigels-com/glew) | MIT / BSD | OpenGL 扩展加载 |

`third_party/` 下的 Cubism SDK 与 GLEW **保持上游原样,本项目不修改其中任何文件**;所有适配都写在 `src/kanban/` 内。使用 Live2D Cubism SDK 需遵守其专有许可(个人/小规模事业者免费额度、发行前需确认最新条款)。

本项目自身代码以 MIT 许可发布;因集成了 LGPL-3.0 组件,再分发时请同时遵守上述许可条款。

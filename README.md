# 虞美人 (Yumeiren) · FolderBgStudio

[![构建并发布](https://github.com/<你的用户名>/FolderBgStudio/actions/workflows/release.yml/badge.svg)](https://github.com/<你的用户名>/FolderBgStudio/actions/workflows/release.yml)
![平台](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-blue)
![框架](https://img.shields.io/badge/Qt-6.x-green)
![构建](https://img.shields.io/badge/CMake-3.21+-informational)

**虞美人** 是一款面向 Windows 10 / 11 的桌面美化工具,把两件事做进了一个应用:

1. **文件资源管理器背景美化** —— 给文件夹窗口换上自定义图片背景,叠加 Blur / Acrylic / Mica 系统级窗口效果;
2. **视频动态壁纸** —— 把视频挂载到桌面图标层之后播放,带完整的播放列表、智能暂停与资源优化。

> 应用在任务栏 / 窗口标题中显示为「Yumeiren」(进程描述为「虞美人」),仓库与旧版本沿用了 **FolderBgStudio** 这一名称,首次运行会自动迁移旧版设置。

---

## ✨ 功能特性

### 🖼 文件夹美化 —— 图片背景

- 一键把任意图片设为**所有文件资源管理器窗口**的背景,支持预设图库(自动扫描 `media/image` 目录)与自定义图片/目录,图库带后台缩略图。
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
- Qt 6(需 **Widgets、Multimedia、MultimediaWidgets** 模块;CI 使用 6.10.2 + MSVC 2022 64 位,MinGW 亦可)
- MSVC 2022 或 MinGW-w64 工具链

### 获取第三方 DLL

仓库出于许可与体积考虑**不提交**这两个 DLL,构建前请从上游项目下载并放入 `resources/dlls/`(文件名保持一致):

| 文件 | 来源 |
| --- | --- |
| `resources/dlls/ExplorerBgTool.dll` | [Maplespe/explorerTool](https://github.com/Maplespe/explorerTool) · [SuryaMajumdar/ExplorerBgTool](https://github.com/SuryaMajumdar/ExplorerBgTool) |
| `resources/dlls/ExplorerBlurMica.dll` | [Maplespe/ExplorerBlurMica](https://github.com/Maplespe/ExplorerBlurMica)(官方 2.0.1) |

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

本项目自身代码以 MIT 许可发布;因集成了 LGPL-3.0 组件,再分发时请同时遵守上述许可条款。

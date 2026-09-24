<p align="center">
  <img src="resources/icons/yumeiren-256.png" alt="虞美人" width="160">
</p>

# 虞美人 · Yumeiren

[![构建并发布](https://github.com/weiraing/Yumeiren/actions/workflows/release.yml/badge.svg)](https://github.com/weiraing/Yumeiren/actions/workflows/release.yml)
[![最新版本](https://img.shields.io/github/v/release/weiraing/Yumeiren?color=2F6FD6&label=release)](https://github.com/weiraing/Yumeiren/releases)
![平台](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078D4)
![Qt](https://img.shields.io/badge/Qt-6.10-41CD52)
![C++](https://img.shields.io/badge/C%2B%2B-17-00599C)
[![许可](https://img.shields.io/badge/license-MIT-97CA00)](LICENSE)

**虞美人**是一个面向 Windows 10 / 11 的桌面美化工具，把三件通常要装三个软件的事做进了同一个应用：

<table>
<tr>
<td width="33%" valign="top">

**🖼 文件夹背景美化**

给每个资源管理器窗口换上自定义图片背景，并叠加 Blur / Acrylic / Mica 系统级窗口效果。

</td>
<td width="33%" valign="top">

**🎬 视频动态壁纸**

把视频挂载到桌面图标层之后播放，带播放列表、播放模式、智能暂停与资源优化。

</td>
<td width="33%" valign="top">

**🎎 看板娘**

Live2D Cubism 驱动的无边框透明桌面宠物，常驻桌面、可拖动、可互动、会看向你的鼠标。

</td>
</tr>
</table>

## 简介

用 C++17 与 Qt 6 写成，三件事共用一套主界面与系统托盘。**免安装、便携** —— 配置、缓存、素材全部落在程序目录下，拷走整个文件夹就带走全部设置。因为要注册资源管理器 Hook DLL、重启资源管理器，程序**需要管理员权限**（清单里写死了 `requireAdministrator`）。

程序在任务栏与窗口标题中显示为 `Yumeiren`，进程描述为「虞美人」。

## 效果预览

<table>
<tr>
<td width="50%" align="center"><img src="static/ui-image.png" alt="文件夹美化 · 图片背景"><br><sub>文件夹美化 · 图片背景</sub></td>
<td width="50%" align="center"><img src="static/ui-effect.png" alt="文件夹美化 · 效果样式"><br><sub>文件夹美化 · 效果样式</sub></td>
</tr>
<tr>
<td width="50%" align="center"><img src="static/effect-explorer-image.png" alt="资源管理器 · 图片背景"><br><sub>资源管理器 · 图片背景</sub></td>
<td width="50%" align="center"><img src="static/effect-explorer-mica.png" alt="资源管理器 · 叠加 Acrylic"><br><sub>资源管理器 · 叠加 Acrylic</sub></td>
</tr>
<tr>
<td width="50%" align="center"><img src="static/ui-kanban.png" height="340" alt="看板娘 · 设置页"><br><sub>看板娘 · 设置页</sub></td>
<td width="50%" align="center"><img src="static/effect-kanban.jpg" height="340" alt="看板娘 · 桌面实拍"><br><sub>看板娘 · 桌面实拍</sub></td>
</tr>
</table>

## 快速开始

1. 到 [Releases](https://github.com/weiraing/Yumeiren/releases) 下载 `Yumeiren-portable-vX.X.X-win64.zip`，解压得到 `Yumeiren/` 目录（Qt 运行时与 MSVC 运行库已内置）。
2. 运行 `Yumeiren/虞美人.exe`，同意 UAC 管理员授权。
3. 在「文件夹美化」页选图片与效果，点击**应用**；不想要了点击**恢复**即可完全卸载。

> ⚠️ 杀软若误报，属于此类 shell 注入工具的常见情况，可添加信任。

**使用须知**

- **单实例**：会话级命名互斥体 `Local\Yumeiren.single-instance` 做守卫。已经开着时再双击不会开出第二个实例，而是**唤起已有窗口**后自己退出。
- **素材随仓库提供**：`data/` 带了一套可直接用的示例素材（约 79 MB：4 个 Live2D 模型、图片图库、视频、19 套网页壁纸），换成自己的只需替换对应子目录 —— 程序按**固定目录结构**扫描，不需要清单文件。第三方素材**版权归各作者，分发或商用前请逐个确认授权**。
- **看板娘无表情时**：部分模型本身不含表情文件，相关入口会自动置灰 —— 是素材问题，不是功能故障。

**常见问题**

| 现象 | 处理方式 |
| --- | --- |
| Windows 大版本更新后背景消失 | 重新点击「应用」 |
| 资源管理器窗口无法打开 | 按住 **ESC** 键点击资源管理器可跳过背景加载，然后点击「恢复」 |
| 「主页 / 图库」页看不到背景 | 属正常现象，进入任意文件夹查看 |
| 提示「旧目录注册（需重新应用）」 | Hook DLL 路径登记在 `HKLM` 的 CLSID 里，程序换过目录后点一次「应用」即可重新注册 |
| 看板娘不显示模型 | 检查 `data/models/<模型名>/*.model3.json` 与贴图；再看 `.cache/logs/videowallpaper.log` |

## 从源码构建

### 依赖

| 依赖 | 必需？ | 说明 |
| --- | :---: | --- |
| **Windows 10 / 11 x64** | ✅ | 项目大量使用 Win32（DWM / WorkerW / Shell 扩展注册） |
| **CMake ≥ 3.21** | ✅ | 用到 `qt_add_executable`、`$<TARGET_RUNTIME_DLLS>` |
| **Qt 6.10.2**（Widgets / Multimedia / MultimediaWidgets） | ✅ | CI 用 MSVC 2022 64 位；MinGW 亦可 |
| **MSVC 2022 或 MinGW-w64** | ✅ | 二选一 |
| `third_party/cubism` + `third_party/glew` | ⬜ | 决定有没有**真 Live2D**；缺失则自动降级为占位角色，**不影响编译** |
| 支持 OpenGL 3.3 的显卡驱动 | ⬜ | 看板娘用；不可用时自动降级为软件占位渲染 |

> `resources/dlls/` 下的两个 Hook DLL 与 `data/` 素材均**随仓库提供**，无需额外下载。

### 编译

安装 [Qt 6.10.2](https://www.qt.io/download-qt-installer)，勾上与你编译器一致的 **MSVC 2022 64-bit** 或 **MinGW 13.1 64-bit**、**Qt Multimedia**（视频壁纸必需）与 **Qt Shader Tools**（Qt 6 的 RHI 上屏路径依赖它），然后：

```bash
git clone https://github.com/weiraing/Yumeiren.git
cd Yumeiren
```

关键是让 CMake 找到 Qt —— 用 `CMAKE_PREFIX_PATH` 指向 Qt 安装目录。两条路线任选其一：

```bat
:: 路线 A：MSVC 2022（与 CI 一致，推荐）
::   在「x64 Native Tools Command Prompt for VS 2022」里执行
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.10.2/msvc2022_64"
cmake --build build --config Release --target Yumeiren
::   产物：build/Release/虞美人.exe

:: 路线 B：MinGW + Ninja（开发期迭代快）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.10.2/mingw_64" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build --target Yumeiren
::   产物：build/虞美人.exe
```

CMake 会自动把 Qt 模块 DLL 与 `platforms/qwindows.dll` 插件拷到 exe 旁边，**构建完可直接双击运行**，不需要先手工跑 `windeployqt`。

| 目标 | 提权 | 用途 |
| --- | :---: | --- |
| `虞美人.exe` | 需管理员 | 正式版，内嵌 `requireAdministrator` 清单与版本资源 |
| `YumeirenTest.exe` | 不需要 | 不带提权清单，供自动化 UI 测试 |

> 两个 exe 共用同一份 `config/.ini` 与 `.cache/`，**不要同时运行**。

### 第三方依赖与素材

`data/` 随仓库提供，通常不用管。**只有想要真 Live2D 看板娘**时才需要准备 `third_party/`：

```bash
# Cubism Native SDK 5 R.5（只搬 Core/ 与 Framework/，跳过占 29MB 的 Samples/）
curl -LO https://cubism.live2d.com/sdk-native/bin/CubismSdkForNative-5-r.5.zip
unzip CubismSdkForNative-5-r.5.zip && mkdir -p third_party/cubism
cp -r CubismSdkForNative-5-r.5/{Core,Framework,LICENSE.md,NOTICE.md,cubism-info.yml} third_party/cubism/

# GLEW 2.3.1（只要 include/ 与 src/）
curl -LO https://github.com/nigels-com/glew/releases/download/glew-2.3.1/glew-2.3.1.zip
unzip glew-2.3.1.zip && mkdir -p third_party/glew
cp -r glew-2.3.1/{include,src,LICENSE.txt} third_party/glew/
```

两处都摆好 → CMake **默认打开** `YUMEIREN_WITH_LIVE2D`；任一缺失 → **默认关闭**，编译占位渲染，构建照常通过。SDK 也可以放仓库外，用 `-DYUMEIREN_CUBISM_SDK=<路径>` / `-DYUMEIREN_GLEW_ROOT=<路径>` 指过去。

> `third_party/`、`tools/`、`docs/` 与 `build*/` 都在 `.gitignore` 里，**`data/` 不在排除之列** —— 素材是随仓库提供的。MinGW 下 Cubism Core 只有 MSVC 的 `.lib`/`.dll`，本项目把 `.dll` 直接放到链接行让 GNU ld 读导出表（分岔点在 `cmake/Live2DCubism.cmake`）；**MSVC 那条路只在 CI 上跑过**。SDK 受**专有许可**约束，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

### CMake 选项

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `CMAKE_PREFIX_PATH` | 空 | **必填**，指向 Qt 安装目录 |
| `YUMEIREN_VERSION` | 空 | 显式指定版本号，覆盖 git 标签与 `VERSION` 文件 |
| `YUMEIREN_WITH_LIVE2D` | 探测 | 显式 `ON`/`OFF` 覆盖探测结果；传 `OFF` 可验证降级路径 |
| `YUMEIREN_CUBISM_SDK` | `third_party/cubism` | Cubism SDK 根目录（含 `Framework/` 与 `Core/`） |
| `YUMEIREN_GLEW_ROOT` | `third_party/glew` | GLEW 源码根目录（含 `include/GL/glew.h` 与 `src/glew.c`） |
| `CMAKE_BUILD_TYPE` | 空 | 仅单配置生成器（Ninja / Makefile）需要，用 `Release` |

> 改了选项记得**删掉 `build/CMakeCache.txt`** 或换一个新构建目录。

### 排错

| 现象 | 处理 |
| --- | --- |
| `Could not find a package configuration file provided by "Qt6"` | 补 `-DCMAKE_PREFIX_PATH="C:/Qt/6.10.2/<你的编译器>"` |
| `YUMEIREN_WITH_LIVE2D=ON，但没找到 Cubism Framework` | 摆好 SDK，或改 `-DYUMEIREN_WITH_LIVE2D=OFF` |
| 编译全绿，**双击 exe 毫无反应、退出码 127** | 缺 Qt DLL —— 用本项目 CMake 构建（它会自动拷），别手工抄 DLL 清单 |
| 模型**完全不出图、也没有报错** | `<程序目录>/FrameworkShaders` 缺失，重新构建 |
| 界面中文变乱码 | 编译器按系统 ANSI 码页解释源码；CMake 已加 `/utf-8`（MSVC）与 `windres -c 65001`（MinGW） |

### 版本号与发版

版本号唯一来源是根目录的 `VERSION` 文件（一行 `X.Y.Z`），configure 阶段解析一次后流向 `project()`、exe 版本资源、清单与 `appinfo::version()`。优先级：`-DYUMEIREN_VERSION` > git 标签指向 HEAD > `VERSION` 文件。**改版本号就改 `VERSION`** —— `resources/*.in` 与 `cmake/*.h.in` 只是模板。

```bash
git commit -am "发布 1.0.1"
git tag v1.0.1
git push origin master --tags   # CI 自动编译 + 校验 exe 版本资源 + 创建 Release
```

> 推送 `v*` 标签触发完整发布流程；Actions 页面手动触发只上传 Artifact、不建 Release。CI 的版本断言与打包断言见 [`.github/workflows/release.yml`](.github/workflows/release.yml)。

## 配置与数据目录

一切以**可执行文件所在目录**为基准（便携式，与当前工作目录无关）：

| 内容 | 位置 |
| --- | --- |
| 应用设置（统一配置中心，便携式 INI） | `<程序目录>/config/.ini` |
| 缓存（图库缩略图、模型预览图、处理后背景图、诊断日志） | `<程序目录>/.cache` |
| Hook DLL 及其 `config.ini`（**不是缓存，勿随缓存清理**） | `<程序目录>/dll` |
| 看板娘 Live2D 模型（每个子目录一个模型） | `<程序目录>/data/models` |
| 图片图库 / 动态壁纸默认视频目录（均递归扫描） | `<程序目录>/data/image`、`<程序目录>/data/video` |
| Cubism 运行期着色器（构建时拷入，**勿手工删**） | `<程序目录>/FrameworkShaders` |
| 开机自启 | 注册表 `HKCU\...\CurrentVersion\Run` 值 `Yumeiren` |

首次运行会自动生成配置并补齐默认值，配置带 **500ms 延迟批量保存**。日志在 `<程序目录>/.cache/logs/videowallpaper.log`；**Debug 级日志只在设置 `YUMEIREN_DIAG=1` 后才落盘**。

## 项目结构

```
Yumeiren/
├── src/
│   ├── main.cpp                  # 入口：着色器缓存开关、单实例、配置加载、亲和性
│   ├── app/                      # 产品信息、运行状态、退出收口
│   ├── config/                   # AppConfig（INI 读写/校验/延迟保存）+ ConfigKeys
│   ├── core/                     # 跨模块共享：CachePaths / Diagnostics / SuspendPolicy
│   ├── explorerbg/               # 文件夹美化：Engine(DLL 注册) / ImageProcess / 图片页 / 效果页
│   ├── kanban/                   # 看板娘：控制器 / 状态机 / 窗口 / 渲染器 / 模型管理 / 设置页
│   ├── platform/windows/         # fbswin:: WorkerW 挂载、全屏/锁屏/电池检测、单实例
│   ├── tray/                     # 系统托盘控制器
│   ├── ui/                       # 主窗口壳 + 共享小部件(LibraryCard / TooltipStyle / UiStyle)
│   └── wallpaper/                # 动态壁纸：视频 / 网页壁纸播放器 + 壁纸页 UI
├── cmake/                        # Version.cmake / Live2DCubism.cmake / *.h.in 模板
├── resources/                    # app.rc.in · app.manifest.in · style.qss · light.qss · icons/ · dlls/
├── static/                       # README 配图（界面图 / 效果图）
├── data/                         # 素材（随仓库提供）：models / image / video / web
├── third_party/                  # Cubism SDK + GLEW（不入库，只读，保持上游原样）
├── tools/ · docs/                # 验证探针与开发期脚本 / 设计与审计文档（均不入库）
├── VERSION                       # ★ 版本号唯一来源（一行 X.Y.Z）
├── CMakeLists.txt · LICENSE · THIRD_PARTY_NOTICES.md
└── .github/workflows/release.yml # 推 v* 标签时编译 + 校验 exe 版本资源 + 发便携包
```

## 致谢

自有代码以 [MIT](LICENSE) 发布；整合的第三方组件各自受其原始许可约束，再分发时需同时遵守（详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)）：

| 项目 | 许可 | 用途 |
| --- | --- | --- |
| [SuryaMajumdar/ExplorerBgTool](https://github.com/SuryaMajumdar/ExplorerBgTool) | MIT | 图片背景 |
| [Maplespe/explorerTool](https://github.com/Maplespe/explorerTool) | MIT | 图片背景 |
| [Maplespe/ExplorerBlurMica](https://github.com/Maplespe/ExplorerBlurMica) | **LGPL-3.0** | Blur / Acrylic / Mica 效果 |
| [Live2D Cubism Native SDK](https://www.live2d.com/en/sdk/download/native/) | [Live2D 专有许可](https://www.live2d.com/en/sdk/license/) | 看板娘渲染（Core 6.0.1） |
| [GLEW](https://github.com/nigels-com/glew) | MIT / BSD | OpenGL 扩展加载 |
| [Qt 6](https://www.qt.io/) | LGPL-3.0 / 商业 | 界面与多媒体框架（动态链接使用） |

- `third_party/` 下的 Cubism SDK 与 GLEW **保持上游原样，本项目不修改其中任何文件**，所有适配都写在 `src/kanban/` 内。Cubism SDK 不是开源软件，使用需遵守其专有许可（个人 / 小规模事业者有免费额度，发行前请确认最新条款）。
- `data/` 下的模型、图片、视频均来自第三方，**版权归各自作者，本项目未逐一取得授权**，随仓库提供只是为了克隆下来就能跑。对外分发或商用前请逐个确认（很多 Live2D 模型明确禁止商用），或把相应子目录删掉再发布 —— 素材缺失只会降级，程序不会崩。

---

<p align="center">
  <sub>用 C++17 与 Qt 6 写成 · 仅面向 Windows 10 / 11</sub>
</p>

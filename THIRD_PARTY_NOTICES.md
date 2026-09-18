# 第三方组件与许可声明

本项目的**自有代码**（`src/`、`cmake/`、`tests/`、`docs/`）以 [MIT](LICENSE) 许可发布。
但它集成了若干第三方组件，这些组件**不适用 MIT**，各自受其原始许可约束。再分发（包括
打包发布便携版）时必须同时遵守下列条款。

---

## 1. 随仓库分发的二进制

| 文件 | 上游项目 | 许可 | 用途 |
| --- | --- | --- | --- |
| `resources/dlls/ExplorerBgTool.dll` | [Maplespe/explorerTool](https://github.com/Maplespe/explorerTool) · [SuryaMajumdar/ExplorerBgTool](https://github.com/SuryaMajumdar/ExplorerBgTool) | MIT | 资源管理器窗口背景注入 |
| `resources/dlls/ExplorerBlurMica.dll` | [Maplespe/ExplorerBlurMica](https://github.com/Maplespe/ExplorerBlurMica)（2.0.1） | **LGPL-3.0** | Blur / Acrylic / Mica 系统级窗口效果 |

这两个 DLL 随仓库提供，构建时由 CMake 打进 Qt 资源、运行时释放到 `<程序目录>/dll`。
本项目**不修改**它们的源码，仅通过官方配置接口调用。

### LGPL-3.0 的额外义务

`ExplorerBlurMica.dll` 是 LGPL-3.0 组件。以动态库形式（DLL）分发满足「允许用户替换该库」
的要求，但再分发时你还需要：

1. 随附 LGPL-3.0 全文（<https://www.gnu.org/licenses/lgpl-3.0.html>）；
2. 保留上游的版权与许可声明（本项目不修改其文件，故其内嵌声明保持原样）；
3. 明确告知使用者该 DLL 的源码获取地址（见上表上游仓库链接）。

---

## 2. 构建期依赖（不随仓库提供）

| 组件 | 上游 | 许可 | 说明 |
| --- | --- | --- | --- |
| Live2D Cubism Native SDK 5 R.5（Core 6.0.1） | [live2d.com](https://www.live2d.com/en/sdk/download/native/) | [Live2D Proprietary Software License](https://www.live2d.com/en/sdk/license/) | 看板娘渲染。**专有许可，不是开源软件**，需自行下载并接受其条款；仓库不提交 |
| GLEW 2.3.1 | [nigels-com/glew](https://github.com/nigels-com/glew) | MIT / BSD-3-Clause | OpenGL 扩展加载，随 SDK 路径一起被 `third_party/glew` 引用 |

### Live2D 许可须知

Live2D Cubism SDK 的使用受其**专有许可**约束，与开源许可完全不同：

- 有面向个人 / 小规模事业者的免费额度，但有年营收门槛与用途限制；
- 需要确认发行时点的**最新条款**（Live2D 会更新其许可协议）；
- 使用第三方 Live2D 模型（`data/models/`）还需额外遵守**各模型作者**的使用条款，
  很多模型明确禁止商用。仓库不包含任何模型素材。

本项目对 SDK 的集成方式是：**源码原样放在 `third_party/` 且不做任何修改**，
适配代码全部隔离在 `src/kanban/Live2DRendererCubism.cpp` 等自有文件内，
因此第三方代码始终可以用上游版本整体替换。

---

## 3. 运行时框架

| 组件 | 许可 |
| --- | --- |
| [Qt 6](https://www.qt.io/)（Widgets / Multimedia / MultimediaWidgets / OpenGL / OpenGLWidgets） | LGPL-3.0 / 商业双许可。以动态链接方式使用，满足 LGPL 要求；发布时请随附 LGPL-3.0 全文 |

---

## 4. 与上游无关的素材

`data/` 目录（Live2D 模型、图片图库、视频）**不在本仓库内**，由使用者自备，
其版权与许可与 Yumeiren 项目无关。

# 虞美人 (Yumeiren) 项目重构规范

## 1. 目录结构规范

```
Yumeiren/
├── cmake/                    # CMake 模块文件
│   └── Live2DCubism.cmake   # Live2D Cubism SDK 构建配置
├── docs/                     # 文档
│   └── dev/                  # 开发文档(入库)
├── resources/                # 资源文件
│   ├── app.rc               # 版本资源
│   ├── app.manifest          # UAC 清单
│   ├── style.qss             # 暗色主题
│   ├── light.qss             # 亮色主题
│   └── dlls/                 # Hook DLL
├── src/                      # 源代码
│   ├── main.cpp              # 程序入口
│   ├── app/                  # 应用生命周期
│   ├── config/               # 配置管理
│   ├── core/                 # 核心工具
│   ├── ui/                   # UI 组件
│   ├── folderbg/             # 文件夹背景美化
│   ├── wallpaper/            # 视频动态壁纸
│   ├── kanban/               # 看板娘(Live2D)
│   ├── tray/                 # 系统托盘
│   └── platform/windows/     # Windows 平台特定
├── third_party/              # 第三方库(不入库)
│   ├── cubism/              # Cubism SDK
│   ├── glew/                # GLEW
│   └── glfw/                # GLFW
└── tools/                    # 工具脚本
```

## 2. 命名规范

### 2.1 文件命名
- **头文件**: PascalCase.h (如 `AppConfig.h`)
- **实现文件**: PascalCase.cpp (如 `AppConfig.cpp`)
- **CMake 文件**: PascalCase.cmake (如 `Live2DCubism.cmake`)
- **资源文件**: 小写或 PascalCase (如 `style.qss`, `app.rc`)

### 2.2 目录命名
- **模块目录**: 全小写 (如 `app/`, `config/`, `ui/`)
- **平台目录**: 小写 (如 `platform/windows/`)

### 2.3 类命名
- **类名**: PascalCase (如 `AppConfig`, `VideoWallpaper`)
- **命名空间**: 全小写 (如 `kanban`, `videodiag`)

### 2.4 函数命名
- **公有函数**: camelCase (如 `startPlaying()`)
- **私有函数**: camelCase (如 `ensureOutputs()`)
- **静态函数**: camelCase (如 `instance()`)

### 2.5 成员变量
- **成员变量**: m_ 前缀 + camelCase (如 `m_playlist`, `m_started`)
- **静态变量**: g_ 前缀 (如 `g_wallpaper`)
- **常量**: k 前缀 + PascalCase (如 `kLongSuspendReleaseMs`)

## 3. 文件大小规范

### 3.1 行数限制
- **头文件**: ≤ 300 行
- **实现文件**: ≤ 600 行
- **CMake 文件**: ≤ 200 行

### 3.2 超大文件拆分策略
当实现文件超过 600 行时，按功能模块拆分为多个文件：
- 保持类声明在单个头文件中
- 按功能将方法定义分散到多个 .cpp 文件
- 每个拆分文件必须包含完整的 #include 列表
- 使用相同的头文件保护宏

### 3.3 拆分示例
```cpp
// MainWindow.h - 保持完整类声明
class MainWindow : public QMainWindow {
    // 所有成员声明
};

// MainWindow.cpp - 核心逻辑(构造/析构/事件)
#include "MainWindow.h"
// ...

// MainWindowImagePage.cpp - 图片页面相关方法
#include "MainWindow.h"
// ...

// MainWindowVideoPage.cpp - 视频页面相关方法
#include "MainWindow.h"
// ...
```

## 4. 模块划分规范

### 4.1 模块职责
| 模块 | 职责 | 关键类 |
|------|------|--------|
| app/ | 应用启动、退出、单实例 | AppInfo, ApplicationRuntimeState, ApplicationShutdown |
| config/ | 配置读写、键名定义 | AppConfig, ConfigKeys |
| core/ | 缓存路径、诊断日志 | CachePaths, Diagnostics |
| ui/ | 主窗口、工具提示 | MainWindow, TooltipStyle |
| folderbg/ | 文件夹背景美化 | FolderBgEngine, ImageProcess |
| wallpaper/ | 视频动态壁纸 | VideoWallpaper |
| kanban/ | 看板娘(Live2D) | KanbanController, Live2DRenderer |
| tray/ | 系统托盘 | SystemTrayController |
| platform/windows/ | Windows 平台 API | DesktopMount |

### 4.2 模块间依赖
- 依赖方向: ui/ → 其他所有模块
- 禁止循环依赖
- 第三方库只在 third_party/ 中引用

## 5. Include 规范

### 5.1 项目内 Include
- 使用相对路径从 src/ 根目录开始
- 示例: `#include "config/AppConfig.h"`
- 示例: `#include "kanban/KanbanController.h"`

### 5.2 第三方 Include
- 使用 SYSTEM 关键字避免警告
- 示例: `#include <QApplication>` (Qt)
- 示例: `#include <windows.h>` (Win32)

### 5.3 Include 顺序
1. 对应头文件 (如 MainWindow.cpp 先 include MainWindow.h)
2. 项目内头文件
3. Qt 头文件
4. 标准库头文件
5. 平台特定头文件

## 6. CMake 规范

### 6.1 源文件列表
- 按模块分组列出
- 使用完整的相对路径
- 示例:
```cmake
set(YUMEIREN_SOURCES
    # 入口
    src/main.cpp
    # 应用生命周期
    src/app/AppInfo.cpp
    src/app/ApplicationRuntimeState.cpp
    # ... 其他模块
)
```

### 6.2 第三方库
- 使用 CACHE PATH 变量指向本地路径
- 默认路径指向 third_party/ 目录
- 允许通过 CMake 命令行覆盖

## 7. 重构流程

### 7.1 准备阶段
1. 备份当前代码
2. 确保现有构建通过
3. 创建重构分支

### 7.2 执行阶段
1. 创建目标目录结构
2. 移动文件到新位置
3. 更新所有 #include 路径
4. 拆分超大文件
5. 更新 CMakeLists.txt
6. 更新文档引用

### 7.3 验证阶段
1. 编译验证(普通配置)
2. 编译验证(Live2D 配置)
3. 冒烟验证：启动构建产物走一遍主流程（**仓库里没有测试目标** —— 测试代码用完即删，
   说明见 `CMakeLists.txt` 里 `cmake/*Tests.cmake` 那一段；临时写测试可以，验证完要删干净）
4. 检查文件大小是否符合规范

### 7.4 清理阶段
1. 删除旧文件
2. 更新 .gitignore
3. 提交重构结果

## 8. 新模块开发规范

### 8.1 创建新模块
1. 在 src/ 下创建模块目录
2. 遵循命名规范创建文件
3. 在 CMakeLists.txt 中添加源文件
4. 编写模块文档

### 8.2 代码审查清单
- [ ] 文件大小符合规范
- [ ] 命名符合规范
- [ ] Include 顺序正确
- [ ] 无循环依赖
- [ ] 编译无警告
- [ ] 文档已更新

## 9. 第三方库管理

### 9.1 库位置
所有第三方库放在 third_party/ 目录下:
- `third_party/cubism/` - Cubism SDK
- `third_party/glew/` - GLEW
- `third_party/glfw/` - GLFW

### 9.2 版本控制
- third_party/ 目录不入库(.gitignore)
- 文档记录使用的版本号
- 升级时更新版本号和文档

### 9.3 构建集成
- 使用 CMake 的 CACHE PATH 变量
- 默认路径指向 third_party/
- 允许通过命令行覆盖

### 9.4 禁止修改第三方源码（硬约束）

**`third_party/` 下的代码一律只读，不做任何修改。** 需要适配时，改动落在
`src/` 里，通过包装、适配器或编译期开关完成，而不是去改别人仓库里的文件。

理由有三条，都不是洁癖：

1. **升级会全丢**。第三方库按整包替换来升级，改过的文件会被覆盖，而且不会有人
   记得改过什么。
2. **责任边界会糊掉**。一旦第三方代码里有自己的改动，之后出现任何异常都要先
   自证清白，排查成本远高于在 src/ 里多写一层适配。
3. **构建产物不算源码**。部署到 `build/` 下的东西(着色器、DLL、配置)是产物，
   可以随时删掉重建；`third_party/` 里的原始文件才是基线。

排查「我是不是动过第三方代码」的实用办法，是按修改时间找，而不是靠记忆：

```bash
find third_party -type f -newermt "2026-09-01"     # 返回 0 个文件即从未改动
```

### 9.5 CI 里怎么拿到 third_party（2026-09-24 定案）

`third_party/` 不入库，所以 CI 的干净检出里没有 SDK。**做法是让工作流自己从官方源现取，
而不是把 SDK 提交进仓库。**

为什么不提交：`.gitignore` 排除 `third_party/` 这条规则当初**就是为 Cubism 的专有许可设的**
（`Core/` 走 Live2D Proprietary Software License）。仓库是 PUBLIC，把 SDK 推上去等于替
Live2D 做再分发；CI 从官方源现取则属于「使用者自行取得 SDK 后在 CI 里用」，是另一回事。
顺带也避开了体积 —— 整包 46MB，其中 `Samples/` 就占 29MB 且构建完全用不到。

落点与本地完全一致（`third_party/cubism`、`third_party/glew`），所以**不需要给 CMake 传任何
开关**：探测式默认值看到文件就自动 `YUMEIREN_WITH_LIVE2D=ON`。

关键判据（都写进了 `.github/workflows/release.yml`）：

| 位置 | 判据 | 挡住什么 |
| --- | --- | --- |
| 准备步骤内 | 点名 7 个「构建真正会读的文件」是否存在 | 空目录、解压到一半、缓存被写坏 |
| 准备步骤内 | GLEW 包 sha256 对齐官方公布值 | 下到半截的包 |
| 准备步骤内 | 解压出的 `cubism-info.yml` 里 `version:` 是预期值 | 下到了另一版 SDK、而文件路径恰好相同 |
| configure 之后 | `CMakeCache.txt` 里 `YUMEIREN_WITH_LIVE2D:BOOL=ON` | **最要命的一条**：探测失败会静默退回 `Live2DRendererStub.cpp`，构建全绿但看板娘是空的 |
| build 之后 | `build/Release/` 下有 `Live2DCubismCore.dll` 与 `FrameworkShaders/` | 运行期读盘的东西没被部署过去（缺了不报错，只是白屏） |
| 打包之后 | 解压便携包，顶层唯一是 `Yumeiren/` 且含上面两样 | 中间某次拷贝漏了 |

> 「目录存在」不是判据 —— 空目录、半截解压都满足它。判据要点名到文件。

版本号只有一处来源：工作流 job 级的 `env.CUBISM_SDK_VERSION` / `env.GLEW_VERSION`，
缓存 key、下载 URL、完整性校验全从它取，所以不可能出现「缓存里躺着哪版」和「URL 拉的哪版」
对不上。**升版本时改那两行**，缓存 key 会跟着变，旧缓存自然失效。

## 10. 文档规范

### 10.1 必需文档
- README.md - 项目说明
- docs/dev/REFACTORING_GUIDE.md - 本文件
- 各模块的头文件注释

### 10.2 注释规范
- **源码只留一句结论**，只解释「为什么」：非直观原因、硬约束、反直觉取值、平台坑
- 类声明前: 功能说明（不超过 4 行）
- 超过 3 行的论述移入 `docs/dev/design-notes-*.md`，原处留一句跳转
- 禁止历史叙述（「曾有一个 xxx()」「xx 已删除」）
- 使用 // 而非 /* */
- 完整规则见 `docs/dev/comment-standard.md`

## 附录: 当前项目文件映射

| 原文件 | 新位置 | 说明 |
|--------|--------|------|
| src/appinfo.{h,cpp} | src/app/AppInfo.{h,cpp} | 重命名 |
| src/engine.{h,cpp} | src/folderbg/FolderBgEngine.{h,cpp} | 移动+重命名 |
| src/imageprocess.{h,cpp} | src/folderbg/ImageProcess.{h,cpp} | 移动 |
| src/videodiag.{h,cpp} | src/core/Diagnostics.{h,cpp} | 移动+重命名 |
| src/tooltipstyle.{h,cpp} | src/ui/TooltipStyle.{h,cpp} | 移动+重命名 |
| src/mainwindow.{h,cpp} | src/ui/MainWindow*.{h,cpp} | 拆分 |
| src/videowallpaper.{h,cpp} | src/wallpaper/VideoWallpaper*.{h,cpp} | 拆分 |

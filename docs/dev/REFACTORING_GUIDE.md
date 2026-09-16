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
3. 运行冒烟测试
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

## 10. 文档规范

### 10.1 必需文档
- README.md - 项目说明
- docs/dev/REFACTORING_GUIDE.md - 本文件
- 各模块的头文件注释

### 10.2 注释规范
- 类声明前: 功能说明
- 公有方法: 参数和返回值说明
- 复杂逻辑: 实现说明
- 使用 // 而非 /* */

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

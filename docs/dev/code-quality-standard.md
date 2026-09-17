# 代码质量规范

本规范适用于虞美人（Yumeiren）Qt/C++ 项目的所有新增和修改代码。

---

## 1. 命名规范

### 文件命名

PascalCase：`VideoWallpaper.cpp`、`KanbanController.h`

### 类命名

PascalCase：`VideoWallpaper`、`KanbanController`、`SystemTrayController`

### 函数命名

camelCase：`startPlaying()`、`pauseResume()`、`loadModel()`

### 成员变量

m_ 前缀 + camelCase：`m_currentModelPath`、`m_isUserPaused`

### 常量

k 前缀 + PascalCase：`kAutostartValue`、`kModule`

### 枚举

PascalCase：`State::Idle`、`PlayMode::SingleLoop`

---

## 2. 文件和目录规范

### 目录结构

```
src/
├── app/          应用生命周期
├── config/       配置管理
├── core/         核心工具（缓存、诊断、图片处理）
├── engine/       系统后端（DLL 注册、Explorer）
├── kanban/       看板娘全栈
├── platform/     平台适配（Win32 API）
├── tray/         系统托盘
├── ui/           主窗口各页
└── wallpaper/    动态壁纸
```

### Include 顺序

1. 对应头文件
2. 项目头文件
3. Qt 头文件
4. 标准库头文件
5. 系统/平台头文件

---

## 3. 类职责规范

- 每个类一个主要职责
- 一个清晰的资源所有权边界
- 一个明确的生命周期
- 一个明确的线程归属

### 避免

- God Object（万能类）
- 过大的 Manager 类
- 循环依赖
- UI 直接操作底层资源

---

## 4. 函数规模规范

- 普通函数：≤ 50 行
- 复杂函数：≤ 100 行
- 单个类：≤ 500 行
- 超过限制必须说明原因

---

## 5. Qt 对象生命周期

### QObject 父子关系

优先使用 Qt 父子对象机制管理生命周期。

### deleteLater

跨事件边界的销毁使用 `deleteLater()`。

### QPointer

可能被 Qt 父子机制销毁的窗口使用 `QPointer` 保护。

### 信号槽

重复连接使用 `Qt::UniqueConnection`。

---

## 6. 线程规范

- QWidget、QWindow、QOpenGLWidget 只能在 GUI 线程操作
- QMediaPlayer、QVideoWidget 只能在 GUI 线程操作
- 后台线程不得直接修改 UI
- 异步回调必须检查对象生命周期

---

## 7. 错误处理

- 关键错误必须有明确处理策略
- 不得静默吞掉关键错误
- 底层模块返回错误或发出错误信号
- UI 层负责展示用户可理解的信息

---

## 8. 日志规范

- 使用 `videodiag::log()` 统一入口
- 分级：Error、Warning、Info、Debug
- 格式：`[HH:mm:ss.zzz][LEVEL][tid=NNNN][Module] message`
- 不得每帧输出日志
- 日志文件 1MB 滚动

---

## 9. 注释规范

详见 `docs/development/comment-standard.md`。

核心原则：
- 注释解释"为什么"，不重复"做什么"
- 命名清晰的代码不需要注释
- 不为显而易见的代码加注释
- 注释必须描述真实行为

---

## 10. 新模块开发规范

1. 先检查是否有可复用的现有代码
2. 文件级注释说明模块职责
3. 类级注释说明生命周期和线程要求
4. 公共接口完整文档化
5. 资源所有权明确
6. 退出时正确清理
7. 构建验证

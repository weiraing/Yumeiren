# Cubism 后端维护说明

## 模块边界

| 文件 | 职责 |
|------|------|
| `Live2DRendererCubism.cpp` | 宿主状态、模型所有权、光标坐标转换 |
| `CubismRuntime.cpp` | 框架初始化、日志、文件回调、上下文借用、着色器换代 |
| `CubismModel.h` | 不包含 SDK 类型的后端内部接口 |
| `CubismModel_p.h` | 后端私有模型声明，仅供模型实现文件包含 |
| `CubismModel.cpp` | CPU 资源装载、更新器登记、资源释放 |
| `CubismModelGl.cpp` | 纹理上传、投影和绘制 |
| `CubismModelInteraction.cpp` | 动画更新、动作、表情和命中反馈 |

控制器继续只依赖 `Live2DRenderer`，不直接访问模型内部接口。
SDK 成员和重写接口沿用 SDK 命名；自有模型方法使用 camelCase，成员使用 `m_` 前缀。
`_p.h` 表示私有实现头，不得被界面或控制器引用。

## 生命周期约束

- 框架先 `StartUp` 再 `Initialize`；分配器和 Option 与进程同寿。
- 关闭模型不调用框架 `Dispose`，允许用户反复启用看板娘。
- GLEW 早于其他 GL 头文件，首次纹理上传前必须完成初始化。
- GL 创建和释放必须取得宿主上下文；模型析构必须早于宿主销毁。
- `GlScope` 只归还自己借用的上下文，不打断宿主已有的绘制状态。
- 新上下文创建渲染器前，先按世代号清理旧着色器缓存。
- CPU 纹理上传后释放，重建 GL 资源时重新解码；渲染热路径不增加图像回读。
- 预载动作由模型拥有；先停止队列，再释放动作，避免残留引用。

## 回归测试

需要本地 Cubism SDK、GLEW、Qt OpenGL 和可用的模型。测试默认关闭，不影响产品构建：

```powershell
cmake -S . -B build -DYUMEIREN_WITH_LIVE2D=ON -DYUMEIREN_BUILD_RENDER_TESTS=ON
cmake --build build --target CubismLifecycleTest -j 1
ctest --test-dir build -R CubismLifecycle --output-on-failure
```

默认使用 `data/models/Haru/Haru.model3.json`，可通过 `YUMEIREN_TEST_MODEL` 指定其他模型。
测试模型和第三方 SDK 不入库。测试链接实际后端，不启动主窗口，不初始化用户配置。

覆盖内容：
- 3 个独立上下文，每个上下文 10 次装载、绘制、卸载。
- 首帧延迟上传、重复交付同一宿主、缺失模型返回错误。
- 画面具有非透明、非黑且不均匀的可见像素，背景不全覆盖。
- 可用动作和表情可以播放，卸载后无 GL 错误。
- 光标在窗口左侧时不移动光标直接切档，立即重算视线；关闭后目标归零。
- 卸载归还借用的上下文，重复卸载和关闭可执行。

## 验证边界

2026-09-18：Live2D Release 的产品、测试版和本地探针均构建成功；
生命周期测试 30 轮通过，本地 Haru 探针的表情、视线和像素检查通过。

这些检查不证明无内存泄漏或性能提升，也不覆盖上下文异常丢失、全部第三方模型、
窗口合成、删除缩略图缓存后的完整界面流程。
完整界面的首开与 Haru 启动已另行手动验证，范围见 `KANBAN_CONTROLLER.md`。
本轮没有修改第三方 SDK；未把局部重构视为全项目重构完成。

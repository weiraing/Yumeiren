// Live2D Cubism Native SDK 渲染后端(任务书 §4.1 / §5 / §9)。
//
// 本类是 Cubism 与本项目之间唯一的边界。两条铁律：
//   1. 头文件绝不 include 任何 Cubism 头 —— 全部 Cubism 对象藏在 Private 里，
//      否则 SDK 头会传染到整个工程(任务书 §17 第 2 条)；
//   2. GL 资源只在 QOpenGLWidget 的上下文里创建/释放(§4.3)：
//      initialize() 与 shutdown() 必须发生在 initializeGL / shutdownGL 时刻，
//      由 KanbanOpenGLView 的 contextReady 信号驱动，控制器不在别的线程碰它。
//
// 实现体分两个文件，构建时二选一(见 CMakeLists.txt)：
//   Live2DRendererStub.cpp    —— SDK 未接入：诚实返回 false，日志写明原因；
//   Live2DRendererCubism.cpp  —— SDK 接入：真正的 Cubism 装载与绘制。
//
// ── Cubism 5 R.5 接入步骤(已核对本地 SDK 头/工程) ─────────────────────────
// 1. 构建配置：SDK 的 Samples/OpenGL/Demo/proj.win.cmake 用
//      CSM_TARGET_WIN_GL + Framework 静态库 + Live2DCubismCore + glew_s；
//      Framework/src/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp 在
//      CSM_TARGET_WIN_GL 下 #include <GL/glew.h>，所以 GLEW 是硬依赖
//      (本地已提供 glew-2.3.1 源码，直接编 source/glew.c，不需要 GLFW：
//      GL 上下文由 Qt 提供，不用 glfwCreateWindow)。
// 2. 框架启动：CubismFramework::Initialize(config) → StartUp()，config 里的
//    Option 内存池与日志回调由 Cubism 自带 LAppAllocator 风格实现提供。
// 3. 模型装载：QFile 读 .model3.json → CubismJson → CubismModelSettingJson →
//    Core::Model::CreateModelAndInitialize(moc3 字节) → Framework Model::Initialize →
//    CubismTextureManager::LoadTexture(逐张 PNG，走 PremultiplyAlpha) →
//    Physics / Pose / Expressions / MotionGroups 分别建管理器并挂到 model 上。
// 4. 每帧：update(dt) 推进 CubismMotionManager + CubismPhysics + Pose + Expression
//    → Model::Update()；render() 里 CubismRenderer_OpenGLES2::DrawModel()，
//    背景只清 alpha=0(透明窗口必须 SRC_ALPHA/ONE 混合，否则边缘发白)。
// 5. 释放：unloadModel() 逐个 Delete 回来；shutdown() = unloadModel + Dispose。
//
// 内存口径(任务书 §13.2)：模型装载/卸载必须成对，重复 start/stop 10 次不得
// 出现句柄、纹理或 VBO 累积。
#ifndef LIVE2DRENDERER_H
#define LIVE2DRENDERER_H

#include "kanban/KanbanRenderer.h"

#include <memory>

namespace kanban {

class Live2DRenderer : public KanbanRenderer
{
public:
    Live2DRenderer();
    ~Live2DRenderer() override;

    QString backendName() const override { return QStringLiteral("Live2D"); }

    // 编译期事实：本 target 是否真的把 Cubism Framework/Core 链进来了。
    // 控制器据此决定是否尝试 Live2D，而不是每次都靠 initialize() 失败去发现。
    static bool sdkCompiledIn();
    // 供界面与日志回答「为什么看不到 Live2D 效果」。
    static QString unavailableReason();

    // 未接入时返回 false，outError 说明「未接入」而不是「模型坏了」。
    bool initialize(QString *outError) override;
    bool loadModel(const QString &modelJsonPath, QString *outError) override;
    void unloadModel() override;

    // 恒为 true：Live2D 一定要 GL 宿主，窗口据此选 QOpenGLWidget 承载。
    bool usesOpenGL() const override;

    void resize(int width, int height, float devicePixelRatio) override;
    void update(float deltaSeconds) override;
    void render() override;
    void paint(QPainter *painter, const QSize &logicalSize) override;

    void pointerMove(const QPointF &pos) override;
    void pointerClick(const QPointF &pos) override;
    bool playNextMotion() override;

    void pause() override;
    void resume() override;
    void shutdown() override;

private:
    struct Private; // Cubism 对象的唯一藏身处

    bool m_ready = false;
    bool m_modelLoaded = false;
    bool m_paused = false;
    int m_width = 320;
    int m_height = 480;
    float m_dpr = 1.0f;
    QString m_modelPath;
    std::unique_ptr<Private> m_d;
};

} // namespace kanban

#endif // LIVE2DRENDERER_H

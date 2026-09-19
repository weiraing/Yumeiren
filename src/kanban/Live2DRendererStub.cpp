// Live2D 后端的「未接入」实现：默认构建编译的那一份(YUMEIREN_WITH_LIVE2D=OFF)。
// 让「SDK 没接」在编译期就成立 —— 不链接 Cubism、不引入 GL 依赖，运行时每个入口
// 老实返回 false，由 KanbanController 切到内置占位动画。
// 接入 SDK 后编译的是 Live2DRendererCubism.cpp，本文件整体退出构建，上层零改动 ——
// 这正是 KanbanRenderer 这层抽象唯一的存在理由。
#include "kanban/Live2DRenderer.h"

#include "core/Diagnostics.h"

namespace kanban {

struct Live2DRenderer::Private {};

bool Live2DRenderer::sdkCompiledIn()
{
    return false;
}

QString Live2DRenderer::unavailableReason()
{
    return QStringLiteral("本构建未编译 Live2D Cubism SDK"
                          "(配置时打开 YUMEIREN_WITH_LIVE2D 并提供 SDK 路径)");
}

Live2DRenderer::Live2DRenderer() : m_d(new Private) {}

Live2DRenderer::~Live2DRenderer() = default;

bool Live2DRenderer::initialize(QString *outError)
{
    if (outError) {
        *outError = unavailableReason();
    }
    return false;
}

bool Live2DRenderer::loadModel(const QString &, QString *outError)
{
    if (outError) {
        *outError = QStringLiteral("Live2D 未接入，无法装载模型");
    }
    return false;
}

void Live2DRenderer::unloadModel() {}

bool Live2DRenderer::usesOpenGL() const
{
    // 本构建连 GL 宿主视图都没编译，答 false 否则窗口会去找不存在的视图类。
    return false;
}

void Live2DRenderer::resize(int width, int height, float devicePixelRatio)
{
    m_width = width;
    m_height = height;
    m_dpr = devicePixelRatio;
}

bool Live2DRenderer::rebuildTexturesIfNeeded()
{
    // 本构建没有 SDK、没有纹理。声明成了 override，必须给这一份定义。
    return false;
}

void Live2DRenderer::update(float) {}
void Live2DRenderer::render() {}
void Live2DRenderer::paint(QPainter *, const QSize &) {}

void Live2DRenderer::setGlHost(KanbanGlHost *)
{
    // 本构建连 GL 宿主视图都不存在，收到宿主指针也无事可做。
}
QString Live2DRenderer::gazeDebugText() const
{
    return QStringLiteral("未接入 SDK");
}
void Live2DRenderer::pointerMove(const QPointF &) {}
// 必须给这个定义：头文件里声明成了 override，虚表会指向这个符号而非基类的内联实现，
// 不定义就是链接错误(ld 报 undefined reference)。函数体转发给基类，保持
// m_gazeStrength 这个唯一状态写入点 —— 界面靠回读它回填档位。
void Live2DRenderer::setGazeStrength(int strength)
{
    KanbanRenderer::setGazeStrength(strength);
}
void Live2DRenderer::pointerClick(const QPointF &) {}

bool Live2DRenderer::playNextMotion()
{
    return false;
}

int Live2DRenderer::playableMotionCount() const
{
    // 0 = 本构建没有 SDK，一段动作都播不了，入口据此置灰。
    return 0;
}

int Live2DRenderer::expressionCount() const
{
    // 0 = 本后端没有表情，控制器据此切到占位渲染器。
    return 0;
}

bool Live2DRenderer::playNextExpression()
{
    return false;
}

void Live2DRenderer::pause() { m_paused = true; }
void Live2DRenderer::resume() { m_paused = false; }

void Live2DRenderer::shutdown()
{
    m_ready = false;
    m_modelLoaded = false;
    m_modelPath.clear();
    videodiag::log(videodiag::Level::Debug,
                   QStringLiteral("[Live2D] 未接入：shutdown 为空操作"),
                   QStringLiteral("Live2D"));
}

} // namespace kanban

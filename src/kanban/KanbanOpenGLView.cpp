// OpenGL 宿主视图实现。仅 YUMEIREN_WITH_LIVE2D=ON 时参与编译。
#include "kanban/KanbanOpenGLView.h"

#include <QResizeEvent>
#include <QOpenGLContext>

#include "kanban/KanbanRenderer.h"
#include "core/Diagnostics.h"

namespace kanban {

KanbanOpenGLView::KanbanOpenGLView(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setObjectName(QStringLiteral("KanbanOpenGLView"));
    // 逐像素透明：看板娘边缘必须是真 alpha，而不是黑底/白底。
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setAlphaBufferSize(8);
    format.setSamples(0); // 多重采样与透明 FBO 在部分驱动上会退回黑边，关掉
    // 必须点名 OpenGL 3.3(兼容剖面)：Cubism 的 Windows GL 渲染器在首次绘制前
    // 用 wglGetProcAddress 抓 glGenFramebuffers / glBlitFramebuffer 等 3.x 入口，
    // 抓不到就静默不画(不报错、不崩溃，只有一片空)。兼容剖面是因为它同时
    // 用到 glUseProgram(0) 这类固定管线时代的写法；Qt 默认只请求 2.0。
    format.setMajorVersion(3);
    format.setMinorVersion(3);
    format.setProfile(QSurfaceFormat::NoProfile);
    setFormat(format);
    // GL 后端自己管 swapBuffers 上屏，不需要 Qt 再画一遍背景。
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

KanbanOpenGLView::~KanbanOpenGLView()
{
    // 先摘掉宿主指针再让 QOpenGLWidget 拆上下文：渲染器可能比本视图活得久
    // (控制器降级到占位后端时不销毁渲染器)，留着就是悬空指针。
    if (m_renderer) {
        m_renderer->setGlHost(nullptr);
    }
    // 本析构函数体跑在 QOpenGLWidget 拆上下文之前，所以此刻 makeCurrent 仍可用，
    // 但 GL 资源的正常释放走 KanbanController::stop()：那里先 shutdown 再删窗口。
}

void KanbanOpenGLView::setRenderer(KanbanRenderer *renderer)
{
    m_renderer = renderer;
}

void KanbanOpenGLView::initializeGL()
{
    // 上下文已当前化：此刻才允许创建 Cubism/纹理资源。
    makeCurrent();
    m_contextReadySent = false;
    emit contextReady();
    doneCurrent();
}

void KanbanOpenGLView::resizeGL(int w, int h)
{
    if (m_renderer) {
        m_renderer->resize(w, h, float(devicePixelRatioF()));
    }
}

// —— KanbanGlHost ——

bool KanbanOpenGLView::glHostReady() const
{
    // 上下文要到首次绘制前才创建；没创建时渲染器必须放弃 GL 操作而不是崩。
    return context() != nullptr;
}

bool KanbanOpenGLView::glIsCurrent() const
{
    return context() && context() == QOpenGLContext::currentContext();
}

bool KanbanOpenGLView::glMakeCurrent()
{
    if (!context()) {
        return false;
    }
    QOpenGLWidget::makeCurrent();
    return glIsCurrent();
}

void KanbanOpenGLView::glDoneCurrent()
{
    QOpenGLWidget::doneCurrent();
}

QSize KanbanOpenGLView::glPixelSize() const
{
    return QSize(int(qRound64(width() * devicePixelRatioF())),
                 int(qRound64(height() * devicePixelRatioF())));
}

void KanbanOpenGLView::paintGL()
{
    if (m_renderer && m_renderer->usesOpenGL()) {
        m_renderer->render();
    }
}

} // namespace kanban

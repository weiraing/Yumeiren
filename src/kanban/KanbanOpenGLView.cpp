// OpenGL 宿主视图实现。仅 YUMEIREN_WITH_LIVE2D=ON 时参与编译。
#include "kanban/KanbanOpenGLView.h"

#include <QResizeEvent>

#include "kanban/KanbanRenderer.h"
#include "videodiag.h"

namespace kanban {

KanbanOpenGLView::KanbanOpenGLView(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setObjectName(QStringLiteral("KanbanOpenGLView"));
    // 逐像素透明：看板娘边缘必须是真 alpha，而不是黑底/白底。
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setAlphaBufferSize(8);
    format.setSamples(0); // 多重采样与透明 FBO 在部分驱动上会退回黑边，关掉
    setFormat(format);
    // GL 后端自己管 swapBuffers 上屏，不需要 Qt 再画一遍背景。
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

KanbanOpenGLView::~KanbanOpenGLView()
{
    // 上下文由 QOpenGLWidget 在析构后销毁；渲染器必须在那之前 shutdown，
    // 这件事由 KanbanController::stop() 保证(先 renderer->shutdown 再删窗口)。
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

void KanbanOpenGLView::paintGL()
{
    if (m_renderer && m_renderer->usesOpenGL()) {
        m_renderer->render();
    }
}

} // namespace kanban

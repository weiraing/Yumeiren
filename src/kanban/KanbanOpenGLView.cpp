// OpenGL 宿主视图实现。仅 YUMEIREN_WITH_LIVE2D=ON 时参与编译。
#include "kanban/KanbanOpenGLView.h"

#include "core/Diagnostics.h"
#include "kanban/KanbanRenderer.h"

#include <QOpenGLContext>
#include <QResizeEvent>

namespace kanban {

namespace {

// 世代发号器用单调计数而非 context() 指针：指针会随对象释放被复用，一旦复用就等于
// 「换了上下文却当成没换」，表现为偶发的「第二次启动画面空白」，极难定位。
quint64 nextGlContextGeneration()
{
    static quint64 s_next = 1;
    return s_next++;
}

} // namespace

KanbanOpenGLView::KanbanOpenGLView(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setObjectName(QStringLiteral("KanbanOpenGLView"));
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setAlphaBufferSize(8);
    format.setSamples(0); // 多重采样与透明 FBO 在部分驱动上会退回黑边，关掉
    // 必须点名 OpenGL 3.3：Cubism 的 Windows GL 渲染器用 wglGetProcAddress 抓 3.x 入口，
    // 抓不到就静默不画(不报错、只有一片空)；Qt 默认只请求 2.0。
    format.setMajorVersion(3);
    format.setMinorVersion(3);
    format.setProfile(QSurfaceFormat::NoProfile);
    setFormat(format);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

KanbanOpenGLView::~KanbanOpenGLView()
{
    // 先摘宿主指针再让 QOpenGLWidget 拆上下文：渲染器可能比本视图活得久，留着就是
    // 悬空指针。
    if (m_renderer) {
        m_renderer->setGlHost(nullptr);
    }
    // GL 资源的正常释放走 KanbanController::stop()：那里先 shutdown 再删窗口。
}

void KanbanOpenGLView::setRenderer(KanbanRenderer *renderer)
{
    m_renderer = renderer;
}

void KanbanOpenGLView::initializeGL()
{
    makeCurrent();
    // 先领新世代号再发 contextReady：控制器在 contextReady 里装载模型，会碰那份进程级
    // 着色器缓存，它必须已经能看到新世代号，否则会把上个上下文的死 id 当成自己的。
    m_contextGeneration = nextGlContextGeneration();
    applog::log(applog::Level::Info,
                   QStringLiteral("[KanbanGL] initializeGL 上下文就绪：%1 / %2 / 绘制面 %3x%4")
                       .arg(QString::fromLatin1(reinterpret_cast<const char *>(
                                glGetString(GL_VERSION))),
                            QString::fromLatin1(reinterpret_cast<const char *>(
                                glGetString(GL_RENDERER))))
                       .arg(glPixelSize().width())
                       .arg(glPixelSize().height()),
                   QStringLiteral("KanbanGL"));
    m_firstPaintDone = false;
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

quint64 KanbanOpenGLView::glContextGeneration() const
{
    return m_contextGeneration;
}

void KanbanOpenGLView::paintGL()
{
    const bool firstPaint = !m_firstPaintDone;
    if (firstPaint) {
        m_firstPaintDone = true;
        // 「initializeGL 触发了」不等于「画面出来了」，中间还隔着一次 paintGL。
        applog::log(applog::Level::Info,
                       QStringLiteral("[KanbanGL] 首帧 paintGL(绘制面 %1x%2)")
                           .arg(glPixelSize().width())
                           .arg(glPixelSize().height()),
                       QStringLiteral("KanbanGL"));
    }
    if (m_renderer && m_renderer->usesOpenGL()) {
        m_renderer->render();
    }
    if (firstPaint) {
        applog::log(applog::Level::Debug, QStringLiteral("[KanbanGL] 首帧 paintGL 返回"),
                       QStringLiteral("KanbanGL"));
    }
}

} // namespace kanban

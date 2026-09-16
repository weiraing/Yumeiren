// 看板娘窗口实现。
#include "kanban/KanbanWindow.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "kanban/KanbanRenderer.h"
#include "kanban/KanbanSoftwareView.h"
#ifdef YUMEIREN_WITH_LIVE2D
#include "kanban/KanbanOpenGLView.h"
#endif
#include "videodiag.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace kanban {

namespace {
constexpr const char *kModule = "KanbanWindow";
constexpr int kDragThresholdPx = 4;   // 超过这个位移算拖动，不算点击
constexpr int kScreenMarginPx = 24;   // 首次落位离屏幕右/下边的留白
constexpr int kMinWindowEdgePx = 120; // 缩放下限：防止滚轮把窗口搓没
} // namespace

KanbanWindow::KanbanWindow(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("KanbanWindow"));
    // 无边框 + 逐像素透明 + 不进任务栏 + 不抢焦点。
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

#ifdef Q_OS_WIN
    // 资源管理器重启(含本软件 regsvr32 后重启 explorer)会丢样式，
    // 订阅 TaskbarCreated 广播重贴置顶/穿透。
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
#endif

    videodiag::logObjectEvent("create", this, QStringLiteral("kanban window"));
}

KanbanWindow::~KanbanWindow()
{
    videodiag::logObjectEvent("destroy", this, QStringLiteral("kanban window"));
}

// —— 视图选型 ——

void KanbanWindow::attachRenderer(KanbanRenderer *renderer)
{
    m_renderer = renderer;
    const bool wantGl = m_renderer && m_renderer->usesOpenGL();
#ifndef YUMEIREN_WITH_LIVE2D
    // 本构建没有 GL 宿主(未接入 SDK)：GL 渲染器根本不会被控制器选中，
    // 真走到这里说明接线错了，留日志而不是静默画黑屏。
    if (wantGl) {
        videodiag::log(videodiag::Level::Error,
                       QStringLiteral("[KanbanWindow] 渲染器要求 GL 宿主，但本构建未编译 GL 视图"),
                       QLatin1String(kModule));
    }
#endif
    if (wantGl != m_glHost || !m_view) {
        m_glHost = wantGl;
        ensureView();
    } else if (m_renderer) {
        // 同类视图换渲染器：只换指针并同步尺寸。
        if (auto *sw = qobject_cast<KanbanSoftwareView *>(m_view)) {
            sw->setRenderer(m_renderer);
        }
#ifdef YUMEIREN_WITH_LIVE2D
        else if (auto *gl = qobject_cast<KanbanOpenGLView *>(m_view)) {
            gl->setRenderer(m_renderer);
        }
#endif
    }
}

void KanbanWindow::detachRenderer()
{
    m_renderer = nullptr;
}

void KanbanWindow::ensureView()
{
    if (m_view) {
        // 先摘出布局再删：布局持有指针，直接 delete 会让布局短暂指空。
        static_cast<QVBoxLayout *>(layout())->removeWidget(m_view);
        m_view->deleteLater();
        m_view = nullptr;
    }

#ifdef YUMEIREN_WITH_LIVE2D
    if (m_glHost) {
        auto *gl = new KanbanOpenGLView(this);
        gl->setRenderer(m_renderer);
        connect(gl, &KanbanOpenGLView::contextReady, this, &KanbanWindow::glContextReady);
        m_view = gl;
    } else
#endif
    {
        auto *sw = new KanbanSoftwareView(this);
        sw->setRenderer(m_renderer);
        m_view = sw;
    }

    layout()->addWidget(m_view);
    // 交互代码只写一份：视图的事件冒泡到窗口前先用事件过滤器截获。
    m_view->installEventFilter(this);
    m_view->setMouseTracking(true);
    m_view->setAttribute(Qt::WA_Hover, true);
    m_view->show();
}

void KanbanWindow::requestFrame()
{
    if (m_view) {
        m_view->update();
    }
}

// —— 原生样式 ——

void KanbanWindow::setAlwaysOnTop(bool onTop)
{
    if (m_alwaysOnTop == onTop) {
        return;
    }
    m_alwaysOnTop = onTop;
    applyTopmostStyle();
    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("[KanbanWindow] 窗口置顶=%1").arg(onTop),
                   QLatin1String(kModule));
}

void KanbanWindow::applyTopmostStyle()
{
#ifdef Q_OS_WIN
    if (!testAttribute(Qt::WA_WState_Created)) {
        return; // HWND 还不存在，等 show 后再贴
    }
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        return;
    }
    // 用 SetWindowPos 而不是改 Qt 窗口标志：改标志会让 Qt 销毁并重建 HWND，
    // 画面闪一下不说，子视图的 GL 上下文也跟着没了。
    SetWindowPos(hwnd, m_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}

void KanbanWindow::setMouseThrough(bool through)
{
    if (m_mouseThrough == through) {
        return;
    }
    m_mouseThrough = through;
    applyMouseThroughStyle();
    // 穿透后收不到鼠标事件，悬停态必须手工退出，否则状态机会卡在 Hover。
    if (through) {
        emit pointerLeft();
    }
    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("[KanbanWindow] 鼠标穿透=%1").arg(through),
                   QLatin1String(kModule));
}

void KanbanWindow::applyMouseThroughStyle()
{
#ifdef Q_OS_WIN
    if (!testAttribute(Qt::WA_WState_Created)) {
        return;
    }
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        return;
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (m_mouseThrough) {
        ex |= LONG_PTR(WS_EX_TRANSPARENT);
    } else {
        ex &= ~LONG_PTR(WS_EX_TRANSPARENT);
    }
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    // 样式改动需要一次 SetWindowPos 才落地(其余参数全是空操作)。
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
}

void KanbanWindow::setOpacityPercent(int percent)
{
    m_opacityPercent = qBound(20, percent, 100);
    setWindowOpacity(m_opacityPercent / 100.0);
}

void KanbanWindow::setPausedVisual(bool paused)
{
    m_pausedVisual = paused;
    setCursor(paused ? Qt::ArrowCursor : Qt::OpenHandCursor);
    requestFrame();
}

void KanbanWindow::setModelDisplayName(const QString &)
{
    // 名称显示在主界面与托盘提示里，窗口本体不放任何说明性浮层(任务书 §14.3)。
}

void KanbanWindow::setInteractionEnabled(bool enabled)
{
    m_interactionEnabled = enabled;
    if (!enabled) {
        m_pressSeen = false;
        m_dragging = false;
        emit pointerLeft();
    }
}

void KanbanWindow::placeFromConfig(int x, int y, int width, int height)
{
    if (width >= kMinWindowEdgePx) {
        resize(width, qMax(height, kMinWindowEdgePx));
    }
    if (x >= 0 && y >= 0) {
        move(x, y);
        return;
    }
    // 首次启动：贴主屏可用区右下角，避开任务栏。
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) {
        return;
    }
    const QRect avail = screen->availableGeometry();
    // 形参 width/height 遮蔽了同名成员函数，这里要的是「resize 之后的实际尺寸」。
    move(avail.right() - this->width() - kScreenMarginPx,
         avail.bottom() - this->height() - kScreenMarginPx);
}

void KanbanWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 布局会自动把子视图铺满；这里只做一件布局管不了的事：把尺寸同步给窗口本身。
    applyTopmostStyle();
    applyMouseThroughStyle();
}

// —— 交互：全部走事件过滤器，两个视图共用同一套语义 ——

bool KanbanWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_view || !event) {
        return QWidget::eventFilter(watched, event);
    }

    const QPointF viewPos = [](const QEvent *e) {
        if (e->type() == QEvent::Wheel) {
            return static_cast<const QWheelEvent *>(e)->position();
        }
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease
            || e->type() == QEvent::MouseMove) {
            return static_cast<const QMouseEvent *>(e)->position();
        }
        return QPointF();
    }(event);

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && m_interactionEnabled) {
            m_pressSeen = true;
            m_dragging = false;
            m_pressGlobal = me->globalPosition().toPoint();
            m_dragOffset = m_pressGlobal - m_view->mapToGlobal(QPoint(0, 0));
            setCursor(Qt::ClosedHandCursor);
        }
        break;
    }
    case QEvent::MouseMove: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (m_interactionEnabled) {
            emit hoveredAt(viewPos);
        }
        if (!(me->buttons() & Qt::LeftButton) || !m_pressSeen) {
            break;
        }
        const QPoint global = me->globalPosition().toPoint();
        if (!m_dragging) {
            if ((global - m_pressGlobal).manhattanLength() < kDragThresholdPx) {
                break;
            }
            m_dragging = true;
            emit dragStarted();
        }
        move(global - m_dragOffset);
        break;
    }
    case QEvent::MouseButtonRelease: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton || !m_pressSeen) {
            break;
        }
        m_pressSeen = false;
        setCursor(m_pausedVisual ? Qt::ArrowCursor : Qt::OpenHandCursor);
        if (m_dragging) {
            m_dragging = false;
            emit dragFinished();
        } else if (m_interactionEnabled) {
            emit clicked(viewPos);
        }
        break;
    }
    case QEvent::HoverEnter:
        if (m_interactionEnabled) {
            emit pointerEntered();
        }
        break;
    case QEvent::HoverLeave:
        if (m_interactionEnabled) {
            emit pointerLeft();
        }
        break;
    case QEvent::Wheel: {
        auto *we = static_cast<QWheelEvent *>(event);
        const int steps = we->angleDelta().y() / 120;
        if (steps != 0 && m_interactionEnabled) {
            emit scaleStepped(steps);
        }
        we->accept();
        return true; // 已处理，避免继续冒泡到窗口触发别的默认行为
    }
    case QEvent::ContextMenu: {
        auto *ce = static_cast<QContextMenuEvent *>(event);
        QMenu menu(this);
        QAction *pauseAct = menu.addAction(QStringLiteral("暂停 / 恢复"));
        connect(pauseAct, &QAction::triggered, this, &KanbanWindow::pauseResumeRequested);
        QAction *nextAct = menu.addAction(QStringLiteral("播放下一个动作"));
        connect(nextAct, &QAction::triggered, this, &KanbanWindow::playNextRequested);
        QAction *modelAct = menu.addAction(QStringLiteral("切换模型"));
        connect(modelAct, &QAction::triggered, this, &KanbanWindow::nextModelRequested);
        menu.addSeparator();
        QAction *hideAct = menu.addAction(QStringLiteral("暂时隐藏"));
        connect(hideAct, &QAction::triggered, this, &KanbanWindow::hideRequested);
        QAction *throughAct = menu.addAction(QStringLiteral("鼠标穿透"));
        throughAct->setCheckable(true);
        throughAct->setChecked(m_mouseThrough);
        connect(throughAct, &QAction::toggled, this, &KanbanWindow::setMouseThrough);
        QAction *topAct = menu.addAction(QStringLiteral("窗口置顶"));
        topAct->setCheckable(true);
        topAct->setChecked(m_alwaysOnTop);
        connect(topAct, &QAction::toggled, this, &KanbanWindow::setAlwaysOnTop);
        menu.addSeparator();
        QAction *settingAct = menu.addAction(QStringLiteral("打开主界面设置"));
        connect(settingAct, &QAction::triggered, this, &KanbanWindow::settingsRequested);
        QAction *quitAct = menu.addAction(QStringLiteral("取消看板娘"));
        connect(quitAct, &QAction::triggered, this, &KanbanWindow::quitRequested);
        menu.exec(ce->globalPos());
        return true;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void KanbanWindow::closeEvent(QCloseEvent *event)
{
    // 关闭请求一律转成隐藏请求；真停止走控制器的 stop()(托盘/界面的「取消看板娘」)。
    event->ignore();
    emit hideRequested();
}

bool KanbanWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType == QStringLiteral("windows_generic_MSG") && message && m_taskbarCreatedMsg) {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message == UINT(m_taskbarCreatedMsg)) {
            applyTopmostStyle();
            applyMouseThroughStyle();
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("[KanbanWindow] 收到 TaskbarCreated，已重贴窗口样式"),
                           QLatin1String(kModule));
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
#endif
    Q_UNUSED(result);
    return QWidget::nativeEvent(eventType, message, result);
}

} // namespace kanban

// 看板娘窗口实现。
#include "kanban/KanbanWindow.h"

#include "core/Diagnostics.h"
#include "kanban/KanbanRenderer.h"
#include "kanban/KanbanSoftwareView.h"
#ifdef YUMEIREN_WITH_LIVE2D
#include "kanban/KanbanOpenGLView.h"
#endif

#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace kanban {

namespace {
constexpr const char *kModule = "KanbanWindow";
constexpr int kDragThresholdPx = 4;   // 超过这个位移算拖动，不算点击
constexpr int kScreenMarginPx = 24;   // 首次落位离屏幕右/下边的留白
constexpr int kMinWindowEdgePx = 120; // 缩放下限：防止滚轮把窗口搓没

// 矩形是否至少有一部分落在某块屏幕的可用区里：识别「上次存的位置已不在任何屏幕上」。
bool isOnAnyScreen(const QRect &rect)
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (screen && screen->availableGeometry().intersects(rect)) {
            return true;
        }
    }
    return false;
}
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
    // 资源管理器重启会丢样式，订阅 TaskbarCreated 广播重贴置顶/穿透。
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
    // 本构建未接入 SDK 时 GL 渲染器不会被选中；真走到这里说明接线错了，留日志而非静默黑屏。
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
    // 视图可能刚被 ensureView() 重建，宿主指针统一在此交回渲染器。
    m_renderer->setGlHost(dynamic_cast<KanbanGlHost *>(m_view));
}

void KanbanWindow::detachRenderer()
{
    // 视图必须一起松手：控制器随后立即 m_renderer.reset()，而窗口只是排期删除、子视图
    // 还活着 —— 视图再来一次 paintGL 或析构时都会踩到已释放内存(实测表现为点「取消」
    // 后进程 0xC0000005 退出)。
    releaseViewRenderer();
    m_renderer = nullptr;
}

// 视图与窗口对渲染器的引用必须同生共死，漏一处剩下的那份就是野指针。
void KanbanWindow::releaseViewRenderer()
{
    if (!m_view) {
        return;
    }
    if (auto *sw = qobject_cast<KanbanSoftwareView *>(m_view)) {
        sw->setRenderer(nullptr);
    }
#ifdef YUMEIREN_WITH_LIVE2D
    else if (auto *gl = qobject_cast<KanbanOpenGLView *>(m_view)) {
        gl->setRenderer(nullptr);
    }
#endif
}

void KanbanWindow::ensureView()
{
    if (m_view) {
        // 先摘出布局再删：布局持有指针，直接 delete 会让布局短暂指空。
        static_cast<QVBoxLayout *>(layout())->removeWidget(m_view);
        // deleteLater 后旧视图还能活到本轮事件循环结束，期间若来了一次 paintGL，它手里
        // 的渲染器可能已被销毁，所以先松手。
        releaseViewRenderer();
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
    // 用 SetWindowPos 而非改 Qt 窗口标志：改标志会让 Qt 销毁并重建 HWND，画面闪一下且
    // 子视图 GL 上下文也没了。
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
    // 名称显示在主界面与托盘提示里，窗口本体不放任何说明性浮层。
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
    // 坐标未必还落在屏幕上(换分辨率、拔副屏、远程桌面重连)。判据取「与任一屏幕可用区
    // 有交集」而非「完全在屏幕内」—— 窗口露一半在边缘是合法用法。
    if (x >= 0 && y >= 0 && isOnAnyScreen(QRect(QPoint(x, y), size()))) {
        move(x, y);
        return;
    }
    // 首次启动(或位置已失效)：贴主屏可用区右下角，避开任务栏。
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) {
        return;
    }
    const QRect avail = screen->availableGeometry();
    // 形参 width/height 遮蔽了同名成员函数，这里要的是 resize 之后的实际尺寸。
    move(avail.right() - this->width() - kScreenMarginPx,
         avail.bottom() - this->height() - kScreenMarginPx);
}

void KanbanWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 布局会自动把子视图铺满。
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
            || e->type() == QEvent::MouseButtonDblClick || e->type() == QEvent::MouseMove) {
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
    case QEvent::MouseButtonDblClick: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && m_interactionEnabled) {
            emit doubleClicked(viewPos);
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
        // 刻意不给父对象：窗口若在 menu.exec() 期间被销毁，Qt 会顺着父子关系 delete 掉栈
        // 上的 menu，对栈地址 free 属于必崩写法。
        QMenu menu;
        QAction *pauseAct = menu.addAction(QStringLiteral("暂停 / 恢复"));
        connect(pauseAct, &QAction::triggered, this, &KanbanWindow::pauseResumeRequested);
        QAction *nextAct = menu.addAction(QStringLiteral("切换动作"));
        // 可播动作不足两个就置灰：只有一个时「切换」等于原地重播。判据收在渲染器基类。
        nextAct->setEnabled(m_renderer && m_renderer->canPlayNextMotion());
        connect(nextAct, &QAction::triggered, this, &KanbanWindow::playNextRequested);
        QAction *exprAct = menu.addAction(QStringLiteral("切换表情"));
        // 没有表情的模型就把入口置灰(点了没反应比灰掉更让人怀疑坏了)，数量问当前渲染器。
        exprAct->setEnabled(m_renderer && m_renderer->expressionCount() > 0);
        connect(exprAct, &QAction::triggered, this, &KanbanWindow::nextExpressionRequested);
        QAction *modelAct = menu.addAction(QStringLiteral("切换模型"));
        connect(modelAct, &QAction::triggered, this, &KanbanWindow::nextModelRequested);
        menu.addSeparator();
        QAction *throughAct = menu.addAction(QStringLiteral("鼠标穿透"));
        throughAct->setCheckable(true);
        throughAct->setChecked(m_mouseThrough);
        // 发「请求」而非直接调 setMouseThrough：只有控制器那条路会同步成员、落盘配置并
        // 回写设置页复选框。
        connect(throughAct, &QAction::toggled, this, &KanbanWindow::mouseThroughRequested);
        QAction *topAct = menu.addAction(QStringLiteral("窗口置顶"));
        topAct->setCheckable(true);
        topAct->setChecked(m_alwaysOnTop);
        // 同上：置顶也是配置键 + 设置页复选框两处要同步。
        connect(topAct, &QAction::toggled, this, &KanbanWindow::alwaysOnTopRequested);
        menu.addSeparator();

        // 视线追踪刻意不放进这个菜单：档位是「一次定好、长期不动」的偏好，已在设置页与
        // 托盘两处，再来一份只会让菜单变长还容易误点。
        QAction *settingAct = menu.addAction(QStringLiteral("打开设置"));
        connect(settingAct, &QAction::triggered, this, &KanbanWindow::settingsRequested);

        // 「取消看板娘」不能直接在 exec() 里发出去：接收方会销毁本窗口，而此刻我们还在
        // menu.exec() 的嵌套事件循环里；先记下意图，等 exec 返回后再发。
        bool quitAsked = false;
        QAction *quitAct = menu.addAction(QStringLiteral("取消看板娘"));
        connect(quitAct, &QAction::triggered, this, [&quitAsked] { quitAsked = true; });

        menu.exec(ce->globalPos());
        if (quitAsked) {
            emit quitRequested();
        }
        return true;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void KanbanWindow::closeEvent(QCloseEvent *event)
{
    // 窗口是无边框 Qt::Tool，能走到这里的只有系统关机、任务栏「关闭窗口」这类外部请求，
    // 一律吞掉：放行会让窗口消失而控制器仍以为它在跑，且无恢复入口。
    event->ignore();
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

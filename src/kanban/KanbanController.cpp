// 看板娘控制器实现。
#include "kanban/KanbanController.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

#include "app/ApplicationRuntimeState.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "kanban/KanbanAnimationClock.h"
#include "kanban/KanbanRenderer.h"
#include "kanban/KanbanWindow.h"
#include "kanban/Live2DRenderer.h"
#include "kanban/PlaceholderRenderer.h"
#include "core/Diagnostics.h"

namespace kanban {

namespace {
constexpr const char *kModule = "Kanban";
constexpr int kMinEdgePx = 120;   // 窗口边长下限(与 KanbanWindow 一致)
constexpr int kMaxEdgePx = 2400;  // 上限：防止滚轮搓出超大 surface
constexpr int kBaseWidthPx = 320; // scale=100% 时的窗口宽
constexpr int kBaseHeightPx = 480;
constexpr int kScaleStepPercent = 5; // 滚轮一格 = 5%
// 等 GL 上下文的上限。超时说明这台机器/这次会话根本拿不到 GL 宿主，
// 继续等下去就是状态机卡在 Starting —— 用户看到的正是「点了没反应」。
constexpr int kGlReadyTimeoutMs = 5000;
} // namespace

KanbanController::KanbanController(QObject *parent)
    : QObject(parent)
{
    m_clock = new KanbanAnimationClock(this);
    connect(m_clock, &KanbanAnimationClock::tick, this, &KanbanController::onFrameTick);
    loadSettings();
}

KanbanController::~KanbanController()
{
    // 析构走正常收口路径：先停时钟再释放渲染器，最后删窗口。
    // GL 后端要求「资源在持有上下文的线程释放」，所以 shutdownForExit 里
    // 渲染器 shutdown 必须发生在窗口 delete 之前。
    shutdownForExit();
}

// —— 查询 ——

bool KanbanController::isVisible() const
{
    return m_window && m_window->isVisible();
}

bool KanbanController::live2dAvailable() const
{
    return Live2DRenderer::sdkCompiledIn();
}

QStringList KanbanController::modelNames() const
{
    QStringList names;
    for (const ModelInfo *m : m_models.validModels()) {
        names << m->name;
    }
    return names;
}

QVector<ModelInfo> KanbanController::validModelList() const
{
    QVector<ModelInfo> list;
    const QVector<const ModelInfo *> valid = m_models.validModels();
    list.reserve(valid.size());
    for (const ModelInfo *m : valid) {
        if (m)
            list.append(*m);
    }
    return list;
}

int KanbanController::measuredFps() const
{
    return int(m_clock->measuredFps() + 0.5);
}

// —— 设置读写 ——

void KanbanController::loadSettings()
{
    AppConfig &cfg = AppConfig::instance();
    m_scalePercent = cfg.value(QString::fromLatin1(ConfigKeys::Kanban::Scale), 100).toInt();
    m_opacityPercent = cfg.value(QString::fromLatin1(ConfigKeys::Kanban::Opacity), 100).toInt();
    m_targetFps = cfg.value(QString::fromLatin1(ConfigKeys::Kanban::TargetFps), 30).toInt();
    m_alwaysOnTop = cfg.value(QString::fromLatin1(ConfigKeys::Kanban::AlwaysOnTop), true).toBool();
    m_mouseThrough = cfg.value(QString::fromLatin1(ConfigKeys::Kanban::MouseThrough), false).toBool();
    m_interactionEnabled = cfg.value(QString::fromLatin1(ConfigKeys::Kanban::AllowInteraction), true).toBool();
    m_modelPath = cfg.value(QString::fromLatin1(ConfigKeys::Kanban::ModelPath)).toString();

    m_clock->setTargetFps(m_targetFps);
    emit settingsChanged();
}

bool KanbanController::autoStart() const
{
    return AppConfig::instance().value(QString::fromLatin1(ConfigKeys::Kanban::AutoStart), false).toBool();
}

void KanbanController::setAutoStart(bool on)
{
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::AutoStart), on);
}

bool KanbanController::pauseWhenMainHidden() const
{
    return AppConfig::instance().value(QString::fromLatin1(ConfigKeys::Kanban::PauseWhenHidden), true).toBool();
}

void KanbanController::setPauseWhenMainHidden(bool on)
{
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::PauseWhenHidden), on);
}

int KanbanController::refreshModels()
{
    const int n = m_models.rescan();
    videodiag::log(n > 0 ? videodiag::Level::Info : videodiag::Level::Warning,
                   QStringLiteral("[Kanban] 可用模型 %1 个(目录 %2)")
                       .arg(n).arg(KanbanModelManager::defaultModelsRoot()),
                   QLatin1String(kModule));
    return n;
}

// —— 生命周期 ——

bool KanbanController::pickRenderer()
{
    // 先看 Live2D，不可用即换占位：两条路径对上层完全同形，所以这里不需要
    // 任何「是否已接入 SDK」的分支留给调用方。
    if (!Live2DRenderer::sdkCompiledIn()) {
        videodiag::log(videodiag::Level::Info,
                       QStringLiteral("[Kanban] %1，改用内置占位动画")
                           .arg(Live2DRenderer::unavailableReason()),
                       QLatin1String(kModule));
    } else {
        m_renderer = std::make_unique<Live2DRenderer>();
        m_backendName = QStringLiteral("Live2D Cubism");
        return true;
    }
    m_renderer = std::make_unique<PlaceholderRenderer>();
    m_backendName = QStringLiteral("内置占位动画");
    return true;
}

bool KanbanController::ensureWindow()
{
    if (m_window) {
        return true;
    }
    m_window = new KanbanWindow(nullptr);
    // 顺序是硬要求：先给窗口一个非零尺寸，再挂视图。
    // QOpenGLWidget 只在尺寸非零时才去创建上下文并回调 initializeGL，而窗口尺寸
    // 此前只有 applyScaleToWindow() 会给 —— 它却挂在「等 GL 就绪」之后。
    // 两边互等，表现就是窗口永远 0x0、桌面上什么都没有。
    placeWindowFromConfig();
    m_window->attachRenderer(m_renderer.get());
    m_window->setAlwaysOnTop(m_alwaysOnTop);
    m_window->setMouseThrough(m_mouseThrough);
    m_window->setInteractionEnabled(m_interactionEnabled);
    m_window->setOpacityPercent(m_opacityPercent);

    connect(m_window, &KanbanWindow::pointerEntered, this, [this] {
        // 交互态允许被暂停/收口抢占，所以转移失败也不打断用户操作。
        if (m_machine.is(State::Idle)) {
            m_machine.transition(State::Hover, "pointerEntered");
        }
    });
    connect(m_window, &KanbanWindow::pointerLeft, this, [this] {
        if (m_machine.is(State::Hover) || m_machine.is(State::Clicked)
            || m_machine.is(State::Dragging)) {
            m_machine.transition(State::Idle, "pointerLeft");
        }
    });
    connect(m_window, &KanbanWindow::hoveredAt, this, [this](const QPointF &pos) {
        if (m_renderer && !m_machine.isPaused()) {
            m_renderer->pointerMove(pos);
        }
    });
    connect(m_window, &KanbanWindow::clicked, this, &KanbanController::handleClicked);
    connect(m_window, &KanbanWindow::dragStarted, this, [this] {
        m_machine.transition(State::Dragging, "dragStarted");
    });
    connect(m_window, &KanbanWindow::dragFinished, this, [this] {
        if (m_machine.is(State::Dragging)) {
            m_machine.transition(State::Idle, "dragFinished");
        }
        saveGeometry();
    });
    connect(m_window, &KanbanWindow::scaleStepped, this, &KanbanController::handleScaleStepped);
    connect(m_window, &KanbanWindow::pauseResumeRequested, this, &KanbanController::pauseResume);
    connect(m_window, &KanbanWindow::playNextRequested, this, &KanbanController::playNext);
    connect(m_window, &KanbanWindow::nextExpressionRequested, this,
            &KanbanController::playNextExpression);
    connect(m_window, &KanbanWindow::nextModelRequested, this, [this] {
        // 「切换模型」与「播放下一个动作」是两件事：前者强制换模型。
        const ModelInfo *next = m_models.nextValidAfter(m_modelPath);
        if (next) {
            setModelPath(next->modelJsonPath);
        }
    });
    connect(m_window, &KanbanWindow::hideRequested, this, &KanbanController::hideWindow);
    connect(m_window, &KanbanWindow::settingsRequested, this, [this] {
        emit openSettingsRequested();
    });
    connect(m_window, &KanbanWindow::quitRequested, this, [this] {
        stop();
        emit quitKanbanRequested();
    });
    connect(m_window, &KanbanWindow::glContextReady, this, &KanbanController::onGlContextReady);
    return true;
}

bool KanbanController::start()
{
    if (m_machine.isRunning()) {
        return m_machine.is(State::Error) ? false : true;
    }
    m_lastError.clear();
    if (!m_machine.transition(State::Starting, "start")) {
        return false;
    }

    if (m_models.validModels().isEmpty()) {
        refreshModels();
    }
    publishState();
    if (!pickRenderer()) {
        enterError(QStringLiteral("无可用渲染后端"));
        return false;
    }
    if (!ensureWindow()) {
        enterError(QStringLiteral("窗口创建失败"));
        return false;
    }

    if (m_renderer->usesOpenGL()) {
        // GL 资源必须在持有上下文的线程创建：先 show 触发 initializeGL，
        // 真正的 initialize/loadModel 在 onGlContextReady 里做。
        m_waitingGl = true;
        videodiag::log(videodiag::Level::Info,
                       QStringLiteral("[Kanban] 等待 GL 上下文就绪(后端 %1)").arg(m_backendName),
                       QLatin1String(kModule));
        m_window->show();
        publishState();
        // 兜底：initializeGL 迟迟不来(驱动不给 3.3 上下文、远程桌面、窗口没能
        // 真正上屏)时不能把状态机永远吊在 Starting。宁可降级成占位动画，
        // 也不能让用户面对一个「没有任何反馈」的看板娘。
        QTimer::singleShot(kGlReadyTimeoutMs, this, [this] {
            if (!m_waitingGl) {
                return;
            }
            m_waitingGl = false;
            videodiag::log(videodiag::Level::Warning,
                           QStringLiteral("[Kanban] 等待 GL 上下文超时(%1ms)，降级为内置占位动画")
                               .arg(kGlReadyTimeoutMs),
                           QLatin1String(kModule));
            if (!fallbackToPlaceholder()) {
                enterError(QStringLiteral("GL 上下文未就绪，且降级失败"));
            }
        });
        return true;
    }
    // 软件渲染后端不需要等 GL 上下文，直接初始化；成功后窗口才允许露出，
    // 否则初始化失败会留一个透明空壳在桌面上。
    if (!initializeAndLoad() || !activateKanban()) {
        return false;
    }
    showWindow();
    return true;
}

void KanbanController::onGlContextReady()
{
    videodiag::log(videodiag::Level::Debug,
                   QStringLiteral("[Kanban] 收到 GL 上下文就绪信号(waitingGl=%1)").arg(m_waitingGl),
                   QLatin1String(kModule));
    if (!m_waitingGl) {
        return;
    }
    m_waitingGl = false;

    // 本函数由 QOpenGLWidget::initializeGL() 直接调进来，此刻正处在 Qt 的绘制
    // 流程里，只允许做「必须有当前 GL 上下文」的事：建渲染器、解码并上传纹理。
    // 其余收尾(改窗口尺寸、起时钟、发信号)一律排到本轮事件循环之后再跑。
    if (!initializeAndLoad()) {
        QTimer::singleShot(0, this, [this] {
            if (!fallbackToPlaceholder()) {
                enterError(QStringLiteral("GL 后端初始化失败"));
            }
        });
        return;
    }
    QTimer::singleShot(0, this, [this] {
        if (!activateKanban()) {
            enterError(QStringLiteral("看板娘启动收尾失败"));
        }
    });
}

bool KanbanController::initializeAndLoad()
{
    if (!m_renderer) {
        return false;
    }
    QString err;
    if (!m_renderer->initialize(&err)) {
        m_lastError = err;
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("[Kanban] 渲染后端 %1 初始化失败：%2")
                           .arg(m_backendName, err),
                       QLatin1String(kModule));
        return false;
    }

    // 模型装载失败不算致命：占位动画不依赖模型文件，Live2D 后端没模型时也画不出人，
    // 但状态机仍需可用 —— 所以这里只记日志，并把「无模型」如实报给界面。
    const QVector<const ModelInfo *> valid = m_models.validModels();
    if (!valid.isEmpty()) {
        const ModelInfo *chosen = m_models.byJsonPath(m_modelPath);
        if (!chosen || !chosen->valid) {
            chosen = valid.first();
        }
        QString loadErr;
        if (m_renderer->loadModel(chosen->modelJsonPath, &loadErr)) {
            m_currentModelName = chosen->name;
            m_modelPath = chosen->modelJsonPath;
            AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::ModelPath), m_modelPath);
            emit currentModelChanged(m_currentModelName);
        } else {
            m_currentModelName.clear();
            videodiag::log(videodiag::Level::Warning,
                           QStringLiteral("[Kanban] 模型装载失败：%1").arg(loadErr),
                           QLatin1String(kModule));
        }
    } else {
        m_currentModelName.clear();
        videodiag::log(videodiag::Level::Info,
                       QStringLiteral("[Kanban] 未发现可用模型，使用内置占位形象"),
                       QLatin1String(kModule));
    }

    if (m_window) {
        m_window->setModelDisplayName(m_currentModelName);
    }
    return true;
}

// 启动收尾：挂视图、按缩放定窗口尺寸、转 Idle、起时钟、广播状态。
//
// 单独成函数是为了能在「GL 上下文就绪」之外的地方跑：GL 后端的那次
// initializeAndLoad() 是在 QOpenGLWidget::initializeGL() 里被回调的，也就是
// Qt 的绘制流程内部，那里绝不能改窗口尺寸 —— 会在绘制途中触发 resize 与重入的
// paintGL，Qt 的 FBO 被边画边重建，结果是桌面上一片空白。所以收尾一律排到
// 本轮事件循环之后再执行。
bool KanbanController::activateKanban()
{
    if (!m_renderer) {
        return false;
    }
    if (m_window) {
        // 降级路径会在这里把 GL 视图换成软件视图，所以不能挪到 ensureWindow 里。
        m_window->attachRenderer(m_renderer.get());
    }
    applyScaleToWindow();

    if (!m_machine.transition(State::Idle, "activateKanban")) {
        return false;
    }
    m_clock->setTargetFps(m_targetFps);
    m_clock->start();
    publishState();
    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("[Kanban] 已启动：后端=%1 模型=%2 目标帧率=%3")
                       .arg(m_backendName,
                            m_currentModelName.isEmpty() ? QStringLiteral("无") : m_currentModelName)
                       .arg(m_targetFps),
                   QLatin1String(kModule));
    return true;
}

bool KanbanController::fallbackToPlaceholder()
{
    if (m_renderer && m_renderer->usesOpenGL()) {
        m_renderer->shutdown();
    }
    m_renderer = std::make_unique<PlaceholderRenderer>();
    m_backendName = QStringLiteral("内置占位动画");
    emit backendChanged(m_backendName);
    // 占位后端不碰 GL，但收尾仍走同一条路：activateKanban 会 attach 软件视图。
    return initializeAndLoad() && activateKanban();
}

bool KanbanController::retry()
{
    if (!m_machine.is(State::Error)) {
        return false;
    }
    m_machine.reset(State::Stopped);
    return start();
}

void KanbanController::enterError(const QString &reason)
{
    m_lastError = reason;
    m_clock->stop();
    m_machine.transition(State::Error, "enterError");
    // Error 不是「在跑」：后台任务位必须清掉，否则关窗后进程被一个失败的功能吊着。
    ApplicationRuntimeState::instance().setKanbanState(false, false);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::Enabled), false);
    videodiag::log(videodiag::Level::Error,
                   QStringLiteral("[Kanban] 启动失败：%1").arg(reason),
                   QLatin1String(kModule));
    publishState();
    destroyWindow();
}

void KanbanController::pauseResume()
{
    if (m_machine.is(State::Paused)) {
        if (!m_machine.transition(State::Idle, "resume")) {
            return;
        }
        if (m_renderer) {
            m_renderer->resume();
        }
        m_clock->start();
        videodiag::log(videodiag::Level::Info, QStringLiteral("[Kanban] 恢复动画"),
                       QLatin1String(kModule));
    } else if (m_machine.isRunning() && !m_machine.is(State::Starting)) {
        if (!m_machine.transition(State::Paused, "pause")) {
            return;
        }
        m_clock->stop();
        if (m_renderer) {
            m_renderer->pause();
        }
        videodiag::log(videodiag::Level::Info, QStringLiteral("[Kanban] 暂停动画(不销毁资源)"),
                       QLatin1String(kModule));
    } else {
        return;
    }
    if (m_window) {
        m_window->setPausedVisual(m_machine.isPaused());
        m_window->requestFrame();
    }
    publishState();
}

void KanbanController::stop()
{
    if (!m_machine.isRunning()) {
        m_machine.reset(State::Stopped);
        publishState();
        return;
    }
    m_waitingGl = false;
    m_machine.transition(State::Stopping, "stop");
    m_clock->stop();

    if (m_renderer) {
        m_renderer->shutdown(); // 必须早于窗口销毁：GL 资源要活在上下文里
    }
    destroyWindow();
    m_renderer.reset();

    m_machine.transition(State::Stopped, "stop");
    m_backendName.clear();
    m_currentModelName.clear();
    videodiag::log(videodiag::Level::Info, QStringLiteral("[Kanban] 已停止并释放资源"),
                   QLatin1String(kModule));
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::Enabled), false);
    publishState();
}

void KanbanController::destroyWindow()
{
    if (!m_window) {
        return;
    }
    saveGeometry();
    // 三步顺序都是有理由的，别合并：
    //  1) detachRenderer() 让视图松手。控制器在本函数之后马上 m_renderer.reset()，
    //     而窗口只是排期删除，视图还活着 —— 不松手就是野指针。
    //  2) hide() 立刻不可见。deleteLater() 只是排期，窗口在那之前一直可见，
    //     期间任何一次重绘都可能落到已经被销毁的渲染器上；隐藏后窗口不再
    //     产生绘制事件，这一条路被彻底堵死。顺带用户点「取消」也是立刻消失，
    //     而不是等事件循环转到删除那一步。
    //  3) 最后才排期销毁：不能在这里直接 delete —— 本函数可能是从窗口自己的
    //     事件处理里进来的（右键菜单的「取消看板娘」），删掉 this 等于自杀。
    m_window->detachRenderer();
    m_window->hide();
    m_window->deleteLater();
    m_window = nullptr;
}

void KanbanController::playNext()
{
    if (!m_machine.isRunning() || m_machine.isPaused() || !m_renderer) {
        return;
    }
    if (m_renderer->playNextMotion()) {
        m_machine.transition(State::Clicked, "playNextMotion");
        return;
    }
    // 当前模型没有可播动作 → 换下一个可用模型；只剩一个模型则回 Idle(§7.4)。
    const ModelInfo *next = m_models.nextValidAfter(m_modelPath);
    if (next && next->modelJsonPath != m_modelPath) {
        QString err;
        if (m_renderer->loadModel(next->modelJsonPath, &err)) {
            m_currentModelName = next->name;
            m_modelPath = next->modelJsonPath;
            AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::ModelPath), m_modelPath);
            emit currentModelChanged(m_currentModelName);
        }
        return;
    }
    m_machine.transition(State::Idle, "playNextNoop");
}

int KanbanController::expressionCount() const
{
    // 问渲染器而不是模型表：能不能切表情，最终由后端说了算(占位后端也有几个)。
    return m_renderer ? m_renderer->expressionCount() : 0;
}

void KanbanController::playNextExpression()
{
    if (!m_machine.isRunning() || m_machine.isPaused() || !m_renderer) {
        return;
    }
    // 表情与动作是两个独立通道，所以这里既不换模型也不进 Error：
    // 没有表情就静默返回(界面上的入口本来就该是灰的，走到这里说明是快捷键
    // 或托盘触发)。动作那边「没动作就换模型」是 §7.4 明确要求的兜底，
    // 表情没有这条要求 —— 为了看表情而把用户的模型换掉，是更糟的体验。
    if (m_renderer->playNextExpression()) {
        m_machine.transition(State::Clicked, "playNextExpression");
    }
}

void KanbanController::showWindow()
{
    if (!m_window) {
        start();
        return;
    }
    m_window->show();
    // show 期间时钟是停的(不可见即停)，这里按当前状态恢复。
    if (!m_machine.isPaused() && m_machine.isRunning()) {
        m_clock->start();
    }
    if (m_window->isVisible()) {
        const QRect g = m_window->geometry();
        videodiag::log(videodiag::Level::Debug,
                       QStringLiteral("[Kanban] 窗口已显示 %1x%2 @(%3,%4)")
                           .arg(g.width())
                           .arg(g.height())
                           .arg(g.x())
                           .arg(g.y()),
                       QLatin1String(kModule));
    }
}

void KanbanController::hideWindow()
{
    if (!m_window) {
        return;
    }
    m_window->hide();
    m_clock->stop(); // 不可见就不该继续烧 CPU/GPU
    videodiag::log(videodiag::Level::Debug, QStringLiteral("[Kanban] 暂时隐藏，动画停摆"),
                   QLatin1String(kModule));
}

void KanbanController::applyMainWindowVisible(bool visible)
{
    if (!m_machine.isRunning()) {
        return;
    }
    if (!visible && pauseWhenMainHidden()) {
        if (!m_machine.isPaused()) {
            m_clock->stop();
            if (m_renderer) {
                m_renderer->pause();
            }
            m_machine.transition(State::Paused, "mainHidden");
            if (m_window) {
                m_window->setPausedVisual(true);
            }
            publishState();
        }
    } else if (visible && m_machine.isPaused()) {
        m_machine.transition(State::Idle, "mainShown");
        if (m_renderer) {
            m_renderer->resume();
        }
        if (m_window && m_window->isVisible()) {
            m_clock->start();
        }
        if (m_window) {
            m_window->setPausedVisual(false);
        }
        publishState();
    }
}

void KanbanController::shutdownForExit()
{
    m_clock->stop();
    if (m_renderer) {
        m_renderer->shutdown();
    }
    if (m_window) {
        m_window->detachRenderer();
        delete m_window; // 退出路径立即回收，不等事件循环
        m_window = nullptr;
    }
    m_renderer.reset();
    m_machine.reset(State::Stopped);
    ApplicationRuntimeState::instance().setKanbanState(false, false);
}

// —— 每帧 ——

void KanbanController::onFrameTick(float deltaSeconds)
{
    if (!m_renderer || !m_machine.isRunning() || m_machine.isPaused()) {
        return;
    }
    if (m_window && !m_window->isVisible()) {
        // 双保险：窗口被外部(最小化/隐藏)弄没时不推进动画。
        m_clock->stop();
        return;
    }
    m_renderer->update(deltaSeconds);
    if (m_window) {
        m_window->requestFrame();
    }
}

// —— 设置落地 ——

void KanbanController::handleClicked(const QPointF &localPos)
{
    if (!m_renderer) {
        return;
    }
    m_renderer->pointerClick(localPos);
    m_machine.transition(State::Clicked, "clicked");
}

void KanbanController::handleScaleStepped(int steps)
{
    setScalePercent(qBound(20, m_scalePercent + steps * kScaleStepPercent, 300));
}

void KanbanController::setScalePercent(int percent)
{
    m_scalePercent = qBound(20, percent, 300);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::Scale), m_scalePercent);
    applyScaleToWindow();
    emit settingsChanged();
}

// 建窗时的首次落位。尺寸口径与 applyScaleToWindow() 完全一致(都从
// m_scalePercent 推)，所以 GL 就绪后那次 applyScaleToWindow() 不会让窗口跳一下。
void KanbanController::placeWindowFromConfig()
{
    if (!m_window) {
        return;
    }
    AppConfig &cfg = AppConfig::instance();
    // 位置只在用户真的挪过窗口后才有意义；没存过就交给窗口贴主屏右下角(-1)。
    const int x = cfg.value(QString::fromLatin1(ConfigKeys::Kanban::PosX), -1).toInt();
    const int y = cfg.value(QString::fromLatin1(ConfigKeys::Kanban::PosY), -1).toInt();

    const double s = m_scalePercent / 100.0;
    const int w = qBound(kMinEdgePx, int(kBaseWidthPx * s), kMaxEdgePx);
    const int h = qBound(kMinEdgePx, int(kBaseHeightPx * s), kMaxEdgePx);

    m_window->placeFromConfig(x, y, w, h);
    videodiag::log(videodiag::Level::Debug,
                   QStringLiteral("[Kanban] 窗口首次落位 %1x%2 @(%3,%4)")
                       .arg(m_window->width())
                       .arg(m_window->height())
                       .arg(m_window->x())
                       .arg(m_window->y()),
                   QLatin1String(kModule));
}

void KanbanController::applyScaleToWindow()
{
    if (!m_window) {
        return;
    }
    const double s = m_scalePercent / 100.0;
    const int w = qBound(kMinEdgePx, int(kBaseWidthPx * s), kMaxEdgePx);
    const int h = qBound(kMinEdgePx, int(kBaseHeightPx * s), kMaxEdgePx);

    // 以「底边中心」为锚：角色站在桌面上，缩放时脚不该离地或陷进屏幕。
    //
    // 注意 bottomLeft() 的 y 已经是「顶边 + 高度」了，这里只能再补横向的半宽。
    // 早先多写了一次 height，锚点落到顶边下方 2 倍身高处，于是每执行一次本函数
    // 窗口就往下走一整个身高(479 → 958 → 1437 → 1916)，几次之后彻底掉出屏幕 ——
    // 表现就是「看板娘在运行、窗口枚举得到、但桌面上什么都没有」。
    const QRect before = m_window->geometry();
    const QPoint bottomCenter(before.x() + before.width() / 2,
                              before.y() + before.height());
    m_window->resize(w, h);
    m_window->move(bottomCenter.x() - w / 2, bottomCenter.y() - h);
    saveGeometry();
}

void KanbanController::setOpacityPercent(int percent)
{
    m_opacityPercent = qBound(20, percent, 100);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::Opacity), m_opacityPercent);
    if (m_window) {
        m_window->setOpacityPercent(m_opacityPercent);
    }
    emit settingsChanged();
}

void KanbanController::setTargetFps(int fps)
{
    m_targetFps = qBound(10, fps, 60);
    m_clock->setTargetFps(m_targetFps);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::TargetFps), m_targetFps);
    emit settingsChanged();
}

void KanbanController::setAlwaysOnTop(bool onTop)
{
    m_alwaysOnTop = onTop;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::AlwaysOnTop), onTop);
    if (m_window) {
        m_window->setAlwaysOnTop(onTop);
    }
}

void KanbanController::setMouseThrough(bool through)
{
    m_mouseThrough = through;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::MouseThrough), through);
    if (m_window) {
        m_window->setMouseThrough(through);
    }
}

void KanbanController::setInteractionEnabled(bool enabled)
{
    m_interactionEnabled = enabled;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::AllowInteraction), enabled);
    if (m_window) {
        m_window->setInteractionEnabled(enabled);
    }
}

bool KanbanController::setModelPath(const QString &modelJsonPath)
{
    const ModelInfo *info = m_models.byJsonPath(modelJsonPath);
    if (!info || !info->valid) {
        m_lastError = QStringLiteral("模型不可用");
        return false;
    }
    if (!m_renderer) {
        m_modelPath = modelJsonPath;
        AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::ModelPath), m_modelPath);
        return true;
    }
    QString err;
    if (!m_renderer->loadModel(modelJsonPath, &err)) {
        m_lastError = err;
        return false;
    }
    m_modelPath = modelJsonPath;
    m_currentModelName = info->name;
    m_lastError.clear();
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::ModelPath), m_modelPath);
    if (m_window) {
        m_window->setModelDisplayName(m_currentModelName);
        m_window->requestFrame();
    }
    emit currentModelChanged(m_currentModelName);
    return true;
}

void KanbanController::saveGeometry()
{
    if (!m_window) {
        return;
    }
    const QRect g = m_window->geometry();
    AppConfig &cfg = AppConfig::instance();
    cfg.setValue(QString::fromLatin1(ConfigKeys::Kanban::Width), g.width());
    cfg.setValue(QString::fromLatin1(ConfigKeys::Kanban::Height), g.height());
    cfg.setValue(QString::fromLatin1(ConfigKeys::Kanban::PosX), g.x());
    cfg.setValue(QString::fromLatin1(ConfigKeys::Kanban::PosY), g.y());
}

void KanbanController::publishState()
{
    const bool running = m_machine.isRunning();
    const bool paused = m_machine.isPaused();
    if (running != m_runningPublished) {
        m_runningPublished = running;
        emit runningChanged(running);
    }
    if (paused != m_pausedPublished) {
        m_pausedPublished = paused;
        emit pausedChanged(paused);
    }
    ApplicationRuntimeState::instance().setKanbanState(running, paused);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::Enabled), running);
    emit stateChanged(stateText());
    emit backendChanged(m_backendName);
}

} // namespace kanban

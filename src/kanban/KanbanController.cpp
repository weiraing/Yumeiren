// 看板娘控制器实现。
#include "kanban/KanbanController.h"

#include "app/ApplicationRuntimeState.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "core/SuspendPolicy.h"
#include "platform/windows/desktopmount.h"
#include "kanban/KanbanAnimationClock.h"
#include "kanban/KanbanRenderer.h"
#include "kanban/KanbanWindow.h"
#include "kanban/Live2DRenderer.h"
#include "kanban/PlaceholderRenderer.h"

#include <QElapsedTimer>
#include <QTimer>

namespace kanban {

namespace {
constexpr const char *kModule = "Kanban";
// 等 GL 上下文的上限。超时即降级，否则状态机永远卡在 Starting（表现为「点了没反应」）。
constexpr int kGlReadyTimeoutMs = 5000;
// 挂起判定心跳。锁屏/熄屏都是「持续几分钟起步」的事件，1s 足够。
constexpr int kSuspendHeartbeatMs = 1000;
} // namespace

KanbanController::KanbanController(QObject *parent)
    : QObject(parent)
{
    m_clock = new KanbanAnimationClock(this);
    connect(m_clock, &KanbanAnimationClock::tick, this, &KanbanController::onFrameTick);
    connect(m_clock, &KanbanAnimationClock::measuredFpsChanged,
            this, &KanbanController::measuredFpsChanged);
    // 挂起心跳只在跑起来时挂着，没在跑时无事可判。
    m_suspendClock = std::make_unique<QElapsedTimer>();
    m_suspendTimer = new QTimer(this);
    m_suspendTimer->setTimerType(Qt::CoarseTimer);
    m_suspendTimer->setInterval(kSuspendHeartbeatMs);
    connect(m_suspendTimer, &QTimer::timeout, this, &KanbanController::evaluateSuspend);
    loadSettings();
}

KanbanController::~KanbanController()
{
    // GL 资源必须在持有上下文的线程释放：渲染器 shutdown 要早于窗口 delete。
    shutdownForExit();
}


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

int KanbanController::refreshModels()
{
    const int n = m_models.rescan();
    applog::log(n > 0 ? applog::Level::Info : applog::Level::Warning,
                   QStringLiteral("[Kanban] 可用模型 %1 个(目录 %2)")
                       .arg(n).arg(KanbanModelManager::defaultModelsRoot()),
                   QLatin1String(kModule));
    return n;
}


bool KanbanController::pickRenderer()
{
    // Live2D 不可用即换占位：两条路径对上层同形，调用方无需分支。
    if (!Live2DRenderer::sdkCompiledIn()) {
        applog::log(applog::Level::Info,
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
    // 顺序是硬要求：先给窗口非零尺寸再挂视图 —— QOpenGLWidget 只在尺寸非零时才创建
    // 上下文并回调 initializeGL，而给尺寸的 applyScaleToWindow() 挂在「等 GL 就绪」
    // 之后，两边互等会让窗口永远 0x0。
    placeWindowFromConfig();
    m_window->attachRenderer(m_renderer.get());
    m_window->setAlwaysOnTop(m_alwaysOnTop);
    m_window->setMouseThrough(m_mouseThrough);
    m_window->setInteractionEnabled(m_interactionEnabled);
    m_window->setTransparencyPercent(m_transparencyPercent);
    m_window->setMenuAppearance(m_menuBg, m_menuTransparency, m_menuGlass);

    connect(m_window, &KanbanWindow::pointerEntered, this, [this] {
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
        // 只在没开视线追踪时用窗口 hover 兜底：开着时以每帧读全局光标为准，否则 hover
        // 会把全局光标算出的视线拽回窗口内小范围。
        if (m_renderer && !m_machine.isPaused() && !gazeTracking()) {
            m_renderer->pointerMove(pos);
        }
    });
    connect(m_window, &KanbanWindow::clicked, this, &KanbanController::handleClicked);
    connect(m_window, &KanbanWindow::doubleClicked, this, [this](const QPointF &) {
        if (m_doubleClickSwitchEnabled && m_interactionEnabled) {
            playNext();
        }
    });
    // 视线档位只有两条入口：设置页四档单选框、托盘「看板娘 > 视线追踪 >」。
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
    // 右键菜单的窗口行为开关走控制器而非窗口 setter：只有这条路会同步成员、落盘
    // 配置键并 emit settingsChanged()。
    connect(m_window, &KanbanWindow::mouseThroughRequested, this,
            &KanbanController::setMouseThrough);
    connect(m_window, &KanbanWindow::alwaysOnTopRequested, this,
            &KanbanController::setAlwaysOnTop);
    connect(m_window, &KanbanWindow::pauseResumeRequested, this, &KanbanController::pauseResume);
    connect(m_window, &KanbanWindow::playNextRequested, this, &KanbanController::playNext);
    connect(m_window, &KanbanWindow::nextExpressionRequested, this,
            &KanbanController::playNextExpression);
    connect(m_window, &KanbanWindow::nextModelRequested, this, [this] {
        // 「切换模型」与「切换动作」是两件事：前者强制换模型。
        const ModelInfo *next = m_models.nextValidAfter(m_modelPath);
        if (next) {
            setModelPath(next->modelJsonPath);
        }
    });
    connect(m_window, &KanbanWindow::settingsRequested, this, [this] {
        emit openSettingsRequested();
    });
    connect(m_window, &KanbanWindow::quitRequested, this, [this] {
        // 退出收口：窗口「取消看板娘」只管 stop，后续(配置写回/窗口拆毁)都在
        // stop() 里。过去这里还发过一个 quitKanbanRequested 信号，全工程无人
        // 接收，已删。
        stop();
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
        // GL 资源必须在持有上下文的线程创建：先 show 触发 initializeGL，真正的
        // initialize/loadModel 在 onGlContextReady 里做。
        m_waitingGl = true;
        applog::log(applog::Level::Info,
                       QStringLiteral("[Kanban] 等待 GL 上下文就绪(后端 %1)").arg(m_backendName),
                       QLatin1String(kModule));
        m_window->show();
        publishState();
        // 兜底：迟迟拿不到 GL 上下文时不能把状态机永远吊在 Starting，宁可降级占位。
        QTimer::singleShot(kGlReadyTimeoutMs, this, [this] {
            if (!m_waitingGl) {
                return;
            }
            m_waitingGl = false;
            applog::log(applog::Level::Warning,
                           QStringLiteral("[Kanban] 等待 GL 上下文超时(%1ms)，降级为内置占位动画")
                               .arg(kGlReadyTimeoutMs),
                           QLatin1String(kModule));
            if (!fallbackToPlaceholder()) {
                enterError(QStringLiteral("GL 上下文未就绪，且降级失败"));
            }
        });
        return true;
    }
    // 软件后端不用等 GL 上下文；初始化成功才允许露窗，否则会留一个透明空壳。
    if (!initializeAndLoad() || !activateKanban()) {
        return false;
    }
    showWindow();
    return true;
}

void KanbanController::onGlContextReady()
{
    applog::log(applog::Level::Debug,
                   QStringLiteral("[Kanban] 收到 GL 上下文就绪信号(waitingGl=%1)").arg(m_waitingGl),
                   QLatin1String(kModule));
    if (!m_waitingGl) {
        return;
    }
    m_waitingGl = false;

    // 本函数由 QOpenGLWidget::initializeGL() 直接调进来，正处在 Qt 绘制流程里，只
    // 允许做「必须有当前 GL 上下文」的事；改窗口尺寸等收尾排到事件循环之后。
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
        applog::log(applog::Level::Warning,
                       QStringLiteral("[Kanban] 渲染后端 %1 初始化失败：%2")
                           .arg(m_backendName, err),
                       QLatin1String(kModule));
        return false;
    }

    // 模型装载失败不算致命：状态机仍需可用，只记日志并把「无模型」如实报给界面。
    const QVector<const ModelInfo *> valid = m_models.validModels();
    if (!valid.isEmpty()) {
        const ModelInfo *chosen = m_models.byJsonPath(m_modelPath);
        if (!chosen || !chosen->valid) {
            chosen = valid.first();
        }
        QString loadErr;
        if (m_renderer->loadModel(chosen->modelJsonPath, &loadErr)) {
            // 装载成功即「模型又在显存里」，释放标记必须复位(见 setModelPath)。
            m_releasedForSuspend = false;
            m_currentModelName = chosen->name;
            m_modelPath = chosen->modelJsonPath;
            AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::ModelPath), m_modelPath);
            emit currentModelChanged(m_currentModelName);
        } else {
            m_currentModelName.clear();
            applog::log(applog::Level::Warning,
                           QStringLiteral("[Kanban] 模型装载失败：%1").arg(loadErr),
                           QLatin1String(kModule));
        }
    } else {
        m_currentModelName.clear();
        applog::log(applog::Level::Info,
                       QStringLiteral("[Kanban] 未发现可用模型，使用内置占位形象"),
                       QLatin1String(kModule));
    }

    if (m_window) {
        m_window->setModelDisplayName(m_currentModelName);
    }
    return true;
}

// 启动收尾：挂视图、定窗口尺寸、转 Idle、起时钟、广播状态。单独成函数是为了能在
// 「GL 上下文就绪」之外跑：initializeGL() 里改窗口尺寸会触发重入的 paintGL 把 FBO
// 边画边重建。
bool KanbanController::activateKanban()
{
    if (!m_renderer) {
        return false;
    }
    if (m_window) {
        // 降级路径会在这里把 GL 视图换成软件视图，所以不能挪进 ensureWindow。
        m_window->attachRenderer(m_renderer.get());
    }
    applyScaleToWindow();

    // 视线追踪状态落到渲染器上：这里是 Live2D 成功 / 降级 / 重试三条路径的唯一汇合
    // 点（pickRenderer 会漏掉降级新建的渲染器，ensureWindow 又太早）。
    m_renderer->setGazeStrength(m_gazeStrength);
    m_renderer->setMotionLoopEnabled(m_motionLoopEnabled);
    m_renderer->setSoundEnabled(m_soundEnabled);

    if (!m_machine.transition(State::Idle, "activateKanban")) {
        return false;
    }
    m_clock->setTargetFps(m_targetFps);
    m_clock->start();
    // 清零是必要的：本函数也是重试与降级的汇合点，残留状态会让第一次心跳把刚起来的
    // 实例当成「已经挂起很久」。
    m_suspendReasons = 0;
    m_releasedForSuspend = false;
    m_suspendClock->invalidate();
    m_fakeSuspendClock.invalidate();
    m_suspendTimer->start();
    publishState();
    applog::log(applog::Level::Info,
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
    m_suspendTimer->stop();
    m_machine.transition(State::Error, "enterError");
    // Error 不是「在跑」：后台任务位必须清掉，否则进程会被一个失败的功能吊着。
    ApplicationRuntimeState::instance().setKanbanState(false, false);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::Enabled), false);
    applog::log(applog::Level::Error,
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
        // 挂起中只恢复「暂停态」不恢复绘制：画面此刻依然没人看得见。
        if (m_suspendReasons == 0) {
            m_clock->start();
        }
        applog::log(applog::Level::Info, QStringLiteral("[Kanban] 恢复动画"),
                       QLatin1String(kModule));
    } else if (m_machine.isRunning() && !m_machine.is(State::Starting)) {
        if (!m_machine.transition(State::Paused, "pause")) {
            return;
        }
        m_clock->stop();
        if (m_renderer) {
            m_renderer->pause();
        }
        applog::log(applog::Level::Info, QStringLiteral("[Kanban] 暂停动画(不销毁资源)"),
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
    // 收口是幂等的：Stopping 只存活一瞬，这期间按钮/托盘/窗口事件都可能再进来一次，
    // 那时 renderer 已在 reset 路上，再拆一遍就是对着半销毁的对象动手。
    if (m_machine.is(State::Stopping)) {
        return;
    }
    m_waitingGl = false;
    if (!m_machine.transition(State::Stopping, "stop")) {
        return;
    }
    m_clock->stop();
    m_suspendTimer->stop();
    m_suspendReasons = 0;
    m_releasedForSuspend = false;

    if (m_renderer) {
        m_renderer->shutdown(); // 必须早于窗口销毁：GL 资源要活在上下文里
    }
    destroyWindow();
    m_renderer.reset();

    m_machine.transition(State::Stopped, "stop");
    m_backendName.clear();
    m_currentModelName.clear();
    applog::log(applog::Level::Info, QStringLiteral("[Kanban] 已停止并释放资源"),
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
    // 三步顺序不可合并：1) detachRenderer 让视图松手（控制器随即 m_renderer.reset()，
    // 而窗口只是排期删除）；2) hide 立刻不可见（deleteLater 只是排期，期间重绘可能落
    // 到已销毁的渲染器上）；3) 最后才排期销毁，不能直接 delete。
    m_window->detachRenderer();
    m_window->hide();
    m_window->deleteLater();
    m_window = nullptr;
}

// 把已装载的窗口显示出来。只有 start() 会调到这里，用途是「软件渲染后端启动的最后
// 一步」（GL 后端等上下文时已经 show 过）。
void KanbanController::showWindow()
{
    if (!m_window) {
        return;
    }
    m_window->show();
    // show 期间时钟是停的，这里按当前状态恢复；挂起中例外 —— 起帧交给心跳判定
    // 「挂起原因解除」的那一拍。
    if (!m_machine.isPaused() && m_machine.isRunning() && m_suspendReasons == 0) {
        m_clock->start();
    }
    if (m_window->isVisible()) {
        const QRect g = m_window->geometry();
        applog::log(applog::Level::Debug,
                       QStringLiteral("[Kanban] 窗口已显示 %1x%2 @(%3,%4)")
                           .arg(g.width())
                           .arg(g.height())
                           .arg(g.x())
                           .arg(g.y()),
                       QLatin1String(kModule));
    }
}

void KanbanController::toggleVisible()
{
    // 没在跑：快捷键就是「召唤」，走完整启动路径(含后端降级)。
    if (!m_machine.isRunning()) {
        start();
        return;
    }
    if (!m_window) {
        return;
    }
    if (m_window->isVisible()) {
        // 隐藏即停帧。onFrameTick 对隐藏窗口本就有兜底停帧，这里显式停省一拍；
        // 模型与窗口都保留，回来不用重装载。
        m_clock->stop();
        m_window->hide();
        applog::log(applog::Level::Info,
                       QStringLiteral("[Kanban] 快捷键隐藏：帧时钟已停"),
                       QLatin1String(kModule));
    } else {
        m_window->show();
        // 挂起(锁屏/熄屏)期间不抢着起帧，交给心跳在原因解除的那一拍处理。
        if (!m_machine.isPaused() && m_suspendReasons == 0) {
            m_clock->start();
        }
        applog::log(applog::Level::Info,
                       QStringLiteral("[Kanban] 快捷键显示：帧时钟已恢复"),
                       QLatin1String(kModule));
    }
    publishState();
}


void KanbanController::shutdownForExit()
{
    m_clock->stop();
    m_suspendTimer->stop();
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


void KanbanController::setMonitorOn(bool on)
{
    if (m_monitorOn == on) {
        return;
    }
    m_monitorOn = on;
    // 不等下一拍心跳：电源广播本身就是准确时刻。
    evaluateSuspend();
}

qint64 KanbanController::suspendReleaseThresholdMs()
{
    // 看板娘挂起只判「锁屏/熄屏类看不见」一种，取 kHiddenMs 档(与视频壁纸同值同理，
    // 见 core/SuspendPolicy.h)；环境变量供自动化实测把阈值压到几秒内。
    return suspendpolicy::thresholdWithEnvOverride("YUMEIREN_KANBAN_SUSPEND_MS",
                                                   suspendpolicy::kHiddenMs);
}

// 只喂一个假的「看不见」信号，被测的仍是下游整条链：停帧 → 数到阈值 → 释放模型与
// 纹理 → 信号消失后重新装载。锁屏需人工解锁，无人值守脚本只能这样实测。
int KanbanController::fakeSuspendReasons()
{
    static const int windowMs = qEnvironmentVariableIntValue("YUMEIREN_KANBAN_FAKE_SUSPEND_MS");
    if (windowMs <= 0) {
        return 0;
    }
    if (!m_fakeSuspendClock.isValid()) {
        m_fakeSuspendClock.start();
    }
    return m_fakeSuspendClock.elapsed() < windowMs ? SuspendMonitorOff : 0;
}

void KanbanController::evaluateSuspend()
{
    // Starting 期间不插手：那条路径自己会起时钟，停它会把状态机吊在半路。
    if (!m_machine.isRunning() || m_machine.is(State::Starting)) {
        return;
    }

    int reasons = 0;
    if (winhelper::isWorkstationLocked()) {
        reasons |= SuspendLocked;
    }
    if (!m_monitorOn) {
        reasons |= SuspendMonitorOff;
    }
    reasons |= fakeSuspendReasons();
    const bool wasSuspended = m_suspendReasons != 0;
    m_suspendReasons = reasons;

    if (reasons != 0) {
        if (!wasSuspended) {
            m_clock->stop(); // 停帧：GL 侧不再产生任何 update，GPU 立刻归零
            m_suspendClock->restart();
            applog::log(applog::Level::Info,
                           QStringLiteral("[Kanban] 挂起(原因 0x%1)：帧时钟已停")
                               .arg(reasons, 0, 16),
                           QLatin1String(kModule));
        }
        if (!m_releasedForSuspend && m_suspendClock->isValid()
            && m_suspendClock->elapsed() >= suspendReleaseThresholdMs()) {
            releaseForSuspend();
        }
        return;
    }

    // 既没挂起过也没释放过 = 什么都没发生；正常使用时这条心跳是纯只读的。
    if (!wasSuspended && !m_releasedForSuspend) {
        return;
    }
    if (m_releasedForSuspend) {
        restoreFromSuspend();
        m_releasedForSuspend = false;
    }
    m_suspendClock->invalidate();
    if (!m_machine.isPaused() && isVisible()) {
        m_clock->start();
    }
    applog::log(applog::Level::Info, QStringLiteral("[Kanban] 挂起解除，恢复绘制"),
                   QLatin1String(kModule));
}

void KanbanController::releaseForSuspend()
{
    if (!m_renderer) {
        return;
    }
    // 只拆模型与纹理，不拆渲染器与窗口：前者才是几百 MB 的那一笔。
    m_renderer->unloadModel();
    m_releasedForSuspend = true;
    applog::log(applog::Level::Info,
                   QStringLiteral("[Kanban] 持续挂起 %1ms(原因 0x%2)，已释放模型与纹理，"
                                  "回到桌面自动装载")
                       .arg(m_suspendClock->elapsed())
                       .arg(m_suspendReasons, 0, 16),
                   QLatin1String(kModule));
    winhelper::trimProcessMemory(); // 立刻把腾出来的页还给系统，而不是等它慢慢换出
}

void KanbanController::restoreFromSuspend()
{
    // 走 initializeAndLoad 而非只补 loadModel：挂起期间 DPI/尺寸可能变，纹理上限要
    // 按**当下**的绘制面重算。
    initializeAndLoad();
}

} // namespace kanban

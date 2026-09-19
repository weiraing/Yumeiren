#include "tray/SystemTrayController.h"

#include "app/AppInfo.h"
#include "app/ApplicationRuntimeState.h"
#include "app/ApplicationShutdown.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "kanban/KanbanController.h"
#include "kanban/KanbanRenderer.h" // 视线档位枚举与它的译名函数
#include "wallpaper/VideoWallpaper.h"

#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>

#include <functional>
#include <utility>

namespace {

QMenu *addSubmenu(QMenu *parent, const QString &title)
{
    auto *sub = parent->addMenu(title);
    sub->setToolTipsVisible(false);
    return sub;
}

QAction *addEntry(QMenu *menu, const QString &text, std::function<void()> slot)
{
    QAction *action = menu->addAction(text);
    QObject::connect(action, &QAction::triggered, menu, [slot = std::move(slot)] { slot(); });
    return action;
}

} // namespace

SystemTrayController::SystemTrayController(QObject *parent) : QObject(parent)
{
}

SystemTrayController::~SystemTrayController()
{
    if (m_trayIcon)
        m_trayIcon->hide();
}

void SystemTrayController::setKanbanController(kanban::KanbanController *controller)
{
    m_kanban = controller;
    updateMenuState();
}

bool SystemTrayController::initialize()
{
    if (m_trayIcon)
        return m_available;

    if (!AppConfig::instance()
             .value(ConfigKeys::Tray::Enabled, true).toBool()) {
        videodiag::log(videodiag::Level::Info,
                       QStringLiteral("托盘: 设置中已关闭，不创建托盘图标"),
                       QStringLiteral("Tray"));
        ApplicationRuntimeState::instance().setTrayAvailable(false);
        return false;
    }

    // 先探测再创建：不可用时只降级，不阻止启动。
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("托盘: 系统托盘不可用，主窗口关闭将按正常退出处理"),
                       QStringLiteral("Tray"));
        ApplicationRuntimeState::instance().setTrayAvailable(false);
        return false;
    }

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setToolTip(QStringLiteral("%1 - 动态壁纸 / 看板娘").arg(appinfo::displayName()));
    buildContextMenu();
    m_trayIcon->setContextMenu(m_contextMenu);
    m_trayIcon->setIcon(buildTrayIcon());

    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            &SystemTrayController::onTrayActivated);

    m_available = true;
    ApplicationRuntimeState::instance().setTrayAvailable(true);
    videodiag::log(videodiag::Level::Info, QStringLiteral("托盘: 已就绪(常驻显示)"),
                   QStringLiteral("Tray"));

    // 单一订阅点：后台状态变化时刷菜单文字与 tooltip。
    connect(&ApplicationRuntimeState::instance(), &ApplicationRuntimeState::stateChanged, this,
            &SystemTrayController::updateRuntimeState);

    updateRuntimeState();
    return true;
}

void SystemTrayController::buildContextMenu()
{
    m_contextMenu = new QMenu(nullptr);

    connect(m_contextMenu, &QMenu::aboutToShow, this, &SystemTrayController::updateMenuState);

    QMenu *wallMenu = addSubmenu(m_contextMenu, QStringLiteral("动态壁纸"));

    m_actWallToggle = addEntry(wallMenu, QStringLiteral("启动 / 取消"), [] {
        VideoWallpaper &wall = VideoWallpaper::instance();
        if (wall.isStarted()) {
            wall.stopAll();
            AppConfig::instance().setValue(ConfigKeys::Video::WasPlaying, false);
            return;
        }
        QString err;
        if (!wall.startPlaying(&err)) {
            videodiag::log(videodiag::Level::Warning,
                           QStringLiteral("托盘启动动态壁纸失败: %1").arg(err),
                           QStringLiteral("Tray"));
            return;
        }
        AppConfig::instance().setValue(ConfigKeys::Video::WasPlaying, true);
    });
    m_actWallPause = addEntry(wallMenu, QStringLiteral("暂停"), [] {
        VideoWallpaper::instance().pauseResume();
    });
    m_actWallNext = addEntry(wallMenu, QStringLiteral("播放下一个"), [] {
        VideoWallpaper &wall = VideoWallpaper::instance();
        if (!wall.isStarted() || wall.playlist().isEmpty())
            return; // 无列表或已停止：安全返回，不新建播放器
        // nextTrack() 含自动切歌语义，托盘只做「按列表顺序环移」。
        const int next = (wall.currentIndex() + 1) % wall.playlist().size();
        wall.switchToTrack(next);
    });

    QMenu *kanbanMenu = addSubmenu(m_contextMenu, QStringLiteral("看板娘"));

    m_actKanbanToggle = addEntry(kanbanMenu, QStringLiteral("启动 / 取消"), [this] {
        if (!m_kanban)
            return;
        if (m_kanban->isRunning()) {
            m_kanban->stop();
            return;
        }
        if (!m_kanban->start()) {
            videodiag::log(videodiag::Level::Warning,
                           QStringLiteral("托盘启动看板娘失败: %1")
                               .arg(m_kanban->lastError().isEmpty()
                                        ? QStringLiteral("未知原因")
                                        : m_kanban->lastError()),
                           QStringLiteral("Tray"));
        }
    });
    m_actKanbanPause = addEntry(kanbanMenu, QStringLiteral("暂停"), [this] {
        if (m_kanban)
            m_kanban->pauseResume();
    });
    m_actKanbanNext = addEntry(kanbanMenu, QStringLiteral("播放下一个"), [this] {
        if (m_kanban)
            m_kanban->playNext();
    });

    QMenu *gazeMenu = addSubmenu(kanbanMenu, QStringLiteral("视线追踪"));
    auto addGazeEntry = [this, gazeMenu](int strength) {
        auto *act = gazeMenu->addAction(kanban::KanbanRenderer::gazeStrengthName(strength));
        act->setCheckable(true);
        connect(act, &QAction::triggered, this, [this, strength] {
            if (m_kanban)
                m_kanban->setGazeStrength(strength);
        });
        return act;
    };
    m_actGazeOff = addGazeEntry(kanban::KanbanRenderer::GazeOff);
    m_actGazeWeak = addGazeEntry(kanban::KanbanRenderer::GazeWeak);
    m_actGazeMedium = addGazeEntry(kanban::KanbanRenderer::GazeMedium);
    m_actGazeStrong = addGazeEntry(kanban::KanbanRenderer::GazeStrong);

    m_contextMenu->addSeparator();
    m_actQuit = addEntry(m_contextMenu, QStringLiteral("关闭软件"), [this] {
        emit quitRequested();
        ApplicationShutdown::instance().requestQuit(CloseReason::TrayQuit);
    });

    updateMenuState();
}

void SystemTrayController::updateMenuState()
{
    if (!m_contextMenu)
        return;

    ApplicationRuntimeState &runtime = ApplicationRuntimeState::instance();
    VideoWallpaper &wall = VideoWallpaper::instance();

    const bool wallStarted = wall.isStarted();
    const bool wallHasList = !wall.playlist().isEmpty();

    m_actWallToggle->setEnabled(wallStarted || wallHasList);
    m_actWallPause->setEnabled(wallStarted);
    m_actWallNext->setEnabled(wallStarted && wall.playlist().size() > 1);
    m_actWallPause->setText(wall.isManualPaused() ? QStringLiteral("继续")
                                                  : QStringLiteral("暂停"));

    const bool kanbanRunning = m_kanban && m_kanban->isRunning();
    // 同页面 updateKanbanControls()：唯一该置灰的是 Stopping 那一瞬——窗口与渲染器
    // 正在拆，再点一次会撞上非法状态转移。
    const bool kanbanStopping =
        m_kanban && m_kanban->state() == kanban::State::Stopping;
    m_actKanbanToggle->setEnabled(m_kanban && !kanbanStopping);
    m_actKanbanPause->setEnabled(kanbanRunning);
    // 跟随渲染器的可播动作数置灰：点了没反应比灰掉更让人怀疑程序坏了。
    m_actKanbanNext->setEnabled(kanbanRunning && m_kanban->canPlayNextMotion());
    // 四档开关不禁用：没跑时也能先定偏好，下次启动生效；但没注入控制器时无从
    // 读回状态，此时整组灰掉更诚实。
    {
        const bool gazeKnown = m_kanban != nullptr;
        QAction *gazeActs[] = {m_actGazeOff, m_actGazeWeak, m_actGazeMedium, m_actGazeStrong};
        const int gazeStrengths[] = {kanban::KanbanRenderer::GazeOff,
                                     kanban::KanbanRenderer::GazeWeak,
                                     kanban::KanbanRenderer::GazeMedium,
                                     kanban::KanbanRenderer::GazeStrong};
        // 当前档位只读一次：逐项各读一次的话，中途被改掉会出现两项同时勾选或都没勾。
        const int current = gazeKnown ? m_kanban->gazeStrength() : -1;
        for (int i = 0; i < 4; ++i) {
            gazeActs[i]->setEnabled(gazeKnown);
            gazeActs[i]->setChecked(gazeKnown && current == gazeStrengths[i]);
        }
    }
    if (m_kanban)
        m_actKanbanPause->setText(m_kanban->isPaused() ? QStringLiteral("继续")
                                                       : QStringLiteral("暂停"));

    const QString tip = QStringLiteral("%1 - 动态壁纸%2 / 看板娘%3")
                            .arg(appinfo::displayName(),
                                 runtime.wallpaperRunning()
                                     ? (runtime.wallpaperPaused() ? QStringLiteral("(暂停)")
                                                                  : QStringLiteral("(运行中)"))
                                     : QStringLiteral("(未启动)"),
                                 runtime.kanbanRunning()
                                     ? (runtime.kanbanPaused() ? QStringLiteral("(暂停)")
                                                               : QStringLiteral("(运行中)"))
                                     : QStringLiteral("(未启动)"));
    if (m_trayIcon && m_trayIcon->toolTip() != tip)
        m_trayIcon->setToolTip(tip);
}

void SystemTrayController::updateRuntimeState()
{
    if (!m_available)
        return;

    // 托盘常驻(有没有后台任务都显示)，用户随时能从这里启停壁纸/看板娘。
    showTray();
    updateMenuState();
}

void SystemTrayController::showTray()
{
    if (!m_trayIcon || !m_available)
        return;
    if (!m_trayIcon->isVisible()) {
        m_trayIcon->show();
        videodiag::log(videodiag::Level::Debug, QStringLiteral("托盘: 显示"),
                       QStringLiteral("Tray"));
    }
}

void SystemTrayController::hideTray()
{
    if (!m_trayIcon || !m_available)
        return;
    if (m_trayIcon->isVisible()) {
        m_trayIcon->hide();
        videodiag::log(videodiag::Level::Debug, QStringLiteral("托盘: 隐藏"),
                       QStringLiteral("Tray"));
    }
}

bool SystemTrayController::isVisible() const
{
    return m_trayIcon && m_trayIcon->isVisible();
}

void SystemTrayController::showMainWindow()
{
    emit showMainWindowRequested();
}

void SystemTrayController::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
    case QSystemTrayIcon::DoubleClick:
        showMainWindow();
        break;
    case QSystemTrayIcon::Context:
        updateMenuState();
        break;
    default:
        break; // 中键/Unknown 在 Windows 托盘上没有约定俗成的语义
    }
}

QIcon SystemTrayController::buildTrayIcon() const
{
    const QIcon icon = appinfo::appIcon();
    if (!icon.isNull())
        return icon;

    videodiag::log(videodiag::Level::Warning,
                   QStringLiteral("托盘: 图标资源缺失，退化成占位圆点"),
                   QStringLiteral("Tray"));
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0x2E, 0x30, 0x38), 5.0));
    painter.setBrush(QColor(0xF2, 0xF3, 0xF6));
    painter.drawEllipse(QPointF(32.0, 32.0), 25.0, 25.0);
    painter.end();
    return QIcon(pixmap);
}

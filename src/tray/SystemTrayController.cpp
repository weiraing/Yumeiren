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
#include <QPainterPath>
#include <QPixmap>
#include <QSystemTrayIcon>

#include <functional>
#include <utility>

namespace {

// 托盘菜单二级项的分组标题(§7.2 的「动态壁纸 >」「看板娘 >」)。
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

    // §7.8 先探测再创建：不可用时只降级，不阻止启动，也不留空指针。
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

    // 后台状态一变，托盘菜单项同步刷新(单一订阅点，别处不直连菜单)。
    // 2026-09-17 起可见性不再跟着后台状态走，但菜单文字与 tooltip 还要它。
    connect(&ApplicationRuntimeState::instance(), &ApplicationRuntimeState::stateChanged, this,
            &SystemTrayController::updateRuntimeState);

    updateRuntimeState();
    return true;
}

void SystemTrayController::buildContextMenu()
{
    m_contextMenu = new QMenu(nullptr);
    // 菜单外框的收口不在这里做：外框改成直角后不再需要给窗口裁 region
    // (为什么要放弃圆角见 resources/*.qss 里 QMenu 那段注释 —— region 遮罩是
    // 1 位 alpha，圆角必然是硬台阶)。一二级菜单现在共用同一套 QSS 规则，
    // 样式天然一致，不会再有「一级圆角、二级方角」。
    // 状态在展开瞬间现读，避免「菜单显示的是三秒前的状态」。
    connect(m_contextMenu, &QMenu::aboutToShow, this, &SystemTrayController::updateMenuState);

    m_actShowWindow = addEntry(m_contextMenu, QStringLiteral("显示窗口"),
                               [this] { showMainWindow(); });
    m_contextMenu->addSeparator();

    QMenu *wallMenu = addSubmenu(m_contextMenu, QStringLiteral("动态壁纸"));
    // 「启动 / 取消」是一项双态开关，与动态壁纸页那颗「▶ 启动 / ■ 取消」按钮
    // **同一条判据、同一组副作用**：启动失败要留日志，成功要回写 WasPlaying
    // —— 少了这次回写，从托盘启动的壁纸下次开机不会自动恢复(启动时的自动恢复
    // 就是读这个键，见 MainWindow.cpp 里 earlyWasPlaying 那段)。
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
        // nextTrack() 是内部实现(含自动切歌语义)，托盘只做「按列表顺序环移」。
        const int next = (wall.currentIndex() + 1) % wall.playlist().size();
        wall.switchToTrack(next);
    });

    QMenu *kanbanMenu = addSubmenu(m_contextMenu, QStringLiteral("看板娘"));
    // 同看板娘页的「▶ 启动 / ■ 取消」按钮：判据只看 isRunning()。
    // Error 态也算 running(状态机把它归在 running 集合里)，所以「出错时点一下
    // 是收口而不是重试」这条与页面一致 —— 页面上那句 `|| state()==Error`
    // 在这里是冗余的，写了反而容易让人以为 Error 不属于 running。
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
    // 视线追踪四档：做成子菜单而不是一个可勾选项 —— 四档是互斥的单选语义，
    // 挨在「看板娘」下面平铺四个菜单项会让主菜单变长且看不出互斥关系。
    //
    // 每项都用 setCheckable + 手动回读勾选态(见 updateMenuState)：QAction 的
    // checkable 在子菜单里不会自动互斥，所以「唯一被勾选」这件事必须由回读保证，
    // 权威状态在控制器那边。
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

    m_actShowWindow->setEnabled(!runtime.mainWindowVisible());

    const bool wallStarted = wall.isStarted();
    const bool wallHasList = !wall.playlist().isEmpty();
    // 双态开关的可用性 = 页面 updateVideoButtons() 的 `setEnabled(!empty || started)`：
    // 只有「没列表可播且当前没在跑」才真的没得切；在跑的时候必须可点，否则取消不掉。
    m_actWallToggle->setEnabled(wallStarted || wallHasList);
    m_actWallPause->setEnabled(wallStarted);
    m_actWallNext->setEnabled(wallStarted && wall.playlist().size() > 1);
    m_actWallPause->setText(wall.isManualPaused() ? QStringLiteral("继续")
                                                  : QStringLiteral("暂停"));

    const bool kanbanRunning = m_kanban && m_kanban->isRunning();
    // 同页面 updateKanbanControls()：唯一该置灰的是 Stopping 那一瞬 ——
    // 窗口与渲染器正在拆，此时再点一次既没有可撤销的对象，也会撞上
    // Starting->Stopping 之外的非法转移。Running 与 Error 都必须可点。
    const bool kanbanStopping =
        m_kanban && m_kanban->state() == kanban::State::Stopping;
    m_actKanbanToggle->setEnabled(m_kanban && !kanbanStopping);
    m_actKanbanPause->setEnabled(kanbanRunning);
    // 「播放下一个」跟随渲染器的可播动作数置灰：菜单项点了没反应，
    // 和灰掉一样让人怀疑程序坏了，但灰掉至少不骗人。
    m_actKanbanNext->setEnabled(kanbanRunning && m_kanban->canPlayNextMotion());
    // 四档开关不禁用：看板娘没跑时也可以先把偏好定下来，下次启动生效。
    // 但没注入控制器时无从读回状态，此时整组灰掉更诚实。
    {
        const bool gazeKnown = m_kanban != nullptr;
        QAction *gazeActs[] = {m_actGazeOff, m_actGazeWeak, m_actGazeMedium, m_actGazeStrong};
        const int gazeStrengths[] = {kanban::KanbanRenderer::GazeOff,
                                     kanban::KanbanRenderer::GazeWeak,
                                     kanban::KanbanRenderer::GazeMedium,
                                     kanban::KanbanRenderer::GazeStrong};
        // 当前档位只读一次：这一组里必须恰好有一项被勾上，逐项各读一次
        // 万一中途被别的入口改掉，会出现两项同时勾选或一项都没勾。
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

    // 托盘常驻（2026-09-17 按用户要求撤掉「仅在后台任务运行时显示托盘」勾选框）：
    // 有没有后台任务在跑都显示托盘，用户随时能从这里启停壁纸/看板娘。
    // 仍保留这个函数与它的信号连接 —— 下面还要刷菜单文字与 tooltip。
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
        // 左键单击与双击都显示窗口。Windows 上单击图标不会弹出关联菜单
        // (那是右键的 Context)，所以这个手势是空的，正好给「打开主界面」用 ——
        // 托盘图标最符合直觉的行为就是点一下把窗口叫出来。
        //
        // 「单击真的会走到 Trigger 吗」这点不显然，值得记一笔：Qt 的 Windows 后端
        // 收到的不是 WM_LBUTTONUP，而是 Shell 的通知码 NIN_SELECT，winEvent() 里
        // 它和 NIN_KEYSELECT 一起被映射成 activated(Trigger)
        // (见 qtbase/src/plugins/platforms/windows/qwindowssystemtrayicon.cpp)。
        // 双击则是 WM_LBUTTONDBLCLK → DoubleClick；双击的第二次 NIN_SELECT 会被
        // Qt 用 m_ignoreNextMouseRelease 吞掉，所以一次双击总共触发
        // Trigger + DoubleClick 两条 —— 都落到同一个幂等动作上，不会闪两次。
        showMainWindow();
        break;
    case QSystemTrayIcon::Context:
        // Qt 会自行弹出关联菜单；这里只保证内容是最新的。
        updateMenuState();
        break;
    default:
        break; // MiddleClick/Unknown：中键在 Windows 托盘上没有约定俗成的语义
    }
}

// 图标由代码绘制：exe 里没有嵌 .ico(resources/app.rc 未声明 ICON)，
// 而托盘图标必须在无外部文件时也能出来。画的是虞美人(罂粟科)红花金蕊。
QIcon SystemTrayController::buildTrayIcon() const
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    constexpr int kPetals = 5;
    const QPointF center(32.0, 33.0);
    for (int i = 0; i < kPetals; ++i) {
        const qreal angle = -90.0 + i * (360.0 / kPetals);
        painter.save();
        painter.translate(center);
        painter.rotate(angle);
        QPainterPath petal;
        petal.moveTo(0.0, 0.0);
        petal.cubicTo(QPointF(-15.0, -12.0), QPointF(-11.0, -30.0), QPointF(0.0, -26.0));
        petal.cubicTo(QPointF(11.0, -30.0), QPointF(15.0, -12.0), QPointF(0.0, 0.0));
        painter.fillPath(petal, QColor(i % 2 ? 0xC8 : 0xE0, 0x2B, 0x3B));
        painter.restore();
    }

    // 深色描边让浅色任务栏上也看得清
    painter.setPen(QPen(QColor(0x20, 0x14, 0x18), 2.0));
    painter.setBrush(QColor(0xF6, 0xC4, 0x4A));
    painter.drawEllipse(center, 7.0, 7.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x35, 0x1E, 0x22));
    painter.drawEllipse(center, 3.0, 3.0);
    painter.end();

    return QIcon(pixmap);
}

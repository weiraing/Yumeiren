// 系统托盘控制器(任务书 §7)。
//
// 定位：托盘只是「两个后台功能的第二个入口」，不是新的状态来源。
// 因此本类不存任何 isRunning/isPaused 布尔：菜单项的可用态一律在 aboutToShow
// 时从 ApplicationRuntimeState 现读现算，避免托盘菜单和主界面显示打架。
//
// 三条硬约束：
//   · 菜单只建一次，之后只改 setEnabled()/setText()：反复销毁重建会让
//     Windows 上的托盘菜单在快速点击时闪退(任务书 §7.2)；
//   · 托盘不可用绝不致命：isSystemTrayAvailable() 为 false 时只写警告，
//     主窗口关闭行为自动回退为正常退出(§7.8)；
//   · 退出唯一走 ApplicationShutdown::requestQuit(TrayQuit)，不在这里 stop()。
#ifndef SYSTEMTRAYCONTROLLER_H
#define SYSTEMTRAYCONTROLLER_H

#include <QSystemTrayIcon>

class QMenu;
class QAction;
class QSystemTrayIcon;

namespace kanban {
class KanbanController;
}

class SystemTrayController : public QObject
{
    Q_OBJECT

public:
    explicit SystemTrayController(QObject *parent = nullptr);
    ~SystemTrayController() override;

    // 看板娘控制器由主窗口注入(生命周期归主窗口)；不注入时菜单项保持禁用。
    void setKanbanController(kanban::KanbanController *controller);

    // 建图标与菜单。返回 false = 系统托盘不可用或图标创建失败(均已写日志)。
    bool initialize();

    bool isAvailable() const { return m_available; }
    bool isVisible() const;

    void showTray();
    void hideTray();

    // 后台状态变化时刷新可见性与菜单项(连接 ApplicationRuntimeState::stateChanged)。
    void updateRuntimeState();

    void showMainWindow();

signals:
    void showMainWindowRequested();
    void quitRequested();

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

private:
    void buildContextMenu();
    void updateMenuState();
    QIcon buildTrayIcon() const;

    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_contextMenu = nullptr;
    kanban::KanbanController *m_kanban = nullptr;

    // 稳定 action 指针：只改状态，不重建。
    // 「启动 / 取消」是一个双态开关(与页面上那颗按钮同语义)，不是两个菜单项：
    // 合成一项后子菜单短一行，也不会出现「运行中时启动项灰着、取消项亮着」这种
    // 靠置灰来暗示状态的间接表达。
    QAction *m_actWallToggle = nullptr;
    QAction *m_actWallPause = nullptr;
    QAction *m_actWallNext = nullptr;
    QAction *m_actKanbanToggle = nullptr;
    QAction *m_actKanbanPause = nullptr;
    QAction *m_actKanbanNext = nullptr;
    // 视线追踪四档(无/弱/中/强)，互斥，挂在「看板娘 > 视线追踪 >」子菜单下。
    QAction *m_actGazeOff = nullptr;
    QAction *m_actGazeWeak = nullptr;
    QAction *m_actGazeMedium = nullptr;
    QAction *m_actGazeStrong = nullptr;
    QAction *m_actQuit = nullptr;

    bool m_available = false;
};

#endif // SYSTEMTRAYCONTROLLER_H

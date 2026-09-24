// 系统托盘控制器。
//
// 定位：托盘只是「两个后台功能的第二个入口」，不是新的状态来源 —— 本类不存任何
// isRunning/isPaused 布尔，菜单项可用态一律在 aboutToShow 时从
// ApplicationRuntimeState 现读现算，避免托盘菜单与主界面显示打架。
//
// 三条硬约束：
//   · 菜单只建一次，之后只改 setEnabled()/setText()：反复销毁重建会让 Windows 上的
//     托盘菜单在快速点击时闪退；
//   · 托盘不可用绝不致命：isSystemTrayAvailable() 为 false 时只写警告，主窗口关闭
//     行为自动回退为正常退出；
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

    // 返回 false = 托盘不可用或创建失败(均已写日志)。
    bool initialize();

    bool isAvailable() const { return m_available; }
    bool isVisible() const;

    void showTray();
    void hideTray();

    // 后台状态变化时刷托盘可见性与菜单项。
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

    // 稳定 action 指针：只改状态，不重建。「启动 / 取消」是双态开关，不是两个菜单项。
    QAction *m_actWallToggle = nullptr;
    QAction *m_actWallPause = nullptr;
    QAction *m_actWallNext = nullptr;
    QAction *m_actKanbanToggle = nullptr;
    QAction *m_actKanbanPause = nullptr;
    QAction *m_actKanbanNext = nullptr;
    QAction *m_actKanbanThrough = nullptr; // 鼠标穿透(看板娘下方顶层开关，见 buildContextMenu)
    // 视线追踪四档(无/弱/中/强)，互斥。
    QAction *m_actGazeOff = nullptr;
    QAction *m_actGazeWeak = nullptr;
    QAction *m_actGazeMedium = nullptr;
    QAction *m_actGazeStrong = nullptr;
    // 开机自启：全软件唯一入口（2026-09-24 起，原来壁纸页/看板娘页各有一个同名复选框，
    // 已删）。勾选写 HKCU\...\Run，勾选态每次弹出菜单时从注册表现读（见 updateMenuState）。
    QAction *m_actAutostart = nullptr;
    QAction *m_actQuit = nullptr;

    bool m_available = false;
};

#endif // SYSTEMTRAYCONTROLLER_H

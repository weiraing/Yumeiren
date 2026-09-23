#ifndef APPLICATIONRUNTIMESTATE_H
#define APPLICATIONRUNTIMESTATE_H

#include <QObject>

// 主窗口关闭请求的来路：四者处置完全不同，必须显式区分。
//   UserWindowClose      用户点右上角 ✕ —— 有后台任务则隐藏，否则退出
//   TrayQuit             托盘「关闭软件」 —— 真退出
//   ApplicationShutdown  代码主动收口(退出按钮/注销) —— 真退出
//   SystemShutdown       Windows 注销或关机 —— 真退出，且不再弹任何确认
enum class CloseReason {
    UserWindowClose,
    TrayQuit,
    ApplicationShutdown,
    SystemShutdown
};

class ApplicationRuntimeState : public QObject
{
    Q_OBJECT

public:
    static ApplicationRuntimeState &instance();

    // running：用户启动过且尚未取消(自动挂起也算 running，不等于「此刻正在出画」)。
    // paused ：此刻处于暂停/挂起，仅在 running 为真时有意义。
    bool wallpaperRunning() const { return m_wallpaperRunning; }
    bool wallpaperPaused() const { return m_wallpaperPaused; }
    bool kanbanRunning() const { return m_kanbanRunning; }
    bool kanbanPaused() const { return m_kanbanPaused; }
    void setWallpaperState(bool running, bool paused);
    void setKanbanState(bool running, bool paused);

    void setMainWindowVisible(bool visible);
    void setTrayAvailable(bool available);

    // 任一后台功能在跑，进程就必须活着(关闭主窗口只隐藏)。
    bool hasBackgroundTask() const { return m_wallpaperRunning || m_kanbanRunning; }
    bool shouldKeepProcessAlive() const { return hasBackgroundTask() && !isQuitting(); }

    // 退出闸门：一旦置位，closeEvent 里「隐藏而不退出」必须失效，
    // 否则 quit → closeEvent → hide/ignore → 再 quit 会原地打转。
    bool isQuitting() const { return m_quitting; }
    void setQuitting(bool quitting);

signals:
    // 任一状态位变化都发一次；托盘连这一个信号刷新菜单。
    void stateChanged();

private:
    explicit ApplicationRuntimeState(QObject *parent = nullptr);
    Q_DISABLE_COPY(ApplicationRuntimeState)

    bool m_wallpaperRunning = false;
    bool m_wallpaperPaused = false;
    bool m_kanbanRunning = false;
    bool m_kanbanPaused = false;
    bool m_mainWindowVisible = true;
    bool m_trayAvailable = false;
    bool m_quitting = false;
};

#endif // APPLICATIONRUNTIMESTATE_H

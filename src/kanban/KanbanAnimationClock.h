// 统一动画时钟：按真实间隔推进，每秒发布统计；暂停与隐藏由控制器停表。
#ifndef KANBANANIMATIONCLOCK_H
#define KANBANANIMATIONCLOCK_H

#include <QObject>
#include <QElapsedTimer>

class QTimer;

namespace kanban {

class KanbanAnimationClock : public QObject
{
    Q_OBJECT

public:
    explicit KanbanAnimationClock(QObject *parent = nullptr);
    ~KanbanAnimationClock() override;

    // 目标帧率(10..60)。运行中改帧率会立即生效，不重启定时器。
    void setTargetFps(int fps);
    int targetFps() const { return m_targetFps; }

    void start();
    void stop();
    bool isRunning() const { return m_running; }

    // 实测帧率统计(诊断面板用；不驱动任何逻辑)。
    double measuredFps() const { return m_measuredFps; }

signals:
    // deltaSeconds 为距上一帧的真实秒数(首帧与恢复后的第一帧给一个保守值)。
    void tick(float deltaSeconds);
    // 每秒统计及停止归零时通知界面，不随每帧刷新控件。
    void measuredFpsChanged();

private:
    void onTimeout();
    void applyInterval();

    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
    bool m_running = false;
    int m_targetFps = 30;
    qint64 m_lastMs = -1;
    // 帧率统计窗口：累计 1s 内的 tick 数后换算，避免每帧做浮点除法
    int m_frameAccum = 0;
    qint64 m_fpsWindowStart = 0;
    double m_measuredFps = 0.0;
};

} // namespace kanban

#endif // KANBANANIMATIONCLOCK_H

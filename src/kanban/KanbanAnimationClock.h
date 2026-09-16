// 看板娘统一动画时钟(任务书 §5.3)：整个模块只有一个 QTimer 驱动动画。
//
// 关键点：
//   · delta 用 QElapsedTimer 实测，不按 interval 累加 —— 固定累加在系统卡顿、
//     窗口不可见、DPI 变更后会让动画速度漂移；
//   · 目标帧率只是「不要跑太快」的上限：interval 取 1000/fps，实际 delta 仍走实测值；
//   · stop() 幂等，start() 重复调用不会造出第二个定时器；
//   · 不可见即停：窗口 hide / 主窗口最小化 / 暂停都走 stop()，省 CPU 与 GPU。
#ifndef KANBANANIMATIONCLOCK_H
#define KANBANANIMATIONCLOCK_H

#include <QObject>

class QElapsedTimer;
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

private:
    void onTimeout();
    void applyInterval();

    QTimer *m_timer = nullptr;
    QElapsedTimer *m_clock = nullptr;
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

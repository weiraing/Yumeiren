#include "kanban/KanbanAnimationClock.h"

#include <QElapsedTimer>
#include <QTimer>

namespace kanban {

// delta 上限：切窗口/系统睡眠后第一帧的实测间隔可能有几十秒，
// 直接喂给动画参数会让呼吸和动作瞬间跳完，观感是「闪一下」。
constexpr float kMaxDeltaSeconds = 0.1f;

KanbanAnimationClock::KanbanAnimationClock(QObject *parent) : QObject(parent)
{
    m_timer = new QTimer(this);
    // CoarseTimer：让系统把我们的 tick 与它自己的唤醒合并，省电；
    // 动画只要求「每秒约 30 次」，不要求精确到毫秒。
    m_timer->setTimerType(Qt::CoarseTimer);
    connect(m_timer, &QTimer::timeout, this, &KanbanAnimationClock::onTimeout);
    m_clock = new QElapsedTimer();
}

KanbanAnimationClock::~KanbanAnimationClock()
{
    m_timer->stop();
}

void KanbanAnimationClock::setTargetFps(int fps)
{
    const int clamped = qBound(10, fps, 60);
    if (clamped == m_targetFps)
        return;
    m_targetFps = clamped;
    applyInterval();
}

void KanbanAnimationClock::applyInterval()
{
    const int interval = qMax(1, int(1000.0 / double(m_targetFps)));
    m_timer->setInterval(interval);
}

void KanbanAnimationClock::start()
{
    if (m_running)
        return; // 重复 start 不得造出第二个 tick 源
    m_running = true;
    m_clock->restart();
    m_lastMs = -1;
    m_frameAccum = 0;
    m_fpsWindowStart = m_clock->elapsed();
    applyInterval();
    m_timer->start();
}

void KanbanAnimationClock::stop()
{
    if (!m_running)
        return;
    m_running = false;
    m_timer->stop();
    m_measuredFps = 0.0;
}

void KanbanAnimationClock::onTimeout()
{
    const qint64 now = m_clock->elapsed();
    float delta = kMaxDeltaSeconds;
    if (m_lastMs >= 0)
        delta = qMin(float(now - m_lastMs) / 1000.0f, kMaxDeltaSeconds);
    m_lastMs = now;

    ++m_frameAccum;
    if (now - m_fpsWindowStart >= 1000) {
        m_measuredFps = double(m_frameAccum) * 1000.0 / double(now - m_fpsWindowStart);
        m_frameAccum = 0;
        m_fpsWindowStart = now;
    }
    emit tick(delta);
}

} // namespace kanban

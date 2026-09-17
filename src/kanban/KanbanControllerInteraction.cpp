// 看板娘动作、表情、帧推进与光标映射。
#include "kanban/KanbanController.h"

#include "kanban/KanbanAnimationClock.h"
#include "kanban/KanbanRenderer.h"
#include "kanban/KanbanWindow.h"

#include <QCursor>

namespace kanban {

void KanbanController::playNext()
{
    if (!m_machine.isRunning() || m_machine.isPaused() || !m_renderer) {
        return;
    }
    if (m_renderer->playNextMotion()) {
        // 连续互动不重复转移，避免状态机误报非法转移。
        if (!m_machine.is(State::Clicked)) {
            m_machine.transition(State::Clicked, "playNextMotion");
        }
        return;
    }
    // 无动作时保持当前模型，换模型由独立入口处理。
    m_machine.transition(State::Idle, "playNextNoop");
}

int KanbanController::playableMotionCount() const
{
    return m_renderer ? m_renderer->playableMotionCount() : 0;
}

bool KanbanController::canPlayNextMotion() const
{
    return m_renderer ? m_renderer->canPlayNextMotion() : false;
}

int KanbanController::expressionCount() const
{
    return m_renderer ? m_renderer->expressionCount() : 0;
}

void KanbanController::playNextExpression()
{
    if (!m_machine.isRunning() || m_machine.isPaused() || !m_renderer) {
        return;
    }
    // 表情与动作独立；缺少表情不触发换模型或错误状态。
    if (m_renderer->playNextExpression()) {
        if (!m_machine.is(State::Clicked)) {
            m_machine.transition(State::Clicked, "playNextExpression");
        }
    }
}

void KanbanController::onFrameTick(float deltaSeconds)
{
    if (!m_renderer || !m_machine.isRunning() || m_machine.isPaused()) {
        return;
    }
    if (m_window && !m_window->isVisible()) {
        m_clock->stop();
        return;
    }
    m_renderer->update(deltaSeconds);
    // 保留后端既有顺序：推进动画后更新光标目标，再请求绘制。
    if (gazeTracking()) {
        feedGazeTarget();
    }
    if (m_window) {
        m_window->requestFrame();
    }
}

void KanbanController::feedGazeTarget()
{
    if (!m_renderer || !m_window || !m_window->isVisible()) {
        return;
    }
    // 全局采样支持窗外跟随；交给 Qt 换算坐标以适配多屏缩放。
    const QPoint local = m_window->mapFromGlobal(QCursor::pos());
    m_renderer->pointerMove(QPointF(local));
}

void KanbanController::handleClicked(const QPointF &localPos)
{
    if (!m_renderer) {
        return;
    }
    m_renderer->pointerClick(localPos);
    m_machine.transition(State::Clicked, "clicked");
}

} // namespace kanban

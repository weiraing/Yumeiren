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
    // 用户显式点了「切换动作」= 人就在机器前。电源广播若卡在「显示器关」（实测过
    // 一路挂起到拆掉模型、且再没翻回来），借这一下把它翻回「开」—— 否则下面刚恢复的
    // 模型会在下一次心跳里又被拆掉。setMonitorOn 内部走 evaluateSuspend，恢复与起帧
    // 一并完成。
    if (!m_monitorOn) {
        setMonitorOn(true);
    }
    if (m_renderer->playNextMotion()) {
        // 连续互动不重复转移，避免状态机误报非法转移。
        if (!m_machine.is(State::Clicked)) {
            m_machine.transition(State::Clicked, "playNextMotion");
        }
        return;
    }
    // 无动作时保持当前模型，换模型由独立入口处理。已经待机就别再转一次：状态机会把
    // 「待机 → 待机」判成非法转移而刷警告，掩盖真正的失败原因。
    if (!m_machine.is(State::Idle)) {
        m_machine.transition(State::Idle, "playNextNoop");
    }
}

int KanbanController::playableMotionCount() const
{
    return m_renderer ? m_renderer->playableMotionCount() : 0;
}

int KanbanController::currentMotionOrdinal() const
{
    return m_renderer ? m_renderer->currentMotionOrdinal() : 0;
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
    // 纹理上限要跟着绘制面走。重建不能放在 resize() 里：resize 来自 resizeGL，
    // 仍在绘制流程内，在那里重建即重入 paintGL，画面会整片空白。
    m_renderer->rebuildTexturesIfNeeded();
    m_renderer->update(deltaSeconds);
    // 顺序有依赖：推进动画后更新光标目标，再请求绘制。
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

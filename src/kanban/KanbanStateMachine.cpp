// 看板娘状态机实现：状态转移的唯一切面。
//
// 设计口径：
//   · 只允许表里列出的转移，其余一律拒绝并写日志(不静默) —— 真机跑起来时
//     日志里出现「拒绝非法状态转移」就是逻辑漏点，必须修而不是屏蔽；
//   · 自反转移(from == to)也拒绝，避免托盘/窗口事件重复投递被当成成功；
//   · 交互态 Hover / Clicked / Dragging 之间必须能回到 Idle，
//     且任何一种交互态都允许被 Paused / Stopping 抢占(用户随时能暂停或取消)。
#include "kanban/KanbanStateMachine.h"

#include "videodiag.h"

namespace kanban {

bool KanbanStateMachine::allowed(State from, State to)
{
    if (from == to) {
        return false;
    }

    switch (from) {
    case State::Stopped:
        return to == State::Starting;

    case State::Starting:
        return to == State::Idle || to == State::Error || to == State::Stopping;

    case State::Idle:
        return to == State::Hover || to == State::Clicked || to == State::Dragging
            || to == State::Paused || to == State::Stopping || to == State::Error;

    case State::Hover:
        return to == State::Idle || to == State::Clicked || to == State::Dragging
            || to == State::Paused || to == State::Stopping;

    case State::Clicked:
        return to == State::Idle || to == State::Hover || to == State::Paused
            || to == State::Stopping;

    case State::Dragging:
        return to == State::Idle || to == State::Hover || to == State::Paused
            || to == State::Stopping;

    case State::Paused:
        // 暂停中不接受交互态：时钟停摆时指针事件不该改状态，恢复后回 Idle。
        return to == State::Idle || to == State::Stopping || to == State::Error;

    case State::Stopping:
        // 收口途中的任何事件都被丢弃，因此这里唯一出路是 Stopped。
        return to == State::Stopped;

    case State::Error:
        // 失败态保留可重试入口：Starting(重试) 或 Stopped(用户放弃)。
        return to == State::Starting || to == State::Stopped;
    }
    return false;
}

bool KanbanStateMachine::transition(State to, const char *caller)
{
    const State from = m_state;
    if (!allowed(from, to)) {
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("[Kanban] 拒绝非法状态转移 %1 -> %2 (caller=%3)")
                           .arg(kanban::stateText(from), kanban::stateText(to),
                                QString::fromUtf8(caller ? caller : "?")),
                       QStringLiteral("Kanban"));
        return false;
    }

    m_state = to;
    videodiag::log(videodiag::Level::Debug,
                   QStringLiteral("[Kanban] 状态 %1 -> %2 (caller=%3)")
                       .arg(kanban::stateText(from), kanban::stateText(to),
                            QString::fromUtf8(caller ? caller : "?")),
                   QStringLiteral("Kanban"));
    return true;
}

void KanbanStateMachine::reset(State to)
{
    m_state = to;
}

} // namespace kanban

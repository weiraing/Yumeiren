// 看板娘状态机(任务书 §4.1)：状态转移的唯一切面。
//
// 存在的意义是把「谁能进哪个状态」写死在一处，窗口/控制器/托盘都问这里，
// 不允许在窗口类里堆 isStarted/isPausing/isHiding 这类布尔组合。
// 非法转移不静默忽略：返回 false 并写日志，真机跑起来才能发现逻辑漏点。
#ifndef KANBANSTATEMACHINE_H
#define KANBANSTATEMACHINE_H

#include "kanban/KanbanTypes.h"

class QObject;

namespace kanban {

class KanbanStateMachine
{
public:
    State state() const { return m_state; }
    // 注意限定 kanban:: —— 成员名会隐藏同名自由函数，直接写 stateText(m_state)
    // 会在类内查找时就停下，编译期报「参数过多」。
    QString stateText() const { return kanban::stateText(m_state); }

    bool is(State s) const { return m_state == s; }
    bool isRunning() const { return stateIsRunning(m_state); }
    bool isPaused() const { return m_state == State::Paused; }

    // 请求转移；onRejected 非空时非法转移会把调用方上下文写进日志。
    bool transition(State to, const char *caller = nullptr);
    // 只做判定，不改状态。
    static bool allowed(State from, State to);
    // 强制复位(仅析构/退出收口使用，绕过转移表)。
    void reset(State to = State::Stopped);

private:
    State m_state = State::Stopped;
};

} // namespace kanban

#endif // KANBANSTATEMACHINE_H

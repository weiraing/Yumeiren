// 看板娘模块公共类型：状态机取值、状态文案。
//
// 状态集合为 KanbanStateMachine 允许的第一阶段子集。交互态 Hover / Clicked /
// Dragging 纳入状态机，是为了不在窗口类里堆布尔变量。
#ifndef KANBANTYPES_H
#define KANBANTYPES_H

#include <QMetaType>
#include <QString>

namespace kanban {

enum class State {
    Stopped,   // 未运行：窗口不存在
    Starting,  // 正在初始化(SDK/模型/窗口)
    Idle,      // 待机动画循环中
    Hover,     // 鼠标悬停在模型上
    Clicked,   // 单击触发的一次性反馈
    Dragging,  // 正在被拖动移动窗口
    Paused,    // 用户暂停：动画时钟停摆，不更新模型参数
    Stopping,  // 正在收口
    Error,     // 初始化失败：保留可重试入口
};

inline QString stateText(State s)
{
    switch (s) {
    case State::Stopped: return QStringLiteral("未运行");
    case State::Starting: return QStringLiteral("启动中");
    case State::Idle: return QStringLiteral("待机");
    case State::Hover: return QStringLiteral("悬停");
    case State::Clicked: return QStringLiteral("互动");
    case State::Dragging: return QStringLiteral("拖动中");
    case State::Paused: return QStringLiteral("已暂停");
    case State::Stopping: return QStringLiteral("停止中");
    case State::Error: return QStringLiteral("启动失败");
    }
    return QStringLiteral("未知");
}

// 是否处于「已启动」的用户语义：除 Stopped/Error 外都算在跑。Error 特意不算，
// 初始化失败后托盘不该给出「暂停/取消」这类无效项。
inline bool stateIsRunning(State s)
{
    switch (s) {
    case State::Stopped:
    case State::Error:
        return false;
    case State::Starting:
    case State::Idle:
    case State::Hover:
    case State::Clicked:
    case State::Dragging:
    case State::Paused:
    case State::Stopping:
        return true;
    }
    return false;
}

} // namespace kanban

Q_DECLARE_METATYPE(kanban::State)

#endif // KANBANTYPES_H

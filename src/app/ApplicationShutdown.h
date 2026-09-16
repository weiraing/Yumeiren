// 统一退出收口(任务书 §7.6 / §7.7)。
//
// 为什么要有这个类：退出路径有四条(窗口 ✕、托盘「关闭软件」、程序内退出按钮、
// Windows 注销关机)，每条都想「先停干净再 quit」。如果四处各写一遍清理顺序，
// 必然出现某条路径漏掉 stopAll() 或漏掉 save()。这里把顺序固化成一份可注册的
// 步骤表，四条路径都只调 requestQuit(reason)。
//
// 递归防护：requestQuit() 第一次进入即置 quitting 标志，后续调用直接返回；
// quit() 触发的 closeEvent 再想调 requestQuit() 也进不来，因此不会出现
// closeEvent → quit → closeEvent 的循环(§7.7)。
#ifndef APPLICATIONSHUTDOWN_H
#define APPLICATIONSHUTDOWN_H

#include <QObject>
#include <QString>

#include <functional>
#include <utility>
#include <vector>

#include "app/ApplicationRuntimeState.h" // CloseReason

class ApplicationShutdown : public QObject
{
    Q_OBJECT

public:
    static ApplicationShutdown &instance();

    // 注册清理步骤。后注册的先执行(析构顺序)，于是各模块可以「谁最后初始化谁先拆」。
    // 步骤必须自身幂等：退出路径上可能被多个入口催动，但只会跑一次。
    void addStep(const QString &name, std::function<void()> step);

    // 唯一退出入口。reason 只用于日志归因，不改变清理顺序。
    void requestQuit(CloseReason reason = CloseReason::ApplicationShutdown);

    bool isQuitRequested() const { return m_started; }

    static QString reasonText(CloseReason reason);

private:
    explicit ApplicationShutdown(QObject *parent = nullptr);
    Q_DISABLE_COPY(ApplicationShutdown)

    void runSteps();

    std::vector<std::pair<QString, std::function<void()>>> m_steps;
    bool m_started = false;
};

#endif // APPLICATIONSHUTDOWN_H

#include "app/ApplicationShutdown.h"

#include <QApplication>
#include <QElapsedTimer>

#include "config/AppConfig.h"
#include "core/Diagnostics.h"

ApplicationShutdown &ApplicationShutdown::instance()
{
    // 泄漏式单例：退出流程本身会走到这里，析构期再释放反而危险。
    static ApplicationShutdown *self = new ApplicationShutdown(qApp);
    return *self;
}

ApplicationShutdown::ApplicationShutdown(QObject *parent) : QObject(parent)
{
}

QString ApplicationShutdown::reasonText(CloseReason reason)
{
    switch (reason) {
    case CloseReason::UserWindowClose:
        return QStringLiteral("用户点击窗口关闭");
    case CloseReason::TrayQuit:
        return QStringLiteral("托盘关闭软件");
    case CloseReason::ApplicationShutdown:
        return QStringLiteral("程序主动退出");
    case CloseReason::SystemShutdown:
        return QStringLiteral("系统注销或关机");
    }
    return QStringLiteral("未知来路");
}

void ApplicationShutdown::addStep(const QString &name, std::function<void()> step)
{
    if (!step)
        return;
    if (m_started) {
        // 退出已经开始还来注册：立刻补做，别把这一步静默丢掉。
        step();
        return;
    }
    m_steps.emplace_back(name, std::move(step));
}

void ApplicationShutdown::runSteps()
{
    for (auto it = m_steps.rbegin(); it != m_steps.rend(); ++it) {
        QElapsedTimer timer;
        timer.start();
        videodiag::log(videodiag::Level::Debug,
                       QStringLiteral("退出清理: %1").arg(it->first),
                       QStringLiteral("Shutdown"));
        it->second();
        if (const qint64 cost = timer.elapsed(); cost >= 50)
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("退出清理 %1 耗时 %2ms").arg(it->first).arg(cost),
                           QStringLiteral("Shutdown"));
    }
}

void ApplicationShutdown::requestQuit(CloseReason reason)
{
    if (m_started) {
        // §7.7 递归闸门：quit() 引发的第二次关闭请求(以及注销/关机同时到达)
        // 在这里被吃掉，不再重复清理，也不再改来路文本。
        videodiag::log(videodiag::Level::Debug,
                       QStringLiteral("退出请求重复(来路=%1)，忽略")
                           .arg(reasonText(reason)),
                       QStringLiteral("Shutdown"));
        return;
    }
    m_started = true;

    // 先落下退出标志：主窗口 closeEvent 里「隐藏而不退出」的判断依赖它。
    ApplicationRuntimeState::instance().setQuitting(true);

    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("退出开始，来路: %1").arg(reasonText(reason)),
                   QStringLiteral("Shutdown"));

    runSteps();

    AppConfig::instance().save();
    videodiag::log(videodiag::Level::Info, QStringLiteral("退出收口完成，结束事件循环"),
                   QStringLiteral("Shutdown"));
    QApplication::quit();
}

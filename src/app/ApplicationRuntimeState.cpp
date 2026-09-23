#include "app/ApplicationRuntimeState.h"

#include "core/Diagnostics.h"

ApplicationRuntimeState &ApplicationRuntimeState::instance()
{
    static ApplicationRuntimeState s;
    return s;
}

ApplicationRuntimeState::ApplicationRuntimeState(QObject *parent) : QObject(parent)
{
}

void ApplicationRuntimeState::setWallpaperState(bool running, bool paused)
{
    if (m_wallpaperRunning == running && m_wallpaperPaused == paused)
        return;
    m_wallpaperRunning = running;
    m_wallpaperPaused = paused;
    applog::log(applog::Level::Debug,
                   QStringLiteral("运行状态: 动态壁纸 running=%1 paused=%2")
                       .arg(running)
                       .arg(paused),
                   QStringLiteral("RuntimeState"));
    emit stateChanged();
}

void ApplicationRuntimeState::setKanbanState(bool running, bool paused)
{
    if (m_kanbanRunning == running && m_kanbanPaused == paused)
        return;
    m_kanbanRunning = running;
    m_kanbanPaused = paused;
    applog::log(applog::Level::Debug,
                   QStringLiteral("运行状态: 看板娘 running=%1 paused=%2")
                       .arg(running)
                       .arg(paused),
                   QStringLiteral("RuntimeState"));
    emit stateChanged();
}

void ApplicationRuntimeState::setMainWindowVisible(bool visible)
{
    if (m_mainWindowVisible == visible)
        return;
    m_mainWindowVisible = visible;
    emit stateChanged();
}

void ApplicationRuntimeState::setTrayAvailable(bool available)
{
    if (m_trayAvailable == available)
        return;
    m_trayAvailable = available;
    emit stateChanged();
}

void ApplicationRuntimeState::setQuitting(bool quitting)
{
    if (m_quitting == quitting)
        return;
    m_quitting = quitting;
    applog::log(applog::Level::Info,
                   QStringLiteral("运行状态: 退出闸门 %1").arg(quitting ? QStringLiteral("已置位")
                                                                        : QStringLiteral("解除")),
                   QStringLiteral("RuntimeState"));
    emit stateChanged();
}

// VideoWallpaper 的设置读写与配置同步（播放模式、音量、屏幕模式等）。
#include "VideoWallpaper.h"

#include "config/AppConfig.h"
#include "core/Diagnostics.h"

#include <QAudioOutput>
#include <QDir>
#include <QFile>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QVideoWidget>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Powrprof.lib")
#endif

namespace {
QString screenModeName(int mode)
{
    switch (mode) {
    case 0: return QStringLiteral("单屏");
    case 1: return QStringLiteral("扩展");
    case 2: return QStringLiteral("克隆");
    default: return QStringLiteral("未知(%1)").arg(mode);
    }
}
} // namespace


void VideoWallpaper::setScreenMode(int mode)
{
    const int prev = m_screenMode;
    m_screenMode = qBound(0, mode, 2);
    if (m_screenMode == prev)
        return; // 值未变时不重建：启动时 loadSettings 会对已运行的管线重复调用，
                // 重建会短暂保留上一代窗口，平白多出一份渲染表面
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("显示模式切换: %1→%2 outputs=%3")
            .arg(screenModeName(prev)).arg(screenModeName(m_screenMode))
            .arg(m_outputs.size()));
    if (!m_outputs.isEmpty()) {
        layoutOutputs();
        // 重建后恢复播放(状态机会在全屏等挂起原因下保持暂停)
        if (m_started && !m_manualPaused && !m_playlist.isEmpty()) {
            playIndex(qMax(0, m_index));
            evaluateSuspend();
        }
    }
}
void VideoWallpaper::setPauseOnFullscreen(bool on)
{
    m_pauseOnFullscreen = on;
    evaluateSuspend();
}
void VideoWallpaper::setPauseOnBattery(bool on)
{
    m_pauseOnBattery = on;
    evaluateSuspend();
}
void VideoWallpaper::setMonitorOn(bool on)
{
    if (m_monitorOn == on)
        return;
    m_monitorOn = on;
    evaluateSuspend();
}
void VideoWallpaper::setTargetFps(int fps)
{
    m_targetFps = qBound(0, fps, 240);
    for (const VideoOutput &out : std::as_const(m_outputs))
        if (isLiveOutput(out))
            applyPlaybackRate(out.player);
}
void VideoWallpaper::applyPlaybackRate(QMediaPlayer *player)
{
    if (!player)
        return;
    double rate = 1.0;
    if (m_targetFps > 0) {
        const double src = player->metaData()
                               .value(QMediaMetaData::VideoFrameRate)
                               .toDouble();
        if (src > m_targetFps + 0.5)
            rate = m_targetFps / src;
    }
    videodiag::log(videodiag::Level::Debug,
        QStringLiteral("setPlaybackRate(%1)").arg(rate));
    player->setPlaybackRate(rate);
}
void VideoWallpaper::setVolume(int percent)
{
    m_volume = qBound(0, percent, 100);
    bool first = true;
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (!isLiveOutput(out) || !out.audio)
            break; // 列表已失效(清理中/已卸载)：整体停手，不逐条解引用
        out.audio->setMuted(!first);
        out.audio->setVolume(m_volume / 100.0);
        if (first)
            applyAudioPolicy(out, true); // 音量归零时连音频轨一起停掉
        first = false;
    }
}
void VideoWallpaper::applyAudioPolicy(const VideoOutput &out, bool carriesAudio)
{
    if (!isLiveOutput(out))
        return;
    out.player->setActiveAudioTrack(carriesAudio && m_volume > 0 ? 0 : -1);
}
void VideoWallpaper::applyLoopPolicy(QMediaPlayer *player)
{
    if (!player)
        return;
    const bool seamlessLoop = isSeamlessLoop();
    player->setLoops(seamlessLoop ? QMediaPlayer::Infinite : QMediaPlayer::Once);
    videodiag::log(videodiag::Level::Debug,
        QStringLiteral("setLoops(%1) player=%2 widget=%3 source=%4")
            .arg(seamlessLoop ? QStringLiteral("Infinite") : QStringLiteral("Once"))
            .arg(quintptr(player), 0, 16)
            .arg(quintptr(player->videoOutput()), 0, 16)
            .arg(player->source().toString()));
}
void VideoWallpaper::setPlayMode(int mode)
{
    const int bounded = qBound(int(SingleLoop), mode, int(Random));
    if (bounded == m_mode)
        return;
    m_mode = bounded;
    // 无缝循环与列表推进的分界由循环策略决定，改模式必须对现役播放器重新断言；
    // 列表从 1 条变成多条(或反过来)时也依赖这里把 setLoops 纠正回来。
    for (const VideoOutput &out : std::as_const(m_outputs))
        if (isLiveOutput(out))
            applyLoopPolicy(out.player);
    // 停在"播放结束"时用户换了模式：三种模式都会继续转，立刻从当前曲目续播，
    // 不能只改策略让桌面继续黑着/定格着(心跳的恢复分支被 m_playbackFinished 拦着)。
    if (m_playbackFinished && m_started && !m_playlist.isEmpty())
        playIndex(m_index >= 0 ? m_index : 0);
}
void VideoWallpaper::setReclaimMemory(bool on)
{
    m_reclaimMemory = on;
}

// VideoWallpaper 核心实现：播放列表管理、播放器生命周期、挂起策略和错误恢复。
#include "VideoWallpaper.h"

#include "app/AppInfo.h"
#include "wallpaper/WebWallpaper.h"
#include "config/AppConfig.h"
#include "core/Diagnostics.h"
#include "core/SuspendPolicy.h"
#include "platform/windows/desktopmount.h"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>
#include <QVideoWidget>
#include <QWindow>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Powrprof.lib")
#endif

// 仅 GUI 线程可调用的断言宏，用于确保后台线程不使用 QWidget/QVideoWidget。
#define VW_ASSERT_GUI() Q_ASSERT(QThread::currentThread() == qApp->thread())

namespace {

VideoWallpaper *g_wallpaper = nullptr;

} // namespace

VideoWallpaper &VideoWallpaper::instance()
{
    static VideoWallpaper v;
    return v;
}
VideoWallpaper::~VideoWallpaper()
{
    // qApp 已销毁时绝不能走清理：QWidget 调用会踩到空 qApp 路径崩溃(c0000005)
    g_wallpaper = nullptr;
    if (!qApp || m_shutdownDone)
        return;
    shutdownNow();
}
void VideoWallpaper::shutdown()
{
    // 单例不存在时什么都不做：绝不能为了清理而构造单例
    if (!qApp || !g_wallpaper)
        return;
    g_wallpaper->shutdownNow();
}
void VideoWallpaper::shutdownNow()
{
    if (m_shutdownDone)
        return;
    m_shutdownDone = true;
    m_shuttingDown = true; // 此后所有信号回调/延迟任务直接短路
    VW_ASSERT_GUI();
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("退出清理开始: outputs=%1 session=%2 uptime=%3ms")
            .arg(m_outputs.size()).arg(m_playbackSessionId)
            .arg(videodiag::elapsedMs()),
        QStringLiteral("Lifecycle"));
    stopHeartbeatTimers();
    // 一次性摘掉未触发的延迟任务，避免清理中再被回调拽回播放路径
    QCoreApplication::removePostedEvents(this, QEvent::Timer);
    stopAll();
    // 探针对象同样必须赶在 qApp 销毁前释放，否则落到 atexit 链上，与上面的崩溃同因。
    if (m_probePlayer) {
        m_probePlayer->stop();
        m_probePlayer->setVideoOutput(nullptr);
    }
    if (m_probeWidget)
        fbswin::unmountWindow(m_probeWidget);
    delete m_probeWidget;
    delete m_probePlayer;
    delete m_probeAudio;
    m_probeWidget = nullptr;
    m_probePlayer = nullptr;
    m_probeAudio = nullptr;
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("退出清理完成: players=%1/%2 widgets=%3/%4 audios=%5/%6")
            .arg(m_playersCreated).arg(m_playersDestroyed)
            .arg(m_widgetsCreated).arg(m_widgetsDestroyed)
            .arg(m_audiosCreated).arg(m_audiosDestroyed),
        QStringLiteral("Lifecycle"));
}
VideoWallpaper::VideoWallpaper(QObject *parent) : QObject(parent)
{
    g_wallpaper = this;
    m_mountFixClock = std::make_unique<QElapsedTimer>();
    m_mountFixClock->start();
    m_suspendClock = std::make_unique<QElapsedTimer>();

    m_fullscreenTimer = new QTimer(this);
    m_fullscreenTimer->setInterval(1000);
    connect(m_fullscreenTimer, &QTimer::timeout, this, &VideoWallpaper::evaluateSuspend);

    m_reclaimTimer = new QTimer(this);
    m_reclaimTimer->setInterval(30000);
    connect(m_reclaimTimer, &QTimer::timeout, this, [this] {
        // 只在"故意空闲"时裁剪：换曲边界 isPlaying() 会短暂为 false，裁剪活跃缓冲会引起卡顿
        const bool deliberateIdle =
            m_outputs.isEmpty() || m_manualPaused || m_suspendReasons != 0;
        if (m_reclaimMemory && deliberateIdle)
            trimMemory();
    });
    // 心跳定时器只创建不启动，仅在 m_started 期间启用，未播放时进程完全静默、无空转唤醒

    // 关闭/重载实验驱动(仅自动化测试)，用于验证"停止→等待→重载"后内存回落并复现同一基线。
    if (const int stopMs = qEnvironmentVariableIntValue("YUMEIREN_AUTO_STOP_MS");
        stopMs > 0)
        QTimer::singleShot(stopMs, this, &VideoWallpaper::stopAll);
    if (const int startMs = qEnvironmentVariableIntValue("YUMEIREN_AUTO_START_MS");
        startMs > 0)
        QTimer::singleShot(startMs, this, [this] { startPlaying(nullptr); });
    // 暂停/恢复循环实验驱动(仅自动化测试)
    if (const int pauseMs = qEnvironmentVariableIntValue("YUMEIREN_AUTO_PAUSE_MS");
        pauseMs > 0) {
        auto *toggle = new QTimer(this);
        toggle->setInterval(pauseMs);
        connect(toggle, &QTimer::timeout, this, [this] {
            videodiag::log(videodiag::Level::Info,
                QStringLiteral("自动切换暂停: 即将 manualPaused=%1 → %2")
                    .arg(isPlaying() ? 1 : 0).arg(isPlaying() ? 0 : 1));
            pauseResume();
        });
        toggle->start();
    }

    // 分辨率/DPI/热插拔变化 → 防抖后重建布局。QScreen 无 devicePixelRatioChanged，DPI 变化会一并触发 geometryChanged
    const auto screens = QGuiApplication::screens();
    for (QScreen *s : screens)
        connect(s, &QScreen::geometryChanged, this, &VideoWallpaper::scheduleRelayout);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &VideoWallpaper::scheduleRelayout);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &VideoWallpaper::scheduleRelayout);

    videodiag::startDiagSampling();
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("VideoWallpaper 初始化完成 uptime=%1ms")
            .arg(videodiag::elapsedMs()),
        QStringLiteral("Startup"));
    videodiag::stage(QStringLiteral("视频壁纸单例初始化完成"));
}
void VideoWallpaper::ensureHeartbeatTimers()
{
    if (!m_fullscreenTimer->isActive()) {
        m_fullscreenTimer->start();
        videodiag::logObjectEvent("timer-start", m_fullscreenTimer,
                                  QStringLiteral("1000ms evaluateSuspend"));
    }
    if (!m_reclaimTimer->isActive()) {
        m_reclaimTimer->start();
        videodiag::logObjectEvent("timer-start", m_reclaimTimer,
                                  QStringLiteral("30000ms trimMemory"));
    }
}
void VideoWallpaper::stopHeartbeatTimers()
{
    if (m_fullscreenTimer && m_fullscreenTimer->isActive()) {
        m_fullscreenTimer->stop();
        videodiag::logObjectEvent("timer-stop", m_fullscreenTimer);
    }
    if (m_reclaimTimer && m_reclaimTimer->isActive()) {
        m_reclaimTimer->stop();
        videodiag::logObjectEvent("timer-stop", m_reclaimTimer);
    }
}
bool VideoWallpaper::isLiveOutput(const VideoOutput &out) const
{
    if (m_shuttingDown || out.player == nullptr)
        return false;
    for (const VideoOutput &cur : m_outputs)
        if (cur.player == out.player)
            return true;
    return false;
}
bool VideoWallpaper::isPrimaryOutput(const VideoOutput &out) const
{
    return !m_outputs.isEmpty() && out.player == m_outputs.first().player;
}
void VideoWallpaper::playIndex(int index, qint64 resumePos)
{
    if (m_playlist.isEmpty())
        return;
    // 与网页壁纸互斥的**唯一收口**：启动、换曲、双击切曲、挂起恢复全部经过
    // 这里，谁起播视频谁就把网页壁纸停掉。对未运行的网页壁纸是空操作。
    WebWallpaper::instance().stop();
    VW_ASSERT_GUI();
    m_started = true;
    ensureHeartbeatTimers();
    ++m_playbackSessionId; // 会话 ID：每次起播/换曲/重试自增，供诊断日志关联
    if (index != m_index)
        m_fileRetries = 0; // 换曲重置；同 index 的重试调用保留计数
    m_index = qBound(0, index, m_playlist.size() - 1);
    m_resumePosMs = resumePos;   // LoadedMedia 时消费；普通换曲传 -1
    // 起播意味着"列表又活了"：播完标志必须失效，否则挂起恢复与管线重建会被上次会话的收口状态锁住
    m_playbackFinished = false;
    m_watchPosMs = -1;
    m_watchStalls = 0;
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("session=%1 play index=%2/%3 file=%4 resumePos=%5")
            .arg(m_playbackSessionId).arg(m_index + 1).arg(m_playlist.size())
            .arg(QFileInfo(m_playlist[m_index]).fileName()).arg(resumePos));
    QString err;
    if (!ensureOutputs(&err)) {
        videodiag::log(videodiag::Level::Warning,
            QStringLiteral("session=%1 获取桌面挂载点失败: %2")
                .arg(m_playbackSessionId).arg(err));
        emit playbackStateChanged(err);
        return;
    }
    const QUrl url = QUrl::fromLocalFile(m_playlist[m_index]);
    // 不重设 source 是可播同一文件时的快路径：避免 FFmpeg 每圈销毁重建解码器(无黑帧)
    bool sameSource = true;
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (out.player->source() != url) {
            sameSource = false;
            break;
        }
    }
    if (!sameSource) {
        // 自动限帧值清零，等新素材元数据到达再判定，否则会继承上一条的上限；同一素材重播须保留(不再发 metaDataChanged)
        m_autoFps = 0;
        resetFramePacing();
        // 媒体打开需要 1-2s，立刻给状态反馈避免"点了没反应"
        emit playbackStateChanged(
            QStringLiteral("第 %1 个 打开中…").arg(m_index + 1));
    }
    bool first = true;
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (!isLiveOutput(out))
            continue;
        if (sameSource)
            out.player->setPosition(0); // 解码器复用，原地重播
        else {
            out.player->setSource(url);
            applyAudioPolicy(out, first);
        }
        // 循环策略必须每次都断言：列表从 1 个变多个时走 sameSource 快路径，漏掉会遗留单曲循环
        applyLoopPolicy(out.player);
        applyPlaybackRate(out.player);
        out.audio->setVolume(first ? qBound(0, m_volume, 100) / 100.0 : 0);
        out.audio->setMuted(!first);
        out.player->play();
        first = false;
    }
    emitTrackState();
}
bool VideoWallpaper::isSeamlessLoop() const
{
    return m_mode == SingleLoop || m_playlist.size() == 1;
}
void VideoWallpaper::restartSingleLoop()
{
    if (m_shuttingDown || !m_started || m_outputs.isEmpty())
        return;
    videodiag::log(videodiag::Level::Debug,
        QStringLiteral("session=%1 单视频回绕 player=0x%2")
            .arg(m_playbackSessionId)
            .arg(quintptr(m_outputs.first().player), 0, 16));
    m_watchPosMs = -1;
    m_watchStalls = 0;
    resetFramePacing(); // 回绕后第一帧必须立即呈现，不沿用旧节拍
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (!isLiveOutput(out))
            continue;
        out.player->setPosition(0); // 解码器复用，原地重播
        out.player->play();
    }
}
bool VideoWallpaper::atMediaEnd(const QMediaPlayer *player) const
{
    if (!player)
        return false;
    if (player->mediaStatus() == QMediaPlayer::EndOfMedia)
        return true;
    const qint64 dur = player->duration();
    return dur > 0 && player->position() >= dur - 250;
}
void VideoWallpaper::nextTrack()
{
    if (m_shuttingDown)
        return; // 清理中不再推进列表
    if (m_playlist.isEmpty())
        return;
    const int size = m_playlist.size();
    // 失败名单里的曲目不再参与轮换，避免坏素材每圈触发解码器重建
    const auto alive = [&](int i) { return !m_deadTracks.contains(i); };
    if (m_deadTracks.size() >= size) {
        emit playbackStateChanged(QStringLiteral("所有视频都无法播放，已停止"));
        stopAll();
        return;
    }
    if (isSeamlessLoop()) {
        restartSingleLoop();
        return;
    }
    // 随机：从"非当前曲且可播"的候选里等概率挑一个，避免 do-while 在当前曲是唯一幸存者时死循环。
    if (m_mode == Random && size > 1) {
        QVector<int> candidates;
        candidates.reserve(size - 1);
        for (int i = 0; i < size; ++i)
            if (i != m_index && alive(i))
                candidates.append(i);
        if (!candidates.isEmpty()) {
            playIndex(candidates.at(QRandomGenerator::global()->bounded(candidates.size())));
            return;
        }
        restartSingleLoop(); // 只剩当前一条可播：原地重播，不切源
        return;
    }
    // 列表循环：向后找下一条可播曲目，跨过末尾绕回表头；步数上限取 size 覆盖整个列表
    int next = -1;

    for (int step = 1; step <= size; ++step) {
        const int i = (m_index + step) % size;
        if (alive(i)) {
            next = i;
            break;
        }
    }
    if (next < 0) {
        finishPlaylist(); // 整表没有可播曲目
        return;
    }
    playIndex(next);
}
void VideoWallpaper::finishPlaylist()
{
    if (m_shuttingDown)
        return;
    emit playbackStateChanged(QStringLiteral("播放结束"));
    m_playbackFinished = true; // 心跳不再自动重新点火
    m_watchPosMs = -1;
    m_watchStalls = 0;
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("session=%1 列表播完收口 index=%2/%3 mode=%4")
            .arg(m_playbackSessionId).arg(m_index + 1).arg(m_playlist.size())
            .arg(m_mode));
}
void VideoWallpaper::switchToTrack(int index)
{
    VW_ASSERT_GUI();
    if (!m_started || m_shuttingDown || index < 0 || index >= m_playlist.size()
        || index == m_index)
        return;
    m_trackFails.remove(index);
    m_deadTracks.remove(index);
    m_fileRetries = 0;
    playIndex(index);
    if (m_manualPaused || m_suspendReasons != 0) {
        for (const VideoOutput &out : std::as_const(m_outputs))
            if (isLiveOutput(out))
                out.player->pause();
    }
}
void VideoWallpaper::advanceOnError()
{
    if (!m_started || m_shuttingDown || m_outputs.isEmpty() || m_playlist.isEmpty())
        return;
    nextTrack();
    if (m_manualPaused || m_suspendReasons != 0) {
        for (const VideoOutput &out : std::as_const(m_outputs))
            if (isLiveOutput(out))
                out.player->pause();
    }
}
void VideoWallpaper::handleUnplayable(const QString &reason)
{
    if (m_shuttingDown)
        return;
    m_fileRetries = 0;
    const int fails = ++m_trackFails[m_index];
    m_deadTracks.insert(m_index);
    videodiag::log(videodiag::Level::Warning,
        QStringLiteral("session=%1 曲目不可播 track=%2 fails=%3 dead=%4 reason=%5")
            .arg(m_playbackSessionId).arg(m_index + 1).arg(fails)
            .arg(m_deadTracks.size()).arg(reason));
    if (m_deadTracks.size() >= m_playlist.size()) {
        emit playbackStateChanged(
            QStringLiteral("所有视频都无法播放（%1），已停止").arg(reason));
        stopAll();
        return;
    }
    const QString text =
        QStringLiteral("第 %1 个无法播放（%2），已跳过").arg(m_index + 1).arg(reason);
    if (text != m_lastErrorText) {
        m_lastErrorText = text;
        emit playbackStateChanged(text);
    }
    QTimer::singleShot(200, this, &VideoWallpaper::advanceOnError);
}
bool VideoWallpaper::startPlaying(QString *error, int preferIndex)
{
    VW_ASSERT_GUI();
    if (m_shuttingDown) {
        if (error)
            *error = QStringLiteral("正在退出，无法启动壁纸");
        return false;
    }
    if (m_playlist.isEmpty()) {
        if (error) *error = QStringLiteral("播放列表为空，请先添加视频");
        return false;
    }
    m_manualPaused = false; // 用户点击"启动"即视为要求播放
    m_started = true;
    m_playbackFinished = false;
    ensureHeartbeatTimers();
    m_trackFails.clear(); // 新会话：失败名单清空，所有曲目重新获得机会
    m_deadTracks.clear();
    m_fileRetries = 0;
    // 列表中选中了条目时从选中项开始(用户点名的曲目优先于上次进度)
    const bool preferValid = preferIndex >= 0 && preferIndex < m_playlist.size();
    if (m_outputs.isEmpty()) {
        playIndex(preferValid ? preferIndex : (m_index >= 0 ? m_index : 0));
    } else if (preferValid && preferIndex != m_index) {
        playIndex(preferIndex); // 运行中点了启动且选中了其他曲目：直接切换
    } else {
        evaluateSuspend();
    }
    return true;
}
void VideoWallpaper::pauseResume()
{
    VW_ASSERT_GUI();
    if (m_outputs.isEmpty()) {
        // 长挂起已释放管线：重建并按当前状态继续(维持暂停由挂起原因决定)
        if (m_started && !m_playlist.isEmpty()) {
            m_manualPaused = false;
            m_playbackFinished = false;
            evaluateSuspend();
        }
        return;
    }
    m_manualPaused = isPlaying(); // 正在播 → 用户要暂停；已暂停 → 用户要继续
    // 用户手点的暂停/继续优先于"播完待命"；播完后点继续须显式回表头重新起播，对末尾播放器直接 play() 只会立刻再结束
    const bool restartLap = m_playbackFinished && !m_manualPaused;
    m_playbackFinished = false;
    if (restartLap && !m_playlist.isEmpty())
        playIndex(0);
    evaluateSuspend();
    // 被挂起原因拦下时 evaluateSuspend 已发出原因文本，不能再用"播放中"盖掉
    if (isPlaying())
        emitTrackState();
    else if (m_manualPaused)
        emit playbackStateChanged(QStringLiteral("已暂停"));
}
void VideoWallpaper::stopAll()
{
    VW_ASSERT_GUI();
    stopHeartbeatTimers();
    for (const VideoOutput &out : std::as_const(m_outputs))
        if (out.player)
            out.player->stop();
    teardownOutputs();
    m_index = -1;
    m_started = false;
    m_manualPaused = false;
    m_playbackFinished = false;
    m_suspendReasons = 0;
    m_trackFails.clear();
    m_deadTracks.clear();
    m_fileRetries = 0;
    m_lastErrorText.clear();
    m_resumePosMs = -1;
    if (m_reclaimMemory)
        trimMemory(); // 停止后立即还给系统，不等下一个回收周期
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("stopAll: 管线已卸载 session=%1").arg(m_playbackSessionId));
    // 退出清理中窗口正在销毁，不再广播状态变化
    if (!m_shuttingDown)
        emit playbackStateChanged(QStringLiteral("已停止"));
}
bool VideoWallpaper::isPlaying() const
{
    if (m_shuttingDown)
        return false;
    for (const VideoOutput &out : m_outputs)
        if (out.player && out.player->playbackState() == QMediaPlayer::PlayingState)
            return true;
    return false;
}
void VideoWallpaper::evaluateSuspend()
{
    if (m_shuttingDown)
        return;
    if (!m_started) {
        m_suspendReasons = 0;
        return;
    }

    int reasons = 0;
    // 判据与"谁在前台"无关：全屏应用前面压着小窗口时桌面依然不可见，只看前台会误判成已回桌面
    if (m_pauseOnFullscreen && fbswin::isFullscreenWindowPresent())
        reasons |= SuspendFullscreen;
    // 遮挡判定只在主屏铺放时安全(多输出时曾实测每秒反复暂停/恢复)，多屏档位已删、
    // 现在恒为单输出主屏铺放，故无条件启用。
    if (m_pauseOnFullscreen && fbswin::isDesktopCoveredByWindow())
        reasons |= SuspendCovered;
    if (fbswin::isWorkstationLocked())
        reasons |= SuspendLocked;
    if (!m_monitorOn)
        reasons |= SuspendMonitorOff;
    if (m_pauseOnBattery && fbswin::isOnBattery())
        reasons |= SuspendBattery;
    m_suspendReasons = reasons;
    // 挂载健康检查复用同一条 1s 心跳：Progman 兜底在部分 Win11 不被 DWM 合成，故兜底状态由 scheduleMountFix 持续重查
    if (mountIsStale() || !fbswin::hasRealWorker())
        scheduleMountFix();

    const bool shouldPlay = !m_manualPaused && reasons == 0;
    const bool wasPlaying = isPlaying();

    // 长挂起期间管线已被释放，现在应当恢复：重建管线并跳回暂停时的进度
    if (shouldPlay && !m_playbackFinished && m_outputs.isEmpty()
        && !m_playlist.isEmpty()) {
        // 节流：挂载点缺失时每 5s 重试，不空转
        if (!m_suspendClock->isValid() || m_suspendClock->elapsed() >= 5000) {
            m_suspendClock->restart();
            playIndex(qMax(0, m_index), m_resumePosMs);
        }
        m_lastEmittedReasons = 0;
        return;
    }

    // 只有"被挂起/被手动暂停后才解除"才需要心跳捞回来：列表正常播完(不循环)时播放器停在末尾，照抄恢复逻辑会每秒看到 !isPlaying，几秒后把整个列表从头重新点火
    if (shouldPlay && !wasPlaying && !m_playbackFinished) {
        videodiag::log(videodiag::Level::Info,
            QStringLiteral("恢复播放: reasons=0 manualPaused=%1").arg(m_manualPaused));
        // 无缝循环兜底：后端没遵守 setLoops 时会停在末尾，先回绕再播
        const bool rewindTail = isSeamlessLoop();
        for (const VideoOutput &out : std::as_const(m_outputs))
            if (isLiveOutput(out)) {
                if (rewindTail && atMediaEnd(out.player))
                    out.player->setPosition(0);
                out.player->play();
            }
        emitTrackState();
        m_lastEmittedReasons = 0;
        return;
    }
    // 无缝循环看门狗：进度连续 3 拍(约 3s)不前进 = 后端既没回绕也没上报 EndOfMedia → 原地回绕重播
    if (shouldPlay && wasPlaying && isSeamlessLoop() && !m_outputs.isEmpty()
        && isLiveOutput(m_outputs.first())
        // 素材还在探测/缓冲时进度本就不动，别误判成卡死
        && (m_outputs.first().player->mediaStatus() == QMediaPlayer::BufferedMedia
            || m_outputs.first().player->mediaStatus() == QMediaPlayer::EndOfMedia)) {
        const qint64 pos = m_outputs.first().player->position();
        if (pos > m_watchPosMs) {
            m_watchPosMs = pos;
            m_watchStalls = 0;
        } else if (pos < m_watchPosMs) {
            m_watchPosMs = pos;
            m_watchStalls = 0;
        } else if (++m_watchStalls >= 3) {
            videodiag::log(videodiag::Level::Warning,
                QStringLiteral("session=%1 单视频循环停滞在 %2ms，看门狗回绕")
                    .arg(m_playbackSessionId).arg(pos));
            restartSingleLoop();
        }
    } else {
        m_watchPosMs = -1;
        m_watchStalls = 0;
    }
    if (!shouldPlay && wasPlaying) {
        for (const VideoOutput &out : std::as_const(m_outputs))
            if (isLiveOutput(out))
                out.player->pause();
        m_suspendClock->restart(); // 挂起计时开始
        if (m_reclaimMemory) {
            // 暂停后解码器队列逐渐排空，稍等片刻再还工作集
            QTimer::singleShot(2000, this, [this] {
                if (!m_shuttingDown && !isPlaying()
                    && (m_manualPaused || m_suspendReasons != 0))
                    trimMemory();
            });
        }
    }
    // 持续挂起超过该原因的阈值即整条释放(见 kSuspendRelease*Ms)。例外：列表已播完(不循环)时不释放——播完状态会拦住心跳的重建分支，卸载后壁纸再也回不来
    if (!shouldPlay && !wasPlaying && !m_playbackFinished && !m_outputs.isEmpty()
        && m_suspendClock->isValid()
        && m_suspendClock->elapsed() >= suspendReleaseThresholdMs(m_suspendReasons))
        longSuspendRelease();
    if (!shouldPlay) {
        if (reasons != m_lastEmittedReasons) {
            if (reasons & SuspendCovered)
                emit playbackStateChanged(QStringLiteral("桌面被完全遮挡，已自动暂停"));
            else if (reasons & SuspendFullscreen)
                emit playbackStateChanged(QStringLiteral("检测到全屏应用，已自动暂停"));
            else if (reasons & SuspendLocked)
                emit playbackStateChanged(QStringLiteral("系统已锁定，已自动暂停"));
            else if (reasons & SuspendMonitorOff)
                emit playbackStateChanged(QStringLiteral("显示器已关闭，已自动暂停"));
            else if (reasons & SuspendBattery)
                emit playbackStateChanged(QStringLiteral("电池模式，已自动暂停"));
            videodiag::log(videodiag::Level::Info,
                QStringLiteral("自动挂起: reasons=0x%1").arg(reasons, 0, 16));
        }
    }
    m_lastEmittedReasons = reasons;

    // 循环边界取证：复用同一条 1s 心跳记录进度与对象地址，不新增定时器
    if (!m_outputs.isEmpty() && isLiveOutput(m_outputs.first())) {
        const VideoOutput &out = m_outputs.first();
        videodiag::log(videodiag::Level::Debug,
            QStringLiteral("snapshot session=%1 media=%2 state=%3 pos=%4/%5 "
                           "player=0x%6 widget=0x%7 hwnd=0x%8 finished=%9")
                .arg(m_playbackSessionId).arg(int(out.player->mediaStatus()))
                .arg(int(out.player->playbackState()))
                .arg(out.player->position()).arg(out.player->duration())
                .arg(quintptr(out.player), 0, 16)
                .arg(quintptr(out.widget), 0, 16)
                .arg(quintptr(out.widget->winId()), 0, 16)
                .arg(m_playbackFinished ? 1 : 0));
    }
}
void VideoWallpaper::trimMemory()
{
    fbswin::trimProcessMemory();
}
void VideoWallpaper::emitTrackState()
{
    if (m_index >= 0)
        emit playbackStateChanged(QStringLiteral("第 %1 个 播放中").arg(m_index + 1));
    else
        emit playbackStateChanged(QStringLiteral("播放中"));
}
qint64 VideoWallpaper::suspendReleaseThresholdMs(int reasons) const
{
    // 分级阈值见 core/SuspendPolicy.h；环境变量供自动化测试压低全部档位。
    const auto graded = [&](qint64 ms) {
        return suspendpolicy::thresholdWithEnvOverride("YUMEIREN_LONG_SUSPEND_MS", ms);
    };
    // 锁屏/熄屏优先：画面根本不存在，且恢复必然伴随人工动作
    if (reasons & (SuspendLocked | SuspendMonitorOff))
        return graded(suspendpolicy::kHiddenMs);
    if (reasons & SuspendBattery)
        return graded(suspendpolicy::kBatteryMs);
    if (reasons & (SuspendFullscreen | SuspendCovered))
        return graded(suspendpolicy::kCoveredMs);
    return graded(suspendpolicy::kDefaultMs);
}
void VideoWallpaper::longSuspendRelease()
{
    if (m_shuttingDown)
        return;
    qint64 pos = -1;
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (out.player) {
            const qint64 p = out.player->position();
            if (p > 0) {
                pos = p;
                break;
            }
        }
    }
    m_resumePosMs = pos;
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("长挂起释放管线: resumePos=%1 reasons=0x%2 阈值=%3ms")
            .arg(pos).arg(m_suspendReasons, 0, 16)
            .arg(suspendReleaseThresholdMs(m_suspendReasons)));
    teardownOutputs();
    if (m_reclaimMemory)
        trimMemory(); // 立刻把释放后的页还给系统
    emit playbackStateChanged(
        QStringLiteral("暂停较久，已释放壁纸资源；回到桌面自动恢复"));
}
void VideoWallpaper::setAutostart(bool on)
{
    appinfo::setAutostart(on);
}
bool VideoWallpaper::autostartEnabled() const
{
    return appinfo::autostartEnabled();
}

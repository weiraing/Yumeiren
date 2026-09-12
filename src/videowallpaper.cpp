#include "videowallpaper.h"

#include "appinfo.h"
#include "platform/windows/desktopmount.h"

#include <QAudioOutput>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QRandomGenerator>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVideoWidget>

VideoWallpaper &VideoWallpaper::instance()
{
    static VideoWallpaper v;
    return v;
}

VideoWallpaper::~VideoWallpaper()
{
    // 退出前停播并卸载挂载窗口，避免残留一帧冻结的壁纸窗口
    stopAll();
}

// 资源所有权说明：VideoOutput 中的 widget/player/audio 均由 m_outputs 独占持有，
// 统一通过 teardownOutputs() 的 deleteLater 销毁。这里必须用异步删除——teardown
// 可能被播放器的信号链(EndOfMedia → nextTrack)间接触发，同步 delete 会析构正在
// 发信号的 sender 造成 use-after-free。WorkerW 挂载与系统探测在
// platform/windows/desktopmount.cpp，本类只保留播放控制与状态机。

VideoWallpaper::VideoWallpaper(QObject *parent) : QObject(parent)
{
    m_mountFixClock = new QElapsedTimer();
    m_mountFixClock->start();

    m_fullscreenTimer = new QTimer(this);
    m_fullscreenTimer->setInterval(1000);
    connect(m_fullscreenTimer, &QTimer::timeout, this, &VideoWallpaper::evaluateSuspend);
    m_fullscreenTimer->start();

    m_reclaimTimer = new QTimer(this);
    m_reclaimTimer->setInterval(30000);
    connect(m_reclaimTimer, &QTimer::timeout, this, [this] {
        // 仅在“故意空闲”(已完全停止/手动暂停/被挂起原因暂停)时裁剪工作集。
        // 播放中曲目切换的 EndOfMedia 边界 isPlaying() 会短暂为 false，此时
        // 裁剪会把活跃的视频缓冲换出，引起卡顿与页面错误。
        const bool deliberateIdle =
            m_outputs.isEmpty() || m_manualPaused || m_suspendReasons != 0;
        if (m_reclaimMemory && deliberateIdle)
            trimMemory();
    });
    m_reclaimTimer->start();

    // 分辨率/DPI/显示器热插拔变化 → 防抖后重建布局(仅播放中有效)。
    // QScreen 没有 devicePixelRatioChanged 信号；DPI 变化会同时触发 geometryChanged。
    const auto screens = QGuiApplication::screens();
    for (QScreen *s : screens)
        connect(s, &QScreen::geometryChanged, this, &VideoWallpaper::scheduleRelayout);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &VideoWallpaper::scheduleRelayout);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &VideoWallpaper::scheduleRelayout);
}

void VideoWallpaper::setPlaylist(const QStringList &files)
{
    const QString current = (m_index >= 0 && m_index < m_playlist.size())
                                ? m_playlist.at(m_index)
                                : QString();
    m_playlist = files;
    // 播放中增删曲目时按文件名保持当前曲目指针，避免状态退化为“第 0 个”
    m_index = current.isEmpty() ? -1 : m_playlist.indexOf(current);
}

void VideoWallpaper::clearPlaylist()
{
    stopAll();
    m_playlist.clear();
    m_index = -1;
    emit playbackStateChanged(QStringLiteral("已清空播放列表"));
}

void VideoWallpaper::layoutOutputs()
{
    teardownOutputs();
    if (!fbswin::ensureWorker())
        return;

    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty())
        return;

    auto makeOutput = [this](const QRect &g, bool withAudio) {
        VideoOutput out;
        out.logicalRect = g;
        out.widget = new QVideoWidget;
        out.widget->setAspectRatioMode(Qt::IgnoreAspectRatio);
        out.widget->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool
                                   | Qt::WindowTransparentForInput);
        out.player = new QMediaPlayer(this);
        out.audio = new QAudioOutput(this);
        out.audio->setMuted(true);
        out.player->setAudioOutput(out.audio);
        out.player->setVideoOutput(out.widget);

        // Track end: advance in the playlist or stop (keeping the last frame).
        connect(out.player, &QMediaPlayer::mediaStatusChanged, this,
                [this](QMediaPlayer::MediaStatus st) {
            if (st == QMediaPlayer::EndOfMedia)
                nextTrack();
        });
        // Keep the pause/resume label in sync once playback really starts, and
        // count a successful start as proof the current file is playable.
        connect(out.player, &QMediaPlayer::playbackStateChanged, this,
                [this](QMediaPlayer::PlaybackState st) {
            if (st == QMediaPlayer::PlayingState) {
                m_errorStreak = 0;
                m_lastErrorText.clear();
                emitTrackState();
            }
        });
        // 元数据就绪后应用帧率上限(此时才知道视频原生帧率)
        connect(out.player, &QMediaPlayer::metaDataChanged, this,
                [this, out] { applyPlaybackRate(out.player); });
        // 解码/打开失败 → 提示并自动跳过；连续失败铺满列表即整体停播。
        // 只有首个输出参与推进(MirrorAll 的副本播放器会对同一文件重复报错)。
        connect(out.player, &QMediaPlayer::errorOccurred, this,
                [this, out](QMediaPlayer::Error, const QString &msg) {
            if (!m_started || m_outputs.isEmpty()
                || out.player != m_outputs.first().player)
                return;
            ++m_errorStreak;
            if (m_errorStreak >= m_playlist.size()) {
                emit playbackStateChanged(
                    QStringLiteral("所有视频都无法播放（%1），已停止").arg(msg));
                stopAll();
                return;
            }
            const QString text =
                QStringLiteral("第 %1 个无法播放（%2），自动跳过").arg(m_index + 1).arg(msg);
            if (text != m_lastErrorText) {
                m_lastErrorText = text;
                emit playbackStateChanged(text);
            }
            QTimer::singleShot(200, this, &VideoWallpaper::advanceOnError);
        });

        out.widget->setGeometry(g);
        out.widget->show();
        fbswin::mountBehindIcons(out.widget, g);
        m_outputs.append(out);
    };

    if (m_screenMode == PrimaryScreen) {
        makeOutput(QGuiApplication::primaryScreen()->geometry(), true);
    } else if (m_screenMode == StretchAll) {
        QRect total;
        for (QScreen *s : screens)
            total = total.united(s->geometry());
        makeOutput(total, true);
    } else { // MirrorAll: one player per screen, first carries the audio
        for (int i = 0; i < screens.size(); ++i)
            makeOutput(screens[i]->geometry(), i == 0);
    }
}

void VideoWallpaper::remountOutputs()
{
    // 轻量重挂载：只修正父窗口与位置，不重建解码管线(避免换曲时资源反复销毁)。
    // 位置与尺寸都比对物理像素，纠正历史遗留的错误坐标。
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        const qreal dpr = out.widget->devicePixelRatioF();
        const QRect phys(int(out.logicalRect.x() * dpr), int(out.logicalRect.y() * dpr),
                         int(out.logicalRect.width() * dpr),
                         int(out.logicalRect.height() * dpr));
        if (fbswin::isWindowMounted(out.widget, phys))
            continue; // 已在正确位置
        fbswin::mountBehindIcons(out.widget, out.logicalRect);
    }
}

// Explorer 重启会连带销毁挂载在 WorkerW 下的壁纸窗口；探测到失联后由
// scheduleMountFix 重新查找 WorkerW 并重挂载(节流 10s，避免 shell 恢复期空转)。
bool VideoWallpaper::mountIsStale() const
{
    if (m_outputs.isEmpty())
        return false;
    QVideoWidget *w = m_outputs.first().widget;
    if (!w)
        return false;
    const qreal dpr = w->devicePixelRatioF();
    const VideoOutput &out = m_outputs.first();
    const QRect phys(int(out.logicalRect.x() * dpr), int(out.logicalRect.y() * dpr),
                     int(out.logicalRect.width() * dpr),
                     int(out.logicalRect.height() * dpr));
    return !fbswin::isWindowMounted(w, phys);
}

void VideoWallpaper::scheduleMountFix()
{
    if (m_mountFixClock->isValid() && m_mountFixClock->elapsed() < 10000)
        return;
    m_mountFixClock->restart();
    if (!fbswin::ensureWorker())
        return;
    remountOutputs();
}

void VideoWallpaper::scheduleRelayout()
{
    if (!m_started || m_outputs.isEmpty() || m_relayoutPending)
        return;
    m_relayoutPending = true;
    // DPI/几何变化会连发多个信号，防抖合并成一次重建
    QTimer::singleShot(600, this, [this] {
        m_relayoutPending = false;
        if (!m_started || m_outputs.isEmpty())
            return;
        layoutOutputs();
        if (!m_playlist.isEmpty())
            playIndex(qMax(0, m_index));
        evaluateSuspend();
    });
}

void VideoWallpaper::teardownOutputs()
{
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (out.player)
            out.player->stop();
        if (out.widget) {
            fbswin::unmountWindow(out.widget);
            out.widget->hide();
            out.widget->deleteLater();
        }
        if (out.player)
            out.player->deleteLater();
        if (out.audio)
            out.audio->deleteLater();
    }
    m_outputs.clear();
}

bool VideoWallpaper::ensureOutputs(QString *error)
{
    if (!m_outputs.isEmpty()) {
        remountOutputs(); // 轻量重挂载保持 z 序；解码管线复用，切换曲目零重建
        return true;
    }
    layoutOutputs();
    if (m_outputs.isEmpty()) {
        if (error) *error = QStringLiteral("未能获取 WorkerW 桌面挂载点");
        return false;
    }
    return true;
}

void VideoWallpaper::playIndex(int index)
{
    if (m_playlist.isEmpty())
        return;
    m_started = true;
    m_index = qBound(0, index, m_playlist.size() - 1);
    QString err;
    if (!ensureOutputs(&err)) {
        emit playbackStateChanged(err);
        return;
    }
    const QUrl url = QUrl::fromLocalFile(m_playlist[m_index]);
    // 同一文件(单视频循环最常见)：不重设 source，避免 FFmpeg 每圈销毁重建解码器；
    // 从头继续播即可，零资源churn、无黑帧。
    bool sameSource = true;
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (out.player->source() != url) {
            sameSource = false;
            break;
        }
    }
    bool first = true;
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (sameSource)
            out.player->setPosition(0); // 解码器复用，原地重播
        else
            out.player->setSource(url);
        applyPlaybackRate(out.player);
        out.audio->setVolume(first ? qBound(0, m_volume, 100) / 100.0 : 0);
        out.audio->setMuted(!first);
        out.player->play();
        first = false;
    }
    emitTrackState();
}

void VideoWallpaper::nextTrack()
{
    if (m_playlist.isEmpty())
        return;
    int next = m_index + 1;
    if (m_random && m_playlist.size() > 1) {
        do { next = QRandomGenerator::global()->bounded(m_playlist.size()); }
        while (next == m_index);
    } else if (next >= m_playlist.size()) {
        if (!m_autoLoop) {
            emit playbackStateChanged(QStringLiteral("播放结束"));
            return;
        }
        next = 0;
    }
    playIndex(next);
}

// 解码失败自动跳转的收口：挂起/手动暂停期间只换源不强行播放
void VideoWallpaper::advanceOnError()
{
    if (!m_started || m_outputs.isEmpty() || m_playlist.isEmpty())
        return;
    nextTrack();
    if (m_manualPaused || m_suspendReasons != 0) {
        for (const VideoOutput &out : std::as_const(m_outputs))
            if (out.player)
                out.player->pause();
    }
}

bool VideoWallpaper::startPlaying(QString *error)
{
    if (m_playlist.isEmpty()) {
        if (error) *error = QStringLiteral("播放列表为空，请先添加视频");
        return false;
    }
    m_manualPaused = false; // 用户点击“启动”即视为要求播放
    m_started = true;
    if (m_outputs.isEmpty())
        playIndex(m_index >= 0 ? m_index : 0);
    else
        evaluateSuspend(); // 可能处于挂起原因中，由状态机决定播/停
    return true;
}

void VideoWallpaper::pauseResume()
{
    if (m_outputs.isEmpty())
        return;
    m_manualPaused = isPlaying(); // 正在播 → 用户要暂停；已暂停(含自动挂起) → 用户要继续
    evaluateSuspend();
    // 状态文本必须反映真实结果：手动暂停发“已暂停”(按钮文字靠这条信号翻转)；
    // 请求被挂起原因拦下时 evaluateSuspend 已发出原因文本，不能再用
    // “播放中”把它盖掉(否则画面停着、状态栏却显示播放中且不会自愈)。
    if (isPlaying())
        emitTrackState();
    else if (m_manualPaused)
        emit playbackStateChanged(QStringLiteral("已暂停"));
}

void VideoWallpaper::stopAll()
{
    for (const VideoOutput &out : std::as_const(m_outputs))
        if (out.player)
            out.player->stop();
    teardownOutputs();
    m_index = -1;
    m_started = false;
    m_manualPaused = false;
    m_suspendReasons = 0;
    m_errorStreak = 0;
    m_lastErrorText.clear();
    if (m_reclaimMemory)
        trimMemory(); // 停止后立即把解码器释放后的内存还给系统，不等下一个回收周期
    emit playbackStateChanged(QStringLiteral("已停止"));
}

bool VideoWallpaper::isPlaying() const
{
    for (const VideoOutput &out : m_outputs)
        if (out.player && out.player->playbackState() == QMediaPlayer::PlayingState)
            return true;
    return false;
}

void VideoWallpaper::setScreenMode(int mode)
{
    m_screenMode = qBound(0, mode, 2);
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
        applyPlaybackRate(out.player);
}

// 帧率上限实现说明：QMediaPlayer 没有呈现帧率 API，这里用 playbackRate 实现
// “上限”语义——仅当视频原生帧率高于上限时按比例放慢(每个画面停留更久)，
// 视觉为慢动作；视频帧率低于上限时保持原速。解码仍逐帧进行，因此此项
// 主要影响节奏与观感，解码 CPU 不随上限变化。
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
    player->setPlaybackRate(rate);
}

void VideoWallpaper::setVolume(int percent)
{
    m_volume = qBound(0, percent, 100);
    bool first = true;
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        out.audio->setMuted(!first);
        out.audio->setVolume(m_volume / 100.0);
        first = false;
    }
}

// FrameScheduler 状态机：汇总全部挂起原因(全屏/锁屏/显示器关闭/电池)，
// 任一原因存在即暂停解码与呈现，全部消失且用户未手动暂停则自动续播。
// 不渲染的瞬间 CPU/GPU 占用趋近于零，恢复时解码器原地续用，无重建开销。
// 同时承担壁纸窗口健康检查(Explorer 重启恢复)。
void VideoWallpaper::evaluateSuspend()
{
    if (!m_started) {
        m_suspendReasons = 0;
        return;
    }

    int reasons = 0;
    if (m_pauseOnFullscreen && fbswin::isForegroundFullscreen())
        reasons |= SuspendFullscreen;
    if (fbswin::isWorkstationLocked())
        reasons |= SuspendLocked;
    if (!m_monitorOn)
        reasons |= SuspendMonitorOff;
    if (m_pauseOnBattery && fbswin::isOnBattery())
        reasons |= SuspendBattery;
    m_suspendReasons = reasons;

    // 挂载健康检查放在同一条 1s 心跳里，开销为几次窗口句柄查询
    if (mountIsStale())
        scheduleMountFix();

    const bool shouldPlay = !m_manualPaused && reasons == 0;
    const bool wasPlaying = isPlaying();
    if (shouldPlay && !wasPlaying) {
        for (const VideoOutput &out : std::as_const(m_outputs))
            out.player->play();
        emitTrackState();
        m_lastEmittedReasons = 0;
        return;
    }
    if (!shouldPlay && wasPlaying) {
        for (const VideoOutput &out : std::as_const(m_outputs))
            out.player->pause();
        if (m_reclaimMemory) {
            // 暂停后解码器队列逐渐排空，稍等片刻再把工作集还给系统
            QTimer::singleShot(2000, this, [this] {
                if (!isPlaying() && (m_manualPaused || m_suspendReasons != 0))
                    trimMemory();
            });
        }
    }
    if (!shouldPlay) {
        if (reasons != m_lastEmittedReasons) {
            if (reasons & SuspendFullscreen)
                emit playbackStateChanged(QStringLiteral("检测到全屏应用，已自动暂停"));
            else if (reasons & SuspendLocked)
                emit playbackStateChanged(QStringLiteral("系统已锁定，已自动暂停"));
            else if (reasons & SuspendMonitorOff)
                emit playbackStateChanged(QStringLiteral("显示器已关闭，已自动暂停"));
            else if (reasons & SuspendBattery)
                emit playbackStateChanged(QStringLiteral("电池模式，已自动暂停"));
        }
    }
    m_lastEmittedReasons = reasons;
}

void VideoWallpaper::setReclaimMemory(bool on)
{
    m_reclaimMemory = on;
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

void VideoWallpaper::setAutostart(bool on)
{
    appinfo::setAutostart(on);
}

bool VideoWallpaper::autostartEnabled() const
{
    return appinfo::autostartEnabled();
}

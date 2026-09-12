#include "videowallpaper.h"

#include "appinfo.h"

#include <QAudioOutput>
#include <QGuiApplication>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QRandomGenerator>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVideoWidget>

#include <windows.h>
#include <psapi.h>

// 桌面 hosts icons inside SHELLDLL_DefView on a WorkerW window. After
// sending 0x052C to Progman, an extra WorkerW is spawned BEHIND that one; our
// windows are parented to it, so video renders behind the icons but above the
// plain wallpaper.
namespace {

HWND g_workerW = nullptr;

// FrameScheduler 状态检测(均为一次性系统调用，1s 轮询开销可忽略)
bool isForegroundFullscreen()
{
    const HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    RECT r;
    if (!GetWindowRect(fg, &r))
        return false;
    const QRect wr(r.left, r.top, r.right - r.left + 1, r.bottom - r.top + 1);
    for (QScreen *s : QGuiApplication::screens())
        if (wr == s->geometry())
            return true;
    return false;
}

bool isWorkstationLocked()
{
    // 锁屏时输入桌面切换到 Winlogon，OpenInputDesktop 会失败
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!desk)
        return true;
    CloseDesktop(desk);
    return false;
}

bool isOnBattery()
{
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s))
        return false;
    if (s.BatteryFlag & 128)
        return false; // 无电池(台式机)
    return s.ACLineStatus == 0;
}

HWND findWorkerW()
{
    HWND worker = nullptr;
    EnumWindows([](HWND top, LPARAM lp) -> BOOL {
        HWND defView = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defView) {
            *reinterpret_cast<HWND *>(lp) =
                FindWindowExW(nullptr, top, L"WorkerW", nullptr);
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&worker));
    return worker;
}

bool ensureWorker()
{
    // explorer 重启后旧 WorkerW 句柄失效，需要重新查找
    if (g_workerW && IsWindow(g_workerW))
        return true;
    g_workerW = nullptr;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman)
        return false;
    // The shell spawns the WorkerW asynchronously - poll for it.
    for (int attempt = 0; attempt < 30 && !g_workerW; ++attempt) {
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
        g_workerW = findWorkerW();
        if (!g_workerW)
            Sleep(100);
    }
    if (!g_workerW)
        g_workerW = progman; // fallback: still renders behind the icons
    return true;
}

void mountBehindIcons(QWidget *window)
{
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    // 挂到 WorkerW 后坐标为物理像素，需按设备像素比换算(忽略会导致高 DPI 下不满屏)
    const qreal dpr = window->devicePixelRatioF();
    SetParent(hwnd, g_workerW);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0,
                 int(window->width() * dpr), int(window->height() * dpr),
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

} // namespace

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
// 发信号的 sender 造成 use-after-free。

VideoWallpaper::VideoWallpaper(QObject *parent) : QObject(parent)
{
    m_fullscreenTimer = new QTimer(this);
    m_fullscreenTimer->setInterval(1000);
    connect(m_fullscreenTimer, &QTimer::timeout, this, &VideoWallpaper::evaluateSuspend);
    m_fullscreenTimer->start();

    m_reclaimTimer = new QTimer(this);
    m_reclaimTimer->setInterval(30000);
    connect(m_reclaimTimer, &QTimer::timeout, this, [this] {
        // 播放中裁剪工作集会把活跃的视频缓冲换出，引起卡顿与页面错误；
        // 只在暂停/停止的空闲期回收，内存收益相同且零播放开销。
        if (m_reclaimMemory && !isPlaying())
            trimMemory();
    });
    m_reclaimTimer->start();
}

void VideoWallpaper::setPlaylist(const QStringList &files)
{
    m_playlist = files;
    m_index = -1;
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
    if (!ensureWorker())
        return;

    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty())
        return;

    auto makeOutput = [this](const QRect &g, bool withAudio) {
        VideoOutput out;
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
        // Keep the pause/resume label in sync once playback really starts.
        connect(out.player, &QMediaPlayer::playbackStateChanged, this,
                [this](QMediaPlayer::PlaybackState st) {
            if (st == QMediaPlayer::PlayingState && m_index >= 0)
                emit playbackStateChanged(QStringLiteral("第 %1 个 播放中").arg(m_index + 1));
        });
        // 元数据就绪后应用帧率上限(此时才知道视频原生帧率)
        connect(out.player, &QMediaPlayer::metaDataChanged, this,
                [this, out] { applyPlaybackRate(out.player); });

        out.widget->setGeometry(g);
        out.widget->show();
        mountBehindIcons(out.widget);
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
    // 轻量重挂载：只修正父窗口与 z 序，不重建解码管线(避免换曲时资源反复销毁)。
    // 已挂载且尺寸正确的窗口直接跳过，消除每圈的 SetParent/SetWindowPos churn。
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        HWND hwnd = reinterpret_cast<HWND>(out.widget->winId());
        const qreal dpr = out.widget->devicePixelRatioF();
        const int w = int(out.widget->width() * dpr);
        const int h = int(out.widget->height() * dpr);
        RECT r;
        if (g_workerW && GetParent(hwnd) == g_workerW && GetWindowRect(hwnd, &r)
            && r.right - r.left == w && r.bottom - r.top == h)
            continue; // 已在正确位置
        mountBehindIcons(out.widget);
    }
}

void VideoWallpaper::teardownOutputs()
{
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (out.player)
            out.player->stop();
        if (out.widget) {
            HWND hwnd = reinterpret_cast<HWND>(out.widget->winId());
            SetParent(hwnd, nullptr);
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
    emit playbackStateChanged(QStringLiteral("第 %1 个 播放中").arg(m_index + 1));
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
void VideoWallpaper::evaluateSuspend()
{
    if (!m_started) {
        m_suspendReasons = 0;
        return;
    }

    int reasons = 0;
    if (m_pauseOnFullscreen && isForegroundFullscreen())
        reasons |= SuspendFullscreen;
    if (isWorkstationLocked())
        reasons |= SuspendLocked;
    if (!m_monitorOn)
        reasons |= SuspendMonitorOff;
    if (m_pauseOnBattery && isOnBattery())
        reasons |= SuspendBattery;
    m_suspendReasons = reasons;

    const bool shouldPlay = !m_manualPaused && reasons == 0;
    const bool wasPlaying = isPlaying();
    if (shouldPlay && !wasPlaying) {
        for (const VideoOutput &out : std::as_const(m_outputs))
            out.player->play();
        emit playbackStateChanged(QStringLiteral("第 %1 个 播放中").arg(m_index + 1));
        m_lastEmittedReasons = 0;
        return;
    }
    if (!shouldPlay && wasPlaying) {
        for (const VideoOutput &out : std::as_const(m_outputs))
            out.player->pause();
        if (m_reclaimMemory) {
            // 暂停后解码器队列逐渐排空，稍等片刻再把工作集还给系统
            QTimer::singleShot(2000, this, [this] {
                if (!isPlaying())
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
    // Return pages to the OS; playback pages them back in as needed.
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
}

void VideoWallpaper::setAutostart(bool on)
{
    appinfo::setAutostart(on);
}

bool VideoWallpaper::autostartEnabled() const
{
    return appinfo::autostartEnabled();
}

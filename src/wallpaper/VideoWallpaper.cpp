#include "VideoWallpaper.h"

#include "app/AppInfo.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "core/CachePaths.h"
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
#include <dwmapi.h>
#include <powrprof.h>
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Powrprof.lib")
#endif

// 仅 GUI 线程可调用的断言宏，用于确保后台线程不使用 QWidget/QVideoWidget。
#define VW_ASSERT_GUI() Q_ASSERT(QThread::currentThread() == qApp->thread())


namespace {

// 单例裸句柄：仅供 shutdown() 判断“实例是否存在”，绝不触发构造。
// 构造时赋值、析构时清空，因此退出清理完成后析构不会二次清理。
VideoWallpaper *g_wallpaper = nullptr;

// 持续挂起多久后卸载解码管线(省显存/内存)，恢复时重建约需 2s。
// YUMEIREN_LONG_SUSPEND_MS 仅用于自动化测试覆盖阈值。
constexpr qint64 kLongSuspendReleaseMs = 180000;

// 阶段3 错误分类：把 QMediaPlayer::Error 映射为用户可读文本(错误处理文档见
// docs/VIDEO_WALLPAPER_ERROR_HANDLING.md)。detail 仅在分类无法覆盖时透传。
QString mediaErrorText(QMediaPlayer::Error err, const QString &detail)
{
    switch (err) {
    case QMediaPlayer::ResourceError:
        return QStringLiteral("资源错误（文件缺失、损坏或读取失败）");
    case QMediaPlayer::FormatError:
        return QStringLiteral("格式不支持");
    case QMediaPlayer::NetworkError:
        return QStringLiteral("网络流错误");
    case QMediaPlayer::AccessDeniedError:
        return QStringLiteral("访问被拒绝");
    case QMediaPlayer::NoError:
        break;
    }
    return detail.isEmpty() ? QStringLiteral("未知错误") : detail;
}

} // namespace

// 显示模式名(仅诊断日志使用)：多屏问题的时序要靠这一行区分主屏/拉伸/镜像。
VideoWallpaper &VideoWallpaper::instance()
{
    static VideoWallpaper v;
    return v;
}
VideoWallpaper::~VideoWallpaper()
{
    // 兜底路径：正常退出应走 main() 里的显式 shutdown()(见 shutdownNow 注释)。
    // 这里只在“qApp 仍存活且尚未清理”时补做一次；qApp 已销毁时必须放弃清理——
    // 此时任何 QWidget 调用都会踩到 Qt6Widgets 内部的空 qApp 路径(c0000005)，
    // 正是 docs/crash_analysis.md 定位到的退出崩溃。
    g_wallpaper = nullptr;
    if (!qApp || m_shutdownDone)
        return;
    shutdownNow();
}
void VideoWallpaper::shutdown()
{
    // 单例不存在(未使用视频壁纸)时什么都不做：绝不能为了清理而构造单例。
    if (!qApp || !g_wallpaper)
        return;
    g_wallpaper->shutdownNow();
}
void VideoWallpaper::shutdownNow()
{
    if (m_shutdownDone)
        return; // 幂等：重复调用不再触碰任何 Qt 对象
    m_shutdownDone = true;
    m_shuttingDown = true; // 此后所有信号回调/延迟任务直接短路
    VW_ASSERT_GUI();
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("退出清理开始: outputs=%1 session=%2 uptime=%3ms")
            .arg(m_outputs.size()).arg(m_playbackSessionId)
            .arg(videodiag::elapsedMs()),
        QStringLiteral("Lifecycle"));
    stopHeartbeatTimers();
    // 取消尚未触发的延迟任务(重试/跳曲/重布局/裁剪)：context 是本单例，
    // 用 removePostedEvents 一次性摘掉，避免清理中再被回调拽回播放路径。
    QCoreApplication::removePostedEvents(this, QEvent::Timer);
    stopAll(); // 复用统一的停播收口(teardownOutputs + 状态复位)
    // 探针对象(仅 YUMEIREN_PROBE_STAGE 使用)同样必须赶在 qApp 销毁前释放，
    // 否则 ~QWidget/~QMediaPlayer 落到 atexit 链上，与上面的崩溃同因。
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
    m_mountFixClock = new QElapsedTimer();
    m_mountFixClock->start();
    m_suspendClock = new QElapsedTimer();

    m_fullscreenTimer = new QTimer(this);
    m_fullscreenTimer->setInterval(1000);
    connect(m_fullscreenTimer, &QTimer::timeout, this, &VideoWallpaper::evaluateSuspend);

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
    // 心跳定时器这里只创建不启动：只有“用户启动过且未停止”(m_started)期间才需要
    // 轮询，见 ensureHeartbeatTimers/stopHeartbeatTimers。未播放时进程完全静默
    // (任务书 10.1)，不再有空转的 1s/30s 唤醒。

    // 关闭/重载实验驱动(仅自动化测试使用)：定时调用 stopAll/startPlaying，
    // 用于验证“停止→等待→重载”后内存回落并复现同一基线(排查 deleteLater 残留)。
    if (const int stopMs = qEnvironmentVariableIntValue("YUMEIREN_AUTO_STOP_MS");
        stopMs > 0)
        QTimer::singleShot(stopMs, this, &VideoWallpaper::stopAll);
    if (const int startMs = qEnvironmentVariableIntValue("YUMEIREN_AUTO_START_MS");
        startMs > 0)
        QTimer::singleShot(startMs, this, [this] { startPlaying(nullptr); });
    // 暂停/恢复循环实验驱动(仅自动化测试使用)：每 N 毫秒切换一次 pauseResume()。
    // 用于「暂停/恢复 ×100」与「暂停期间是否仍在渲染」两项取证——外部没有
    // 稳定的入口驱动暂停按钮，只有 UI 点击。正常运行不设置该变量，零开销。
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

    // 分辨率/DPI/显示器热插拔变化 → 防抖后重建布局(仅播放中有效)。
    // QScreen 没有 devicePixelRatioChanged 信号；DPI 变化会同时触发 geometryChanged。
    const auto screens = QGuiApplication::screens();
    for (QScreen *s : screens)
        connect(s, &QScreen::geometryChanged, this, &VideoWallpaper::scheduleRelayout);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &VideoWallpaper::scheduleRelayout);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &VideoWallpaper::scheduleRelayout);

    // 阶段7 诊断：采样器仅诊断模式生效(默认 no-op)
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
    VW_ASSERT_GUI();
    m_started = true;
    ensureHeartbeatTimers(); // 起播即恢复挂起状态机与回收心跳
    ++m_playbackSessionId; // 会话 ID：每次起播/换曲/重试自增，供诊断日志关联
    if (index != m_index)
        m_fileRetries = 0; // 换曲重置单文件重试额度；同 index 的重试调用保留计数
    m_index = qBound(0, index, m_playlist.size() - 1);
    m_resumePosMs = resumePos;   // LoadedMedia 时消费；普通换曲传 -1 即无跳转
    // 任何一次显式起播都意味着"列表又活了"：播完标志必须失效，否则心跳的
    // 挂起恢复与管线重建会被上一次会话的收口状态永久锁住(壁纸再也捞不回来)。
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
    // 同一文件(单视频循环最常见)：不重设 source，避免 FFmpeg 每圈销毁重建解码器；
    // 从头继续播即可，零资源churn、无黑帧。
    bool sameSource = true;
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (out.player->source() != url) {
            sameSource = false;
            break;
        }
    }
    if (!sameSource) {
        // 媒体打开+解码器初始化需要 1-2s，立刻给出状态反馈避免“点了没反应”
        emit playbackStateChanged(
            QStringLiteral("第 %1 个 打开中…").arg(m_index + 1));
    }
    bool first = true;
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (!isLiveOutput(out))
            continue; // 防御：列表中不应有失效项，出现即跳过而非解引用
        if (sameSource)
            out.player->setPosition(0); // 解码器复用，原地重播
        else {
            out.player->setSource(url);
            applyAudioPolicy(out, first);
        }
        // 循环策略与媒体源无关，必须每次都断言：列表从 1 个变成多个(或反过来)时
        // 走的是 sameSource 快路径，漏掉这里会把 setLoops(Infinite) 遗留在播放器上，
        // 单曲循环就此吃掉整个列表。
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
        return; // 清理中不再推进列表(否则会被拽回起播路径)
    if (m_playlist.isEmpty())
        return;
    const int size = m_playlist.size();
    // 失败名单里的曲目不再参与轮换(阶段3)，避免坏素材每圈触发解码器重建
    const auto alive = [&](int i) { return !m_deadTracks.contains(i); };
    if (m_deadTracks.size() >= size) {
        emit playbackStateChanged(QStringLiteral("所有视频都无法播放，已停止"));
        stopAll();
        return;
    }
    // 单视频：绝不进入列表推进，也不产生任何用户可见的"播放结束"。
    // 放在失败名单判断之后，坏文件不会被无限重播(任务书 十二)。
    // 单循环模式同理：无论列表多长，都只把当前这一条从头再来一遍。
    if (isSeamlessLoop()) {
        restartSingleLoop();
        return;
    }
    // 随机模式：从"不是当前曲且可播"的候选里等概率挑一个。旧实现用 do-while
    // 反复摇骰子，一旦当前曲是唯一的幸存者就会原地死循环，这里改成有限候选表。
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
    // 列表循环：向后找下一条可播曲目，跨过列表末尾绕回表头，一圈接一圈。
    // 步数上限取 size 覆盖整个列表(最后一步会回到 m_index 自身)。
    int next = -1;
    for (int step = 1; step <= size; ++step) {
        const int i = (m_index + step) % size;
        if (alive(i)) {
            next = i;
            break;
        }
    }
    if (next < 0) {
        finishPlaylist(); // 防御收口：整表没有可播曲目(正常模式轮换到不了这里)
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
        return; // 清理中不再跳曲/重试
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
    m_manualPaused = false; // 用户点击“启动”即视为要求播放
    m_started = true;
    m_playbackFinished = false; // 新会话：允许心跳继续接管恢复
    ensureHeartbeatTimers();
    m_trackFails.clear(); // 全新起播会话：失败名单清空，所有曲目重新获得机会
    m_deadTracks.clear();
    m_fileRetries = 0;
    // 列表中选中了条目时，从选中项开始(用户点名的曲目优先于上次进度)
    const bool preferValid = preferIndex >= 0 && preferIndex < m_playlist.size();
    if (m_outputs.isEmpty()) {
        playIndex(preferValid ? preferIndex : (m_index >= 0 ? m_index : 0));
    } else if (preferValid && preferIndex != m_index) {
        playIndex(preferIndex); // 运行中点了启动且选中了其他曲目：直接切换
    } else {
        evaluateSuspend(); // 可能处于挂起原因中，由状态机决定播/停
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
    m_manualPaused = isPlaying(); // 正在播 → 用户要暂停；已暂停(含自动挂起) → 用户要继续
    // 用户亲手点的暂停/继续优先于"播完待命"：点了就要重新起播，
    // 否则播完的列表按了继续也没反应。
    // 播完之后点"继续"= 重走一圈：停在列表末尾的播放器直接 play() 只会立刻
    // 再结束一次，所以显式回到表头重新起播(sameSource 快路径，原地回绕不重建)。
    const bool restartLap = m_playbackFinished && !m_manualPaused;
    m_playbackFinished = false;
    if (restartLap && !m_playlist.isEmpty())
        playIndex(0);
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
    VW_ASSERT_GUI();
    stopHeartbeatTimers(); // 停播即停止轮询(任务书 10.1)
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
        trimMemory(); // 停止后立即把解码器释放后的内存还给系统，不等下一个回收周期
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("stopAll: 管线已卸载 session=%1").arg(m_playbackSessionId));
    // 退出清理中不再向 UI 广播状态变化：此时窗口正在销毁，文本无人消费。
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
    if (m_pauseOnFullscreen && fbswin::isForegroundFullscreen())
        reasons |= SuspendFullscreen;
    // 主屏模式下，前台应用盖满主屏工作区时壁纸完全不可见，暂停白省
    if (m_pauseOnFullscreen && m_screenMode == PrimaryScreen
        && fbswin::isDesktopCovered())
        reasons |= SuspendCovered;
    if (fbswin::isWorkstationLocked())
        reasons |= SuspendLocked;
    if (!m_monitorOn)
        reasons |= SuspendMonitorOff;
    if (m_pauseOnBattery && fbswin::isOnBattery())
        reasons |= SuspendBattery;
    m_suspendReasons = reasons;

    // 挂载健康检查放在同一条 1s 心跳里，开销为几次窗口句柄查询。
    // Progman 兜底挂载在部分 Win11 构建上不被 DWM 合成(壁纸永不显示)，
    // 因此兜底状态下由 scheduleMountFix 的 10s 节流持续重查，
    // 真正的 WorkerW 一出现就自动迁入。
    if (mountIsStale() || !fbswin::hasRealWorker())
        scheduleMountFix();

    const bool shouldPlay = !m_manualPaused && reasons == 0;
    const bool wasPlaying = isPlaying();

    // 长挂起期间管线已被释放，现在应当恢复：重建管线并跳回暂停时的进度
    if (shouldPlay && !m_playbackFinished && m_outputs.isEmpty()
        && !m_playlist.isEmpty()) {
        // 节流：挂载点缺失(如 explorer 未响应)时每 5s 重试一次，不空转
        if (!m_suspendClock->isValid() || m_suspendClock->elapsed() >= 5000) {
            m_suspendClock->restart();
            playIndex(qMax(0, m_index), m_resumePosMs);
        }
        m_lastEmittedReasons = 0;
        return;
    }

    // 只有"被挂起/被手动暂停后才解除"才需要心跳捞回来。列表正常播完(不循环)时
    // 播放器停在末尾，这里若照抄恢复逻辑就会每秒看到 !isPlaying，几秒后把整个
    // 列表从头重新点火——用户侧表现为壁纸闪没 + "播放结束/第 N 个 播放中"来回跳。
    if (shouldPlay && !wasPlaying && !m_playbackFinished) {
        videodiag::log(videodiag::Level::Info,
            QStringLiteral("恢复播放: reasons=0 manualPaused=%1").arg(m_manualPaused));
        // 无缝循环兜底：后端没遵守 setLoops(Infinite) 时(时长未知的流 / 个别
        // 后端)，播放器会停在末尾，心跳的 play() 只会让它原地卡死。先回绕再播。
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
    // 无缝循环看门狗：进度连续 3 拍(约 3s)完全不前进，说明后端既没回绕也没
    // 上报 EndOfMedia，壁纸会永远定格在最后一帧。原地回到起点重播，不停播、
    // 不切源、不重建窗口。进度倒退视为正常回绕，只重置计数。
    if (shouldPlay && wasPlaying && isSeamlessLoop() && !m_outputs.isEmpty()
        && isLiveOutput(m_outputs.first())
        // 素材还在探测/缓冲时进度本就不动，那不是停滞，别误判成卡死
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
        m_suspendClock->restart(); // 挂起计时开始(持续挂起超阈值即释放管线)
        if (m_reclaimMemory) {
            // 暂停后解码器队列逐渐排空，稍等片刻再把工作集还给系统
            QTimer::singleShot(2000, this, [this] {
                if (!m_shuttingDown && !isPlaying()
                    && (m_manualPaused || m_suspendReasons != 0))
                    trimMemory();
            });
        }
    }
    // 持续挂起超过阈值：壁纸反正看不见，整条解码管线+呈现表面全部释放，
    // 显存/内存回落到近空闲水平；恢复时重建并续播(代价 ~2s)
    // 例外：列表已播完(不循环)时不释放。此时解码器本就停着、只剩一张定格的
    // 末帧，而播完状态会拦住心跳的管线重建分支，卸载之后壁纸就再也回不来了。
    if (!shouldPlay && !wasPlaying && !m_playbackFinished && !m_outputs.isEmpty()
        && m_suspendClock->isValid()) {
        qint64 threshold = kLongSuspendReleaseMs;
        if (const int overrideMs = qEnvironmentVariableIntValue(
                "YUMEIREN_LONG_SUSPEND_MS");
            overrideMs > 0)
            threshold = overrideMs;
        if (m_suspendClock->elapsed() >= threshold)
            longSuspendRelease();
    }
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

    // 循环边界取证(任务书 十三.2)：复用同一条 1s 心跳顺带记录进度与对象地址，
    // 不新增定时器。Debug 级在非诊断模式下直接返回，产品环境零噪声。
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
        QStringLiteral("长挂起释放管线: resumePos=%1").arg(pos));
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

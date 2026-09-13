#include "videowallpaper.h"

#include "appinfo.h"
#include "platform/windows/desktopmount.h"
#include "videodiag.h"

#include <QFileInfo>

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
#include <QWindow>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

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
    m_suspendClock = new QElapsedTimer();

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

    // 关闭/重载实验驱动(仅自动化测试使用)：定时调用 stopAll/startPlaying，
    // 用于验证“停止→等待→重载”后内存回落并复现同一基线(排查 deleteLater 残留)。
    if (const int stopMs = qEnvironmentVariableIntValue("YUMEIREN_AUTO_STOP_MS");
        stopMs > 0)
        QTimer::singleShot(stopMs, this, &VideoWallpaper::stopAll);
    if (const int startMs = qEnvironmentVariableIntValue("YUMEIREN_AUTO_START_MS");
        startMs > 0)
        QTimer::singleShot(startMs, this, [this] { startPlaying(nullptr); });

    // 分辨率/DPI/显示器热插拔变化 → 防抖后重建布局(仅播放中有效)。
    // QScreen 没有 devicePixelRatioChanged 信号；DPI 变化会同时触发 geometryChanged。
    const auto screens = QGuiApplication::screens();
    for (QScreen *s : screens)
        connect(s, &QScreen::geometryChanged, this, &VideoWallpaper::scheduleRelayout);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &VideoWallpaper::scheduleRelayout);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &VideoWallpaper::scheduleRelayout);

    // 阶段7 诊断：采样器仅诊断模式生效(默认 no-op)
    videodiag::startDiagSampling();
    videodiag::log(videodiag::Level::Info, QStringLiteral("VideoWallpaper 初始化完成"));
}

void VideoWallpaper::setPlaylist(const QStringList &files)
{
    const QString current = (m_index >= 0 && m_index < m_playlist.size())
                                ? m_playlist.at(m_index)
                                : QString();
    m_playlist = files;
    // 播放中增删曲目时按文件名保持当前曲目指针，避免状态退化为“第 0 个”
    m_index = current.isEmpty() ? -1 : m_playlist.indexOf(current);
    // 列表内容可能变化，失败名单按索引记录，必须一并失效
    m_trackFails.clear();
    m_deadTracks.clear();
    m_fileRetries = 0;
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
        ++m_playersCreated;
        ++m_widgetsCreated;
        ++m_audiosCreated;
        videodiag::log(videodiag::Level::Debug,
            QStringLiteral("创建输出: players=%1/%2 widgets=%3/%4 audios=%5/%6")
                .arg(m_playersCreated).arg(m_playersDestroyed)
                .arg(m_widgetsCreated).arg(m_widgetsDestroyed)
                .arg(m_audiosCreated).arg(m_audiosDestroyed));
        out.audio->setMuted(true);
        out.player->setAudioOutput(out.audio);
        out.player->setVideoOutput(out.widget);

        // Track end: advance in the playlist or stop (keeping the last frame).
        // 阶段3：LoadedMedia 时先检查 hasVideo——纯音频素材对壁纸无意义：
        // 立即 stop(否则音频继续播放且会进入 PlayingState 重置失败计数)，
        // 然后走与错误一致的跳过链路(重试不会长出视频轨，故不重试)。
        connect(out.player, &QMediaPlayer::mediaStatusChanged, this,
                [this, out](QMediaPlayer::MediaStatus st) {
            videodiag::log(videodiag::Level::Debug,
                QStringLiteral("session=%1 mediaStatus=%2 file=%3")
                    .arg(m_playbackSessionId).arg(int(st))
                    .arg(QFileInfo(out.player->source().toLocalFile()).fileName()));
            if (st == QMediaPlayer::EndOfMedia)
                nextTrack();
            else if (st == QMediaPlayer::LoadedMedia && m_started
                     && !m_outputs.isEmpty()
                     && out.player == m_outputs.first().player) {
                // 长挂起释放后的恢复：跳回暂停时的进度
                if (m_resumePosMs > 0) {
                    out.player->setPosition(m_resumePosMs);
                    if (m_outputs.isEmpty() || out.player == m_outputs.last().player)
                        m_resumePosMs = -1;
                }
                // 无视频轨检测延迟 2s：LoadedMedia 时刻有效视频的 hasVideo 与
                // 分辨率元数据可能尚未就绪(实测误判过正常文件)。到期后仍是
                // 首个输出且仍加载同一 source，才判定为纯音频素材。
                if (!m_noVideoCheckPending) {
                    m_noVideoCheckPending = true;
                    const QUrl src = out.player->source();
                    QTimer::singleShot(2000, this, [this, out, src] {
                        m_noVideoCheckPending = false;
                        if (!m_started || m_outputs.isEmpty()
                            || out.player != m_outputs.first().player
                            || out.player->source() != src)
                            return;
                        if (!out.player->hasVideo()
                            && !out.player->metaData()
                                    .value(QMediaMetaData::Resolution).isValid()) {
                            out.player->stop();
                            handleUnplayable(QStringLiteral("没有视频轨"));
                        }
                    });
                }
            }
        });
        // Keep the pause/resume label in sync once playback really starts, and
        // count a successful start as proof the current file is playable.
        connect(out.player, &QMediaPlayer::playbackStateChanged, this,
                [this, out](QMediaPlayer::PlaybackState st) {
            if (st == QMediaPlayer::PlayingState) {
                // 仅“真素材成功”才恢复失败额度：无视频轨素材也会进入
                // PlayingState(音频在播)，不能借此洗白失败计数
                if (out.player->hasVideo()
                    || out.player->metaData()
                           .value(QMediaMetaData::Resolution).isValid())
                    m_trackFails.remove(m_index);
                m_fileRetries = 0;
                m_lastErrorText.clear();
                emitTrackState();
            }
        });
        // 元数据就绪后应用帧率上限(此时才知道视频原生帧率)；轨道选择也会被
        // 后端在媒体加载时重置为默认，所以这里同时重新断言音频策略。
        // 阶段6：分辨率高于主屏物理分辨率时给出一次性提示(不强制转码，
        // 策略见 docs/VIDEO_MEDIA_COMPATIBILITY_POLICY.md)。
        connect(out.player, &QMediaPlayer::metaDataChanged, this,
                [this, out, carriesAudio = withAudio] {
            videodiag::log(videodiag::Level::Debug,
                QStringLiteral("session=%1 metaDataChanged file=%2")
                    .arg(m_playbackSessionId)
                    .arg(QFileInfo(out.player->source().toLocalFile()).fileName()));
            applyAudioPolicy(out, carriesAudio);
            applyPlaybackRate(out.player);
            const QSize res = out.player->metaData()
                                  .value(QMediaMetaData::Resolution).toSize();
            if (!res.isValid() || res == m_lastHintRes)
                return;
            m_lastHintRes = res;
            QSize screen;
            if (const QScreen *s = QGuiApplication::primaryScreen()) {
                const qreal dpr = s->devicePixelRatio();
                screen = QSize(qRound(s->geometry().width() * dpr),
                               qRound(s->geometry().height() * dpr));
            }
            if (screen.isEmpty()
                || res.width() * res.height() <= screen.width() * screen.height())
                return;
            // 内存估算来自归因实验阶梯：固定 ~210MB + ~88MB/百万像素(±15%)
            const int est = qRound((210.0 + 88.0 * (double(res.width()) * res.height() / 1e6)) / 10) * 10;
            emit playbackStateChanged(QStringLiteral(
                "提示：视频分辨率 %1×%2 高于主屏物理分辨率，播放内存/显存占用较高"
                "（实测约 %3MB），可继续使用或更换适配素材")
                .arg(res.width()).arg(res.height()).arg(est));
        });
        // 解码/打开失败 → 有限重试 → 提示并自动跳过；连续失败铺满列表即整体
        // 停播。只有首个输出参与推进(MirrorAll 的副本播放器会对同一文件重复报错)。
        connect(out.player, &QMediaPlayer::errorOccurred, this,
                [this, out](QMediaPlayer::Error err, const QString &msg) {
            if (!m_started || m_outputs.isEmpty()
                || out.player != m_outputs.first().player)
                return;
            const QString reason = mediaErrorText(err, msg);
            videodiag::log(videodiag::Level::Warning,
                QStringLiteral("session=%1 媒体错误 file=%2 reason=%3 detail=%4")
                    .arg(m_playbackSessionId)
                    .arg(m_index >= 0 && m_index < m_playlist.size()
                             ? QFileInfo(m_playlist[m_index]).fileName()
                             : QStringLiteral("?"))
                    .arg(reason).arg(msg));
            // 有限重试(阶段3)：第1次失败 500ms 后原地重试，第2次 1500ms，
            // 第3次放弃跳曲。重试不推进列表；退避递增，杜绝高频重建。
            // 注意：错误态的播放器对同一 source 不会再发 errorOccurred，
            // 重试前必须清空 source 强制后端重新打开。
            if (m_fileRetries < 2) {
                const int delay = m_fileRetries == 0 ? 500 : 1500;
                ++m_fileRetries;
                emit playbackStateChanged(
                    QStringLiteral("第 %1 个打开失败（%2），重试 %3/2")
                        .arg(m_index + 1).arg(reason).arg(m_fileRetries));
                QTimer::singleShot(delay, this, [this] {
                    // 挂起/手动暂停期间照常重开媒体(只换源)，最终播停由
                    // evaluateSuspend 心跳统一裁决——否则挂起窗口内的重试
                    // 会被永久放弃，留下"已加载但无事件"的僵局(实测复现)。
                    if (!m_started || m_outputs.isEmpty())
                        return;
                    for (const VideoOutput &out : std::as_const(m_outputs))
                        if (out.player)
                            out.player->setSource(QUrl());
                    playIndex(m_index);
                });
                return;
            }
            handleUnplayable(reason);
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
    for (int i = 0; i < m_outputs.size(); ++i) {
        VideoOutput &out = m_outputs[i];
        // explorer 重启会销毁其 WorkerW 及挂在下面的我们的原生窗口，而 Qt 并不
        // 知道原生句柄已死(winId() 返回陈旧句柄)。仅重建原生窗口不够：实测
        // QVideoWidget 的呈现面(D3D 交换链)随旧窗口一起失效，播放器继续向旧
        // 表面送帧，新窗口永远收不到画面(壁纸"消失")。必须换全新 QVideoWidget
        // 并重新绑定视频输出，让呈现面从零建立。
        if (!IsWindow(reinterpret_cast<HWND>(out.widget->winId()))) {
            videodiag::log(videodiag::Level::Info,
                QStringLiteral("壁纸窗口原生句柄已失效(explorer 重启)，更换视频窗口并重绑输出"));
            QVideoWidget *old = out.widget;
            QVideoWidget *nw = new QVideoWidget;
            nw->setAspectRatioMode(old->aspectRatioMode());
            nw->setWindowFlags(old->windowFlags());
            nw->setGeometry(old->geometry());
            out.player->setVideoOutput(nw); // 视频输出重绑到新窗口(旧 sink 随之释放)
            out.widget = nw;
            nw->show();
            old->deleteLater();
        }
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
    // 探测失败(explorer 正在重启，Progman 尚未出现)时不重置节流——下个心跳(1s)
    // 立即重试，把重启后的壁纸黑屏时间从最长 10s 压到 ~2s
    if (!fbswin::ensureWorker())
        return;
    m_mountFixClock->restart();
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
    const int torn = m_outputs.size();
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        if (out.player)
            out.player->stop();
        if (out.widget) {
            fbswin::unmountWindow(out.widget);
            out.widget->hide();
            out.widget->deleteLater();
            ++m_widgetsDestroyed;
        }
        if (out.player)
            out.player->deleteLater();
        if (out.audio)
            out.audio->deleteLater();
        ++m_playersDestroyed;
        ++m_audiosDestroyed;
    }
    m_outputs.clear();
    if (torn > 0)
        videodiag::log(videodiag::Level::Info,
            QStringLiteral("teardown n=%1 累计 players=%2/%3 widgets=%4/%5 audios=%6/%7")
                .arg(torn)
                .arg(m_playersCreated).arg(m_playersDestroyed)
                .arg(m_widgetsCreated).arg(m_widgetsDestroyed)
                .arg(m_audiosCreated).arg(m_audiosDestroyed));
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

void VideoWallpaper::playIndex(int index, qint64 resumePos)
{
    if (m_playlist.isEmpty())
        return;
    m_started = true;
    ++m_playbackSessionId; // 会话 ID：每次起播/换曲/重试自增，供诊断日志关联
    if (index != m_index)
        m_fileRetries = 0; // 换曲重置单文件重试额度；同 index 的重试调用保留计数
    m_index = qBound(0, index, m_playlist.size() - 1);
    m_resumePosMs = resumePos;   // LoadedMedia 时消费；普通换曲传 -1 即无跳转
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
        if (sameSource)
            out.player->setPosition(0); // 解码器复用，原地重播
        else {
            out.player->setSource(url);
            applyAudioPolicy(out, first);
            applyLoopPolicy(out.player);
        }
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
    const int size = m_playlist.size();
    // 失败名单里的曲目不再参与轮换(阶段3)，避免坏素材每圈触发解码器重建
    const auto alive = [&](int i) { return !m_deadTracks.contains(i); };
    if (m_deadTracks.size() >= size) {
        emit playbackStateChanged(QStringLiteral("所有视频都无法播放，已停止"));
        stopAll();
        return;
    }
    if (m_random && size > 1) {
        int next = m_index;
        do { next = QRandomGenerator::global()->bounded(size); }
        while (next == m_index || !alive(next));
        playIndex(next);
        return;
    }
    int next = m_index;
    for (int step = 0; step < size; ++step) {
        next += 1;
        if (next >= size) {
            if (!m_autoLoop) {
                emit playbackStateChanged(QStringLiteral("播放结束"));
                return;
            }
            next = 0;
        }
        if (alive(next))
            break;
    }
    playIndex(next);
}

// 解码失败自动跳转的收口：挂起/手动暂停期间只换源不强行播放
// 列表双击(任务书交互)：运行中立即切换到指定条目作为壁纸。挂起/手动暂停期间
// 只换源不强行播放(与 advanceOnError 同策略)，恢复由状态机裁决；用户点名会
// 清除该曲目的失败记录，给一次重新打开的机会。
void VideoWallpaper::switchToTrack(int index)
{
    if (!m_started || index < 0 || index >= m_playlist.size() || index == m_index)
        return;
    m_trackFails.remove(index);
    m_deadTracks.remove(index);
    m_fileRetries = 0;
    playIndex(index);
    if (m_manualPaused || m_suspendReasons != 0) {
        for (const VideoOutput &out : std::as_const(m_outputs))
            if (out.player)
                out.player->pause();
    }
}

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

// 错误统一收口(阶段3)：重试耗尽或素材无视频轨时跳下一曲，该曲目立即进入
// 失败名单、不再参与后续轮换——轮转重试既造成坏素材乒乓循环，也触发后端
// 对特定媒体的重载阻塞(实测复现)；瞬态故障已由跳过前的原地重试覆盖。
// 全部曲目入名单则整体停播。仅首个输出允许调用(防 MirrorAll 重复推进)。
void VideoWallpaper::handleUnplayable(const QString &reason)
{
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
    if (m_playlist.isEmpty()) {
        if (error) *error = QStringLiteral("播放列表为空，请先添加视频");
        return false;
    }
    m_manualPaused = false; // 用户点击“启动”即视为要求播放
    m_started = true;
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
    if (m_outputs.isEmpty()) {
        // 长挂起已释放管线：重建并按当前状态继续(维持暂停由挂起原因决定)
        if (m_started && !m_playlist.isEmpty()) {
            m_manualPaused = false;
            evaluateSuspend();
        }
        return;
    }
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
    m_trackFails.clear();
    m_deadTracks.clear();
    m_fileRetries = 0;
    m_lastErrorText.clear();
    m_resumePosMs = -1;
    if (m_reclaimMemory)
        trimMemory(); // 停止后立即把解码器释放后的内存还给系统，不等下一个回收周期
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("stopAll: 管线已卸载 session=%1").arg(m_playbackSessionId));
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
    const int prev = m_screenMode;
    m_screenMode = qBound(0, mode, 2);
    if (m_screenMode == prev)
        return; // 值未变时不重建：启动时 loadSettings 会对已运行的管线重复调用，
                // 重建会短暂保留上一代窗口，平白多出一份渲染表面
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
    videodiag::log(videodiag::Level::Debug,
        QStringLiteral("setPlaybackRate(%1)").arg(rate));
    player->setPlaybackRate(rate);
}

void VideoWallpaper::setVolume(int percent)
{
    m_volume = qBound(0, percent, 100);
    bool first = true;
    for (const VideoOutput &out : std::as_const(m_outputs)) {
        out.audio->setMuted(!first);
        out.audio->setVolume(m_volume / 100.0);
        if (first)
            applyAudioPolicy(out, true); // 音量归零时连音频轨一起停掉
        first = false;
    }
}

// 音频策略：音量 0(壁纸默认态)时彻底不选音频轨——AAC 解码线程、重采样器和
// 音频设备占用全部省掉；调高音量由 setVolume 恢复。轨道选择会被后端在媒体
// 加载时重置，playIndex 和 metaDataChanged 两处都会调用这里重新断言。
void VideoWallpaper::applyAudioPolicy(const VideoOutput &out, bool carriesAudio)
{
    if (!out.player)
        return;
    out.player->setActiveAudioTrack(carriesAudio && m_volume > 0 ? 0 : -1);
}

// 单视频循环走后端原生 setLoops(Infinite)：EndOfMedia→setPosition(0) 的手工
// 循环在换头瞬间解码器 seek 会清空呈现面，壁纸闪黑帧；后端循环无黑帧间隙，
// 但实测回绕瞬间仍有约 1 帧的运动跳变(帧差约为正常运动的 2 倍，60fps 屏捕
// 帧级取证)，且后端不发任何事件、应用层无法拦截。彻底消除需内容级配合：
// 把首帧克隆 2-3 帧垫到片尾(tools/make_loop_clip.ps1)，跳变在数学上不可见。
// 多曲目列表仍走 EndOfMedia→nextTrack 手动推进(需要切源)。
void VideoWallpaper::applyLoopPolicy(QMediaPlayer *player)
{
    if (!player)
        return;
    const bool seamlessLoop = m_autoLoop && !m_random && m_playlist.size() == 1;
    player->setLoops(seamlessLoop ? QMediaPlayer::Infinite : QMediaPlayer::Once);
    videodiag::log(videodiag::Level::Debug,
        QStringLiteral("setLoops(%1) player=%2 widget=%3 source=%4")
            .arg(seamlessLoop ? QStringLiteral("Infinite") : QStringLiteral("Once"))
            .arg(quintptr(player), 0, 16)
            .arg(quintptr(player->videoOutput()), 0, 16)
            .arg(player->source().toString()));
}

void VideoWallpaper::setAutoLoop(bool on)
{
    m_autoLoop = on;
    for (const VideoOutput &out : std::as_const(m_outputs))
        applyLoopPolicy(out.player);
}

void VideoWallpaper::setRandom(bool on)
{
    m_random = on;
    for (const VideoOutput &out : std::as_const(m_outputs))
        applyLoopPolicy(out.player);
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
    if (shouldPlay && m_outputs.isEmpty() && !m_playlist.isEmpty()) {
        // 节流：挂载点缺失(如 explorer 未响应)时每 5s 重试一次，不空转
        if (!m_suspendClock->isValid() || m_suspendClock->elapsed() >= 5000) {
            m_suspendClock->restart();
            playIndex(qMax(0, m_index), m_resumePosMs);
        }
        m_lastEmittedReasons = 0;
        return;
    }

    if (shouldPlay && !wasPlaying) {
        videodiag::log(videodiag::Level::Info,
            QStringLiteral("恢复播放: reasons=0 manualPaused=%1").arg(m_manualPaused));
        for (const VideoOutput &out : std::as_const(m_outputs))
            out.player->play();
        emitTrackState();
        m_lastEmittedReasons = 0;
        return;
    }
    if (!shouldPlay && wasPlaying) {
        for (const VideoOutput &out : std::as_const(m_outputs))
            out.player->pause();
        m_suspendClock->restart(); // 挂起计时开始(持续挂起超阈值即释放管线)
        if (m_reclaimMemory) {
            // 暂停后解码器队列逐渐排空，稍等片刻再把工作集还给系统
            QTimer::singleShot(2000, this, [this] {
                if (!isPlaying() && (m_manualPaused || m_suspendReasons != 0))
                    trimMemory();
            });
        }
    }
    // 持续挂起超过阈值：壁纸反正看不见，整条解码管线+呈现表面全部释放，
    // 显存/内存回落到近空闲水平；恢复时重建并续播(代价 ~2s)
    if (!shouldPlay && !wasPlaying && !m_outputs.isEmpty()
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
}

void VideoWallpaper::setReclaimMemory(bool on)
{
    m_reclaimMemory = on;
}

void VideoWallpaper::trimMemory()
{
    fbswin::trimProcessMemory();
}

// 内存归因探针：把内存增量拆分到“创建播放器/设置媒体源/开始解码/创建呈现表面”
// 四个阶段。对象独立于 m_outputs(不进状态机、不触发回收定时器的判断)，进程即测
// 即弃；正常发布路径不会设置 YUMEIREN_PROBE_STAGE，此函数不执行。
void VideoWallpaper::runProbeStage(const QString &stage)
{
    if (stage == QLatin1String("B") || stage == QLatin1String("C")
        || stage == QLatin1String("D") || stage == QLatin1String("E")) {
        m_probePlayer = new QMediaPlayer(this);
        m_probeAudio = new QAudioOutput(this);
        m_probeAudio->setMuted(true);
        m_probePlayer->setAudioOutput(m_probeAudio);
    }
    if (stage == QLatin1String("B"))
        return;
    const QStringList files = appinfo::settings()
                                  .value(QStringLiteral("video/playlist")).toStringList();
    if (files.isEmpty())
        return;
    const QUrl url = QUrl::fromLocalFile(files.first());
    if (stage == QLatin1String("E")) {
        m_probeWidget = new QVideoWidget;
        m_probeWidget->setAspectRatioMode(Qt::IgnoreAspectRatio);
        m_probeWidget->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool
                                      | Qt::WindowTransparentForInput);
        m_probePlayer->setVideoOutput(m_probeWidget);
        const QRect g = QGuiApplication::primaryScreen()->geometry();
        m_probeWidget->setGeometry(g);
        m_probeWidget->show();
        fbswin::mountBehindIcons(m_probeWidget, g);
    }
    m_probePlayer->setSource(url);
    if (stage == QLatin1String("D"))
        m_probePlayer->play(); // 无 QVideoWidget：测纯解码开销，不建呈现表面
}

void VideoWallpaper::emitTrackState()
{
    if (m_index >= 0)
        emit playbackStateChanged(QStringLiteral("第 %1 个 播放中").arg(m_index + 1));
    else
        emit playbackStateChanged(QStringLiteral("播放中"));
}

// 持续挂起(全屏/遮挡/锁屏/熄屏)超过阈值：卸载整条解码管线。暂停态下
// 解码表面+帧池+交换链仍占着数百 MB 显存/内存，而壁纸根本不可见。
// m_resumePosMs 记住进度，恢复时重建管线经 LoadedMedia 跳回原位置。
void VideoWallpaper::longSuspendRelease()
{
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

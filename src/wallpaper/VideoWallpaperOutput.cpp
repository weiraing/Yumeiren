// VideoWallpaper 的输出窗口管理与播放器操作（主屏输出、挂载、错误恢复）。
#include "VideoWallpaper.h"

#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
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
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVideoWidget>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Powrprof.lib")
#endif

// 仅 GUI 线程可调用的断言宏
#define VW_ASSERT_GUI() Q_ASSERT(QThread::currentThread() == qApp->thread())

namespace {
// 与 VideoWallpaper.cpp 匿名命名空间中的同名实现保持一致
QString mediaErrorText(QMediaPlayer::Error error, const QString &none)
{
    switch (error) {
    case QMediaPlayer::NoError:
        return none;
    case QMediaPlayer::ResourceError:
        return QStringLiteral("资源错误(文件不存在或格式不支持)");
    case QMediaPlayer::FormatError:
        return QStringLiteral("格式错误(解码失败)");
    case QMediaPlayer::NetworkError:
        return QStringLiteral("网络错误");
    case QMediaPlayer::AccessDeniedError:
        return QStringLiteral("访问被拒绝");
    default:
        return QStringLiteral("未知错误(%1)").arg(static_cast<int>(error));
    }
}
} // namespace
#include <QWindow>

void VideoWallpaper::setPlaylist(const QStringList &files)
{
    const QString current = (m_index >= 0 && m_index < m_playlist.size())
                                ? m_playlist.at(m_index)
                                : QString();
    m_playlist = files;
    // 播放中增删曲目时按文件名保持当前曲目指针，避免状态退化为"第 0 个"
    m_index = current.isEmpty() ? -1 : m_playlist.indexOf(current);
    m_trackFails.clear(); // 失败名单按索引记录，列表变化后必须一并失效
    m_deadTracks.clear();
    m_fileRetries = 0;
    // 列表条数决定无缝循环是否生效，换列表后必须对现役播放器重新断言
    m_playbackFinished = false;
    for (const VideoOutput &out : std::as_const(m_outputs))
        if (isLiveOutput(out))
            applyLoopPolicy(out.player);
}
void VideoWallpaper::layoutOutputs()
{
    VW_ASSERT_GUI();
    if (m_shuttingDown)
        return;
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
        // 限帧中转 sink 挂在播放器下(而非本单例)，随播放器析构一并销毁。不能用 setVideoOutput(widget)：那样帧直喂窗口 sink，应用层没有插入点，保速丢帧无从实现
        out.tap = new QVideoSink(out.player);
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
        out.player->setVideoSink(out.tap);
        connect(out.tap, &QVideoSink::videoFrameChanged, this,
                [this, player = out.player](const QVideoFrame &frame) {
            forwardFrame(frame, player);
        });
        videodiag::logObjectEvent("create", out.player,
            QStringLiteral("player widget=%1 audio=%2 sink=%3")
                .arg(quintptr(out.widget), 0, 16)
                .arg(quintptr(out.audio), 0, 16)
                .arg(quintptr(out.tap), 0, 16));
        videodiag::logObjectEvent("bind", out.widget, QStringLiteral("player=0x%1")
            .arg(quintptr(out.player), 0, 16));

        // 纯音频素材对壁纸无意义：LoadedMedia 时立即 stop(否则音频继续播并重置失败计数)，再跳过
        connect(out.player, &QMediaPlayer::mediaStatusChanged, this,
                [this, out](QMediaPlayer::MediaStatus st) {
            if (!isLiveOutput(out))
                return; // 输出已卸载/正在退出
            videodiag::log(videodiag::Level::Debug,
                QStringLiteral("session=%1 mediaStatus=%2 playState=%3 pos=%4/%5 "
                               "file=%6 player=0x%7 widget=0x%8")
                    .arg(m_playbackSessionId).arg(int(st))
                    .arg(int(out.player->playbackState()))
                    .arg(out.player->position()).arg(out.player->duration())
                    .arg(QFileInfo(out.player->source().toLocalFile()).fileName())
                    .arg(quintptr(out.player), 0, 16)
                    .arg(quintptr(out.widget), 0, 16));
            // 只有首个输出推进列表(现固定单输出，isPrimaryOutput 恒真，保留以防未来再有多输出)
            if (st == QMediaPlayer::EndOfMedia) {
                if (isPrimaryOutput(out))
                    nextTrack();
            }
            else if (st == QMediaPlayer::LoadedMedia && m_started
                     && !m_outputs.isEmpty()
                     && isPrimaryOutput(out)) {
                // 长挂起释放后的恢复：跳回暂停时的进度
                if (m_resumePosMs > 0) {
                    out.player->setPosition(m_resumePosMs);
                    if (m_outputs.isEmpty() || out.player == m_outputs.last().player)
                        m_resumePosMs = -1;
                }
                // 无视频轨检测延迟 2s：LoadedMedia 时刻 hasVideo 与分辨率元数据尚未就绪(实测误判过)
                if (!m_noVideoCheckPending) {
                    m_noVideoCheckPending = true;
                    const QUrl src = out.player->source();
                    QTimer::singleShot(2000, this, [this, out, src] {
                        m_noVideoCheckPending = false;
                        if (!isLiveOutput(out) || !m_started || m_outputs.isEmpty()
                            || !isPrimaryOutput(out) || out.player->source() != src)
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
        // 仅"真素材成功"才恢复失败额度：无视频轨素材也会进入 PlayingState(音频在播)
        connect(out.player, &QMediaPlayer::playbackStateChanged, this,
                [this, out](QMediaPlayer::PlaybackState st) {
            if (!isLiveOutput(out))
                return;
            if (st == QMediaPlayer::PlayingState) {
                if (out.player->hasVideo()
                    || out.player->metaData()
                           .value(QMediaMetaData::Resolution).isValid())
                    m_trackFails.remove(m_index);
                m_fileRetries = 0;
                m_lastErrorText.clear();
                emitTrackState();
            }
        });
        // 元数据就绪后应用帧率上限；轨道选择会被后端在媒体加载时重置，故同时重新断言音频策略。素材分辨率超主屏时给一次性提示(策略见 docs/VIDEO_MEDIA_COMPATIBILITY_POLICY.md)
        connect(out.player, &QMediaPlayer::metaDataChanged, this,
                [this, out, carriesAudio = withAudio] {
            if (!isLiveOutput(out))
                return;
            videodiag::log(videodiag::Level::Debug,
                QStringLiteral("session=%1 metaDataChanged file=%2")
                    .arg(m_playbackSessionId)
                    .arg(QFileInfo(out.player->source().toLocalFile()).fileName()));
            applyAudioPolicy(out, carriesAudio);
            applyPlaybackRate(out.player);
            const QSize res = out.player->metaData()
                                  .value(QMediaMetaData::Resolution).toSize();
            if (!res.isValid())
                return; // 分辨率未知时不动自动限帧，等下一次回调
            QSize screen;
            if (const QScreen *s = QGuiApplication::primaryScreen()) {
                const qreal dpr = s->devicePixelRatio();
                screen = QSize(qRound(s->geometry().width() * dpr),
                               qRound(s->geometry().height() * dpr));
            }
            // 素材像素数超屏幕物理像素 → 本次会话自动限帧(不写用户配置)；用户手动设过则以其为准
            const bool oversized =
                !screen.isEmpty()
                && res.width() * res.height() > screen.width() * screen.height();
            const int autoFps = oversized ? kAutoFpsOversized : 0;
            if (autoFps != m_autoFps) {
                m_autoFps = autoFps;
                resetFramePacing();
                applyPlaybackRate(out.player);
                videodiag::log(videodiag::Level::Info,
                    QStringLiteral("自动限帧: %1x%2 对屏幕 %3x%4 → 上限=%5")
                        .arg(res.width()).arg(res.height())
                        .arg(screen.width()).arg(screen.height())
                        .arg(autoFps > 0 ? QString::number(autoFps)
                                         : QStringLiteral("跟随视频")));
            }
            if (res == m_lastHintRes)
                return;
            m_lastHintRes = res;
            if (!oversized)
                return;
            // 内存估算来自归因实验阶梯：固定 ~210MB + ~88MB/百万像素(±15%)
            const int est = qRound((210.0 + 88.0 * (double(res.width()) * res.height() / 1e6)) / 10) * 10;
            emit playbackStateChanged(QStringLiteral(
                "提示：视频分辨率 %1×%2 高于主屏物理分辨率，播放内存/显存占用较高"
                "（实测约 %3MB）；已自动把帧率上限设为 %4（保速丢帧，画面速度不变），"
                "可在左侧「帧率上限」改回跟随视频")
                .arg(res.width()).arg(res.height()).arg(est).arg(kAutoFpsOversized));
        });
        // 解码/打开失败 → 有限重试 → 提示并跳过；失败铺满列表即整体停播。只有首个输出参与推进
        connect(out.player, &QMediaPlayer::errorOccurred, this,
                [this, out](QMediaPlayer::Error err, const QString &msg) {
            if (!isLiveOutput(out) || !m_started || m_outputs.isEmpty()
                || !isPrimaryOutput(out))
                return;
            const QString reason = mediaErrorText(err, msg);
            videodiag::log(videodiag::Level::Warning,
                QStringLiteral("session=%1 媒体错误 file=%2 reason=%3 detail=%4")
                    .arg(m_playbackSessionId)
                    .arg(m_index >= 0 && m_index < m_playlist.size()
                             ? QFileInfo(m_playlist[m_index]).fileName()
                             : QStringLiteral("?"))
                    .arg(reason).arg(msg));
            // 有限重试：第1次 500ms、第2次 1500ms 后原地重试，第3次放弃跳曲。错误态播放器对同一 source 不再发 errorOccurred，故重试前须清空 source
            if (m_fileRetries < 2) {
                const int delay = m_fileRetries == 0 ? 500 : 1500;
                ++m_fileRetries;
                emit playbackStateChanged(
                    QStringLiteral("第 %1 个打开失败（%2），重试 %3/2")
                        .arg(m_index + 1).arg(reason).arg(m_fileRetries));
                QTimer::singleShot(delay, this, [this] {
                    // 挂起/暂停期间照常重开媒体(只换源)，播停由心跳统一裁决——否则重试会被永久放弃，留下"已加载但无事件"的僵局
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

    // 多屏档位(全屏拉伸/多屏镜像)已删(2026-09-24)：固定只铺主屏，行为与删除前的默认档一致。
    makeOutput(QGuiApplication::primaryScreen()->geometry(), true);
}
VideoWallpaper::VideoOutput *VideoWallpaper::liveOutputFor(const QMediaPlayer *player)
{
    // 按值捕获的 VideoOutput 副本会随重挂载/重建过期，故每次回现役表按指针取当前项
    if (m_shuttingDown || !player)
        return nullptr;
    for (VideoOutput &out : m_outputs)
        if (out.player == player)
            return &out;
    return nullptr;
}
void VideoWallpaper::forwardFrame(const QVideoFrame &frame, const QMediaPlayer *player)
{
    VideoOutput *out = liveOutputFor(player);
    if (!out || !out->widget)
        return; // 输出已卸载/正在退出
    QVideoSink *dst = out->widget->videoSink();
    if (!dst)
        return;
    const int fps = effectiveTargetFps();
    // 不限帧与慢动作档都整帧直通：慢动作档的限帧已由 setPlaybackRate 在上游完成
    if (fps <= 0 || !m_keepSpeed) {
        dst->setVideoFrame(frame);
        return;
    }
    // 保速丢帧：按节拍器决定这一帧是否转发。容差取周期 1/8(太小会在源帧率≈目标帧率时因取整抖动误丢，太大则把目标抬高一档)。节拍用单调时钟而非帧 PTS：不依赖 startTime，免疫暂停/回绕
    if (!out->frameClock.isValid()) {
        out->frameClock.start();
        out->nextFrameNs = 0;
    }
    const qint64 periodNs = 1000000000LL / fps;
    const qint64 now = out->frameClock.nsecsElapsed();
    if (now + periodNs / 8 < out->nextFrameNs)
        return; // 还没到下一个呈现时刻：不拷贝、不上传、不合成
    // 落后超过一个周期(暂停/回绕/seek 后)就重新对齐，不补帧补出连发
    out->nextFrameNs = (now > out->nextFrameNs + periodNs) ? now + periodNs
                                                          : out->nextFrameNs + periodNs;
    dst->setVideoFrame(frame);
}
void VideoWallpaper::resetFramePacing()
{
    // 只失效节拍器，不碰播放器/窗口：下一帧到达时 forwardFrame 重新起表并放行
    for (VideoOutput &out : m_outputs) {
        out.frameClock.invalidate();
        out.nextFrameNs = 0;
    }
}
void VideoWallpaper::remountOutputs()
{
    if (m_shuttingDown)
        return; // 清理期间只允许销毁，不允许再创建/挂载窗口
    // 轻量重挂载：只修正父窗口与位置，不重建解码管线(避免换曲时资源反复销毁)
    for (int i = 0; i < m_outputs.size(); ++i) {
        VideoOutput &out = m_outputs[i];
        // 桌面宿主被销毁会连带干掉挂在下面的原生窗口，而 Qt 不知道(winId() 已陈旧)。必须换全新 QVideoWidget：其呈现面(D3D 交换链)随旧窗口一起失效，只重建原生窗口播放器仍向旧表面送帧。
        if (!IsWindow(reinterpret_cast<HWND>(out.widget->winId()))) {
            videodiag::log(videodiag::Level::Info,
                QStringLiteral("壁纸窗口原生句柄已失效(explorer 重启)，更换视频窗口(限帧中转保持不变)"));
            QVideoWidget *old = out.widget;
            QVideoWidget *nw = new QVideoWidget;
            nw->setAspectRatioMode(old->aspectRatioMode());
            nw->setWindowFlags(old->windowFlags());
            nw->setGeometry(old->geometry());
            out.widget = nw;
            nw->show();
            old->deleteLater();
        }
        const qreal dpr = out.widget->devicePixelRatioF();
        const QRect phys(int(out.logicalRect.x() * dpr), int(out.logicalRect.y() * dpr),
                         int(out.logicalRect.width() * dpr),
                         int(out.logicalRect.height() * dpr));
        if (fbswin::isWindowMounted(out.widget, phys))
            continue;
        fbswin::mountBehindIcons(out.widget, out.logicalRect);
    }
}
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
    if (m_shuttingDown)
        return;
    if (m_mountFixClock->isValid() && m_mountFixClock->elapsed() < 10000)
        return;
    // 探测失败(explorer 重启中，Progman 尚未出现)时不重置节流，下个心跳(1s)立即重试，把重启后的黑屏时间从最长 10s 压到 ~2s
    if (!fbswin::ensureWorker())
        return;
    m_mountFixClock->restart();
    remountOutputs();
}
void VideoWallpaper::scheduleRelayout()
{
    if (m_shuttingDown || !m_started || m_outputs.isEmpty() || m_relayoutPending)
        return;
    m_relayoutPending = true;
    // DPI/几何变化会连发多个信号，防抖合并成一次重建
    QTimer::singleShot(600, this, [this] {
        m_relayoutPending = false;
        if (m_shuttingDown || !m_started || m_outputs.isEmpty())
            return;
        layoutOutputs();
        if (!m_playlist.isEmpty())
            playIndex(qMax(0, m_index));
        evaluateSuspend();
    });
}
void VideoWallpaper::teardownOutputs()
{
    if (m_outputs.isEmpty())
        return;
    const int torn = m_outputs.size();
    // 先换出整张表再处理：teardown 会被播放器信号链间接重入，重入时必须看到空列表，否则同一批对象被二次 deleteLater
    QList<VideoOutput> victims;
    victims.swap(m_outputs);
    // 退出清理时事件循环已不在、deleteLater 永不兑现，故就地强制兑现，保证都在 qApp 存活时销毁
    const bool flushNow = m_shuttingDown;
    for (const VideoOutput &out : std::as_const(victims)) {
        if (out.player) {
            // 先断开本类与播放器的全部连接，清理路径上不再有信号回调
            out.player->disconnect(this);
            out.player->stop();
        }
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
        if (flushNow) {
            // 顺序固定：播放器(持有 sink 与音频输出引用) → 音频输出 → 视频窗口
            QCoreApplication::sendPostedEvents(out.player, QEvent::DeferredDelete);
            QCoreApplication::sendPostedEvents(out.audio, QEvent::DeferredDelete);
            QCoreApplication::sendPostedEvents(out.widget, QEvent::DeferredDelete);
        }
    }
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("管线卸载 n=%1%2 累计 players=%3/%4 widgets=%5/%6 audios=%7/%8")
            .arg(torn)
            .arg(flushNow ? QStringLiteral("(退出模式·就地销毁)") : QString())
            .arg(m_playersCreated).arg(m_playersDestroyed)
            .arg(m_widgetsCreated).arg(m_widgetsDestroyed)
            .arg(m_audiosCreated).arg(m_audiosDestroyed),
        QStringLiteral("Lifecycle"));
}
bool VideoWallpaper::ensureOutputs(QString *error)
{
    if (m_shuttingDown) {
        if (error)
            *error = QStringLiteral("正在退出，不再创建视频输出");
        return false;
    }
    if (!m_outputs.isEmpty()) {
        remountOutputs(); // 轻量重挂载保持 z 序；解码管线复用，换曲零重建
        return true;
    }
    layoutOutputs();
    if (m_outputs.isEmpty()) {
        if (error) *error = QStringLiteral("未能获取 WorkerW 桌面挂载点");
        return false;
    }
    return true;
}
void VideoWallpaper::runProbeStage(const QString &stage)
{
    VW_ASSERT_GUI();
    if (stage == QLatin1String("B") || stage == QLatin1String("C")
        || stage == QLatin1String("D") || stage == QLatin1String("E")) {
        m_probePlayer = new QMediaPlayer(this);
        m_probeAudio = new QAudioOutput(this);
        m_probeAudio->setMuted(true);
        m_probePlayer->setAudioOutput(m_probeAudio);
    }
    if (stage == QLatin1String("B"))
        return;
    const QStringList files = AppConfig::instance()
        .value(ConfigKeys::Video::Playlist).toStringList();
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
        m_probePlayer->play(); // 无 QVideoWidget：测纯解码开销
}

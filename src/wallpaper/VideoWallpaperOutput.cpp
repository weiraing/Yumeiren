// VideoWallpaper 的输出窗口管理与播放器操作（多屏输出、挂载、错误恢复）。
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
// Copied from VideoWallpaper.cpp anonymous namespace (shared helpers)
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
    // 播放中增删曲目时按文件名保持当前曲目指针，避免状态退化为“第 0 个”
    m_index = current.isEmpty() ? -1 : m_playlist.indexOf(current);
    // 列表内容可能变化，失败名单按索引记录，必须一并失效
    m_trackFails.clear();
    m_deadTracks.clear();
    m_fileRetries = 0;
    // 列表条数决定无缝循环是否生效，换列表后必须对现役播放器重新断言；
    // "已播完"标志也随新列表失效。
    m_playbackFinished = false;
    for (const VideoOutput &out : std::as_const(m_outputs))
        if (isLiveOutput(out))
            applyLoopPolicy(out.player);
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
    VW_ASSERT_GUI(); // 创建/销毁 QVideoWidget：仅 GUI 线程
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
        videodiag::logObjectEvent("create", out.player,
            QStringLiteral("player widget=%1 audio=%2")
                .arg(quintptr(out.widget), 0, 16)
                .arg(quintptr(out.audio), 0, 16));
        videodiag::logObjectEvent("bind", out.widget, QStringLiteral("player=0x%1")
            .arg(quintptr(out.player), 0, 16));

        // Track end: advance in the playlist or stop (keeping the last frame).
        // 阶段3：LoadedMedia 时先检查 hasVideo——纯音频素材对壁纸无意义：
        // 立即 stop(否则音频继续播放且会进入 PlayingState 重置失败计数)，
        // 然后走与错误一致的跳过链路(重试不会长出视频轨，故不重试)。
        connect(out.player, &QMediaPlayer::mediaStatusChanged, this,
                [this, out](QMediaPlayer::MediaStatus st) {
            if (!isLiveOutput(out))
                return; // 输出已被卸载/正在退出清理：不再触碰播放器与状态机
            videodiag::log(videodiag::Level::Debug,
                QStringLiteral("session=%1 mediaStatus=%2 playState=%3 pos=%4/%5 "
                               "file=%6 player=0x%7 widget=0x%8")
                    .arg(m_playbackSessionId).arg(int(st))
                    .arg(int(out.player->playbackState()))
                    .arg(out.player->position()).arg(out.player->duration())
                    .arg(QFileInfo(out.player->source().toLocalFile()).fileName())
                    .arg(quintptr(out.player), 0, 16)
                    .arg(quintptr(out.widget), 0, 16));
            // 只有首个输出推进列表：MirrorAll 下每台显示器各有一个播放器，
            // 它们会对同一曲目各自上报一次 EndOfMedia，全部放行就会一次边界
            // 跳两格(不循环模式下表现为"第二个视频根本没播")。
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
                // 无视频轨检测延迟 2s：LoadedMedia 时刻有效视频的 hasVideo 与
                // 分辨率元数据可能尚未就绪(实测误判过正常文件)。到期后仍是
                // 首个输出且仍加载同一 source，才判定为纯音频素材。
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
        // Keep the pause/resume label in sync once playback really starts, and
        // count a successful start as proof the current file is playable.
        connect(out.player, &QMediaPlayer::playbackStateChanged, this,
                [this, out](QMediaPlayer::PlaybackState st) {
            if (!isLiveOutput(out))
                return;
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
    if (m_shuttingDown)
        return; // 清理期间只允许销毁，不允许再创建/挂载任何窗口
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
    // 探测失败(explorer 正在重启，Progman 尚未出现)时不重置节流——下个心跳(1s)
    // 立即重试，把重启后的壁纸黑屏时间从最长 10s 压到 ~2s
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
    // 先把整张表换出再逐个处理：teardown 可能被播放器信号链(EndOfMedia →
    // nextTrack → handleUnplayable → stopAll)间接重入，重入时看到必须是空列表，
    // 否则同一批对象会被二次 deleteLater(任务书 6.3 的再入风险)。
    QList<VideoOutput> victims;
    victims.swap(m_outputs);
    // 退出清理期间事件循环已经不在，deleteLater 永远不会被兑现；此时就地强制
    // 兑现，保证播放器/音频输出/视频窗口都在 qApp 存活时真正销毁(任务书 6.2)。
    const bool flushNow = m_shuttingDown;
    for (const VideoOutput &out : std::as_const(victims)) {
        if (out.player) {
            // 先断开本类与播放器的全部连接：清理路径上不应再有信号回调进来。
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
            // 顺序固定：播放器(持有 sink 与音频输出引用) → 音频输出 → 视频窗口。
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
        m_probePlayer->play(); // 无 QVideoWidget：测纯解码开销，不建呈现表面
}

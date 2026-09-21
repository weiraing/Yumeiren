/**
 * @file VideoWallpaper.h
 * @brief 动态壁纸核心类：视频播放、多屏输出、挂起策略与生命周期管理。
 */
#ifndef VIDEOWALLPAPER_H
#define VIDEOWALLPAPER_H

#include <QElapsedTimer>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QString>

#include <memory>

class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QVideoSink;
class QVideoFrame;
class QTimer;

/**
 * @brief 动态壁纸核心类（单例）。
 *
 * 生命周期要求：shutdown() 必须在 main() 中、QApplication 存活时调用（详见注释）。
 * 线程要求：所有公开方法必须在 Qt GUI 线程调用，播放器与窗口操作不可跨线程。
 */
class VideoWallpaper : public QObject
{
    Q_OBJECT
public:
    enum ScreenMode { PrimaryScreen = 0, StretchAll = 1, MirrorAll = 2 };
    // 播放模式(互斥单选框)：只播当前这条 / 按序循环 / 每次随机挑一条
    enum PlayMode { SingleLoop = 0, ListLoop = 1, Random = 2 };

    // 素材像素数高于屏幕物理像素时本次会话自动采用的帧率上限：实测 4K 每帧成本是 1080p 的 3.8 倍
    static constexpr int kAutoFpsOversized = 24;

    static VideoWallpaper &instance();

    const QStringList &playlist() const { return m_playlist; }
    void setPlaylist(const QStringList &files);
    void clearPlaylist();

    bool startPlaying(QString *error, int preferIndex = -1);
    void pauseResume();
    void stopAll();
    bool isPlaying() const;
    bool isManualPaused() const { return m_manualPaused; }
    int currentIndex() const { return m_index; }

    void setPlayMode(int mode);
    int playMode() const { return m_mode; }
    void setPauseOnFullscreen(bool on);
    void setPauseOnBattery(bool on);
    void setReclaimMemory(bool on);
    void setAutostart(bool on);
    bool autostartEnabled() const;
    void setScreenMode(int mode);
    void setVolume(int percent);
    void setMonitorOn(bool on);
    void setTargetFps(int fps);
    // true 保速丢帧(速率恒 1.0，多余帧在 QVideoSink 中转处丢弃)；false 慢动作(最省资源但画面变慢)
    void setKeepSpeed(bool on);
    bool keepSpeed() const { return m_keepSpeed; }
    // 有效帧率上限：手动设置优先，其次是自动限帧
    int effectiveTargetFps() const;
    // 当前是否处于自动限帧状态
    bool autoFpsActive() const { return m_targetFps <= 0 && m_autoFps > 0; }
    bool isStarted() const { return m_started; }
    void evaluateSuspend();
    // 退出收口：必须在 QApplication 仍存活时调用——静态单例析构在其后，QWidget 调用会踩空 qApp 崩溃(c0000005)。幂等。详见 docs/crash_analysis.md
    static void shutdown();

    // 运行中立即切换到指定条目作为壁纸；未启动时忽略
    void switchToTrack(int index);
    // 自动化内存实验探针(仅 YUMEIREN_PROBE_STAGE 驱动)：B=播放器+音频输出；C=B+媒体源；D=C+play()；E=C+挂载 QVideoWidget(不播放)
    void runProbeStage(const QString &stage);

signals:
    void playbackStateChanged(const QString &text);

private:
    explicit VideoWallpaper(QObject *parent = nullptr);
    ~VideoWallpaper() override;

    // 真正的清理动作(静态 shutdown() 与析构兜底共用)
    void shutdownNow();
    // 心跳定时器只在 m_started 期间运行，停止后不再有空转轮询
    void ensureHeartbeatTimers();
    void stopHeartbeatTimers();

    // 挂起原因(位掩码)：任一存在即暂停播放，全部消失且用户未手动暂停则恢复
    enum SuspendReason
    {
        SuspendNone = 0,
        SuspendFullscreen = 1,
        SuspendLocked = 2,
        SuspendMonitorOff = 4,
        SuspendBattery = 8,     // 用户开启后生效
        SuspendCovered = 16     // 仅主屏模式
    };

    // 单个显示器的播放器+输出窗口组合，由 layoutOutputs() 创建、teardownOutputs() 销毁，所有权归本类。
    struct VideoOutput
    {
        QVideoWidget *widget = nullptr;
        QMediaPlayer *player = nullptr;
        QAudioOutput *audio = nullptr;
        // 限帧中转：tap 的 videoFrameChanged 里按目标帧率决定是否转发给 widget->videoSink()
        QVideoSink *tap = nullptr;
        // 丢帧节拍器：只在 tap 回调里读写，用单调时钟而非帧 PTS
        QElapsedTimer frameClock;
        qint64 nextFrameNs = 0; // 下一个允许转发的时刻(ns，相对 frameClock)
        QRect logicalRect; // 期望的逻辑几何，重挂载时换算物理坐标比对
    };
    // 捕获型 lambda 的存活校验：只比对指针值，绝不解引用可能已释放的对象
    bool isLiveOutput(const VideoOutput &out) const;
    // 按播放器指针取当前有效的输出项(lambda 里的副本可能已过期)
    VideoOutput *liveOutputFor(const QMediaPlayer *player);
    void forwardFrame(const QVideoFrame &frame, const QMediaPlayer *player);
    // 让下一次转发立即放行(改帧率上限/切限帧方式/换素材后调用)
    void resetFramePacing();
    qint64 suspendReleaseThresholdMs(int reasons) const;
    bool isPrimaryOutput(const VideoOutput &out) const;
    bool ensureOutputs(QString *error);
    void layoutOutputs();
    void remountOutputs();
    bool mountIsStale() const;
    void scheduleMountFix();
    void scheduleRelayout();
    void teardownOutputs();
    void longSuspendRelease();
    void playIndex(int index, qint64 resumePos = -1);
    void nextTrack();
    // 当前曲目是否"从头无缝循环"：单循环模式或列表只剩一条。命中时交给后端 setLoops 回绕，不切源
    bool isSeamlessLoop() const;
    void restartSingleLoop();       // 无缝循环兜底：原地回到起点，不停播、不切源
    // 播放器是否已停在素材末尾(上报 EndOfMedia，或进度贴着时长不动)
    bool atMediaEnd(const QMediaPlayer *player) const;
    // 整表无可播曲目时的收口
    void finishPlaylist();
    void advanceOnError();
    // 重试耗尽或素材无视频轨时调用；仅首个输出允许调用(防 MirrorAll 重复推进)
    void handleUnplayable(const QString &reason);
    void trimMemory();
    void applyPlaybackRate(QMediaPlayer *player);
    void applyAudioPolicy(const VideoOutput &out, bool carriesAudio);
    void applyLoopPolicy(QMediaPlayer *player);
    void emitTrackState();

    QStringList m_playlist;
    int m_index = -1;
    int m_mode = SingleLoop;
    bool m_pauseOnFullscreen = false;
    bool m_pauseOnBattery = false;
    bool m_reclaimMemory = true;
    int m_screenMode = PrimaryScreen;
    int m_volume = 0;

    // FrameScheduler 状态机
    bool m_started = false;        // 用户启动过且未停止
    bool m_manualPaused = false;   // 用户手动暂停(优先于自动恢复)
    bool m_shuttingDown = false;   // 正在/已经退出清理(拒绝一切回调与延迟任务)
    bool m_shutdownDone = false;   // 清理完成(析构不再重复)
    bool m_monitorOn = true;       // 显示器电源状态(MainWindow 转发)
    int m_suspendReasons = 0;      // 当前生效的挂起原因
    int m_lastEmittedReasons = -1; // 去重：挂起原因不变时不重复发状态文本
    // 列表自然播完。心跳只负责捞回"被挂起"的播放器，绝不能把"已正常播完"的列表重新点火——那是状态文本反复横跳、壁纸闪没的根因
    bool m_playbackFinished = false;
    // 单视频循环看门狗：连续若干拍进度纹丝不动 = 后端既没回绕也没发 EndOfMedia
    qint64 m_watchPosMs = -1;
    int m_watchStalls = 0;
    // 帧率上限(0=跟随视频原生帧率)。默认 24：实测 4K60 内存/显存/CPU 约 -40%
    int m_targetFps = 24;
    bool m_keepSpeed = true; // true=保速丢帧(默认) / false=慢动作
    int m_autoFps = 0; // 本次会话的自动限帧值，绝不写进用户配置

    // 错误恢复：每曲目独立失败计数，跳过一次即入失败名单不再轮换(免坏曲目每圈重建解码器)；全部进名单才整体停播
    QHash<int, int> m_trackFails;
    QSet<int> m_deadTracks;
    QString m_lastErrorText;       // 同一错误只发一次状态
    int m_fileRetries = 0;         // 当前曲目有限重试计数(0/1=重试,≥2=跳过)

    // 长挂起释放：m_resumePosMs 记录暂停位置，恢复时经 LoadedMedia 跳回
    // 两个时钟都无父对象可挂，用 unique_ptr 管起来（原来是裸 new，没人 delete）。
    std::unique_ptr<QElapsedTimer> m_suspendClock;
    qint64 m_resumePosMs = -1;

    // Explorer 重启 / 窗口失效修复的节流
    std::unique_ptr<QElapsedTimer> m_mountFixClock;
    bool m_relayoutPending = false;

    QList<VideoOutput> m_outputs;
    QTimer *m_fullscreenTimer = nullptr;
    QTimer *m_reclaimTimer = nullptr;

    // 生命周期可观测性：会话 ID 与对象创建/销毁计数，仅诊断窗口使用
    quint64 m_playbackSessionId = 0;
    int m_playersCreated = 0, m_playersDestroyed = 0;
    int m_widgetsCreated = 0, m_widgetsDestroyed = 0;
    int m_audiosCreated = 0, m_audiosDestroyed = 0;

    QSize m_lastHintRes; // 同一分辨率只提示一次
    bool m_noVideoCheckPending = false; // 延迟无视频轨检测的去重标志

    // 探针专用对象(独立于 m_outputs，不参与状态机)
    QMediaPlayer *m_probePlayer = nullptr;
    QAudioOutput *m_probeAudio = nullptr;
    QVideoWidget *m_probeWidget = nullptr;
};

#endif // VIDEOWALLPAPER_H

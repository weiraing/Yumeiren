/**
 * @file VideoWallpaper.h
 * @brief 动态壁纸核心类：视频播放、多屏输出、挂起策略与生命周期管理。
 *
 * 负责视频播放列表管理、多显示器输出窗口的创建/挂载/销毁、播放/暂停/恢复
 * 策略（全屏遮挡、锁屏、电池、显示器关闭）、播放错误恢复和内存回收。
 * 窗口挂载到 Windows 桌面层（WorkerW）由 desktopmount 模块负责。
 */
#ifndef VIDEOWALLPAPER_H
#define VIDEOWALLPAPER_H

#include <QElapsedTimer>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QString>

class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QVideoSink;
class QVideoFrame;
class QTimer;

/**
 * @brief 动态壁纸核心类。
 *
 * 单例。负责视频播放、多屏输出、挂起/恢复策略和播放器生命周期管理。
 * 具体的 WorkerW 挂载由 fbswin:: 命名空间函数完成，不在本类范围内。
 *
 * 生命周期要求：
 * - startPlaying() 可重复调用，内部防重入；
 * - stopAll() 必须释放所有播放器和输出窗口；
 * - shutdown() 必须在 main() 中、QApplication 存活时调用（详见注释）。
 *
 * 线程要求：
 * - 所有公开方法必须在 Qt GUI 线程调用；
 * - QMediaPlayer 和 QVideoWidget 的操作不可跨线程。
 */
class VideoWallpaper : public QObject
{
    Q_OBJECT
public:
    enum ScreenMode { PrimaryScreen = 0, StretchAll = 1, MirrorAll = 2 };
    // 播放模式(三选一，UI 上是"模式"一行的互斥单选框)：
    //   SingleLoop 单循环 —— 只播当前这一条，播完从头再来(默认)
    //   ListLoop   列表循环 —— 按列表顺序逐个播，播完最后一条回到第一条
    //   Random     随机     —— 每次从列表随机挑一条，播完再随机挑下一条
    enum PlayMode { SingleLoop = 0, ListLoop = 1, Random = 2 };

    // 素材像素数高于屏幕物理像素时，本次会话自动采用的帧率上限。4K(3840×2160)
    // 在 2560×1600 屏上是 2.0 倍像素，实测 3D 引擎每帧成本也是 1080p 的 3.8 倍，
    // 限到 24 帧即可让 3D 占用按呈现帧数比例下降，而画面速度不受影响。
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
    // 限帧方式(见 ConfigKeys::Video::FpsKeepSpeed)：
    //   true  保速丢帧 —— 播放速率恒为 1.0，多出来的帧在 QVideoSink 中转处丢弃。
    //         画面速度与素材一致；GPU 的 3D 引擎(色彩转换+缩放)占用按呈现帧数
    //         线性下降，但解码引擎与帧池不变(解码器仍按源帧率出帧)。
    //   false 慢动作   —— setPlaybackRate 放慢，呈现帧数同样下降，且因消费端变慢
    //         把解码器一起拖住，解码、GPU、帧池三项同时下降，是资源最省的档位，
    //         代价是画面明显变慢。
    void setKeepSpeed(bool on);
    bool keepSpeed() const { return m_keepSpeed; }
    // 本次会话的有效帧率上限：手动设置优先，其次是对高分辨率素材的自动限帧。
    int effectiveTargetFps() const;
    // 当前是否处于「素材分辨率高于屏幕」的自动限帧状态(UI 提示用)。
    bool autoFpsActive() const { return m_targetFps <= 0 && m_autoFps > 0; }
    bool isStarted() const { return m_started; }
    void evaluateSuspend();
    // 退出收口(崩溃修复)：必须在 main() 里、QApplication 仍存活时调用。
    // 函数内静态单例的析构由 CRT atexit 链在 main() 返回之后执行，那时
    // ~QApplication 已跑完，任何 QWidget 调用都会落到 qApp==nullptr 的
    // Qt6Widgets 内部路径上(c0000005)。详见 docs/crash_analysis.md。
    // 幂等；单例尚未创建时不做任何事(绝不在此处构造单例)。
    static void shutdown();

    // 列表双击(任务书交互): 运行中立即切换到指定条目作为壁纸；
    // 未启动时忽略(此时由 startPlaying 的 preferIndex 决定起点)
    void switchToTrack(int index);
    // 自动化内存实验探针(仅 YUMEIREN_PROBE_STAGE 环境变量驱动，正常运行不触发)：
    // B=仅创建 QMediaPlayer+QAudioOutput / C=B+设置媒体源 / D=C+play()(无视频窗口) /
    // E=C+创建并挂载 QVideoWidget(不播放)。用于把内存增量归因到具体阶段。
    void runProbeStage(const QString &stage);

signals:
    void playbackStateChanged(const QString &text);

private:
    explicit VideoWallpaper(QObject *parent = nullptr);
    ~VideoWallpaper() override;

    // 真正的清理动作(成员函数，供静态 shutdown() 与析构兜底共用)
    void shutdownNow();
    // 心跳定时器：只在 m_started 期间运行，停止后不再有空转轮询(任务书 10.1)
    void ensureHeartbeatTimers();
    void stopHeartbeatTimers();

    // 挂起原因(位掩码)：任一存在即暂停播放，全部消失且用户未手动暂停则恢复
    enum SuspendReason
    {
        SuspendNone = 0,
        SuspendFullscreen = 1,  // 有窗口全屏(不要求它是前台窗口)
        SuspendLocked = 2,      // 系统锁定
        SuspendMonitorOff = 4,  // 显示器关闭
        SuspendBattery = 8,     // 电池供电(用户开启后生效)
        SuspendCovered = 16     // 桌面被应用窗口完全遮挡(主屏模式)
    };

    // 单个显示器的播放器+输出窗口组合。每个显示器一个 VideoOutput，
    // 由 ensureOutputs() 创建、layoutOutputs() 布局、teardownOutputs() 销毁。
    // player/audio/widget 的所有权归 VideoWallpaper，输出窗口由 Qt 父子机制管理。
    struct VideoOutput
    {
        QVideoWidget *widget = nullptr;
        QMediaPlayer *player = nullptr;
        QAudioOutput *audio = nullptr;
        // 限帧中转(保速丢帧)：播放器把帧交给 tap，tap 的 videoFrameChanged 里
        // 按目标帧率决定是否转发给 widget->videoSink()。帧是隐式共享的
        // QVideoFrame，转发只是一次引用计数，不产生拷贝；被丢掉的帧到此为止。
        QVideoSink *tap = nullptr;
        // 丢帧节拍器：只在 tap 的回调里读写。用单调时钟而不是帧 PTS，是为了
        // 不依赖后端是否填了 startTime，且对「暂停后恢复/回绕」天然免疫。
        QElapsedTimer frameClock;
        qint64 nextFrameNs = 0; // 下一个允许转发的时刻(ns，相对 frameClock)
        QRect logicalRect; // 期望的逻辑几何(所在屏幕/覆盖区域)，重挂载时换算物理坐标比对
    };
    // 捕获型 lambda 的存活校验(任务书 6.3)：只比对指针值，绝不解引用可能已释放的对象
    bool isLiveOutput(const VideoOutput &out) const;
    // 按播放器指针取当前有效的输出项(输出可能已被卸载/重建，lambda 里的副本会过期)
    VideoOutput *liveOutputFor(const QMediaPlayer *player);
    // tap 回调的落点：按目标帧率决定这一帧转不转发
    void forwardFrame(const QVideoFrame &frame, const QMediaPlayer *player);
    // 让下一次转发立即放行(改帧率上限/切限帧方式/换素材后调用)，避免沿用旧节拍
    void resetFramePacing();
    // 持续挂起多久后卸载解码管线。按挂起原因分级：看不见且恢复必然伴随人工
    // 动作的场景(锁屏/熄屏)尽快释放，可能随时切回桌面的场景(全屏/遮挡)留短驻留。
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
    // 当前曲目是否应当"从头无缝循环"：单循环模式，或列表里只剩一条素材。
    // 命中时交给后端 setLoops(Infinite) 回绕，不产生 EndOfMedia，也不切源。
    bool isSeamlessLoop() const;
    void restartSingleLoop();       // 无缝循环兜底：原地回到起点，不停播、不切源、不改状态文本
    // 播放器是否已经停在素材末尾(后端上报 EndOfMedia，或进度贴着时长不动)
    bool atMediaEnd(const QMediaPlayer *player) const;
    // 整表无可播曲目时的收口：只定格，绝不停止播放器
    void finishPlaylist();
    void advanceOnError();
    // 错误统一收口(阶段3)：重试耗尽或素材无视频轨时调用——跳下一曲，
    // 连续失败铺满列表则整体停播。仅首个输出允许调用(防 MirrorAll 重复推进)。
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
    // 生命周期保护(任务书 6.3)：退出清理中拒绝一切信号回调与延迟任务，
    // 并且清理只执行一次。两个标志各自只有一个含义，不构成状态机。
    bool m_shuttingDown = false;   // 正在/已经做退出清理
    bool m_shutdownDone = false;   // 清理完成(析构不再重复)
    bool m_monitorOn = true;       // 显示器电源状态(MainWindow 转发)
    int m_suspendReasons = 0;      // 当前生效的挂起原因
    int m_lastEmittedReasons = -1; // 去重：挂起原因不变时不重复发状态文本
    // 列表自然播完(整表无可播曲目)。1s 心跳只负责把"被挂起"的播放器捞回来，
    // 绝不能把"已经正常播完"的列表重新点火——那正是边界处
    // "播放结束 → 第 N 个 播放中"反复横跳、壁纸闪没的根因。
    bool m_playbackFinished = false;
    // 单视频循环看门狗：连续若干拍进度纹丝不动 = 后端既没回绕也没发 EndOfMedia，
    // 就地回绕重播。仅在心跳里读写，不参与其他逻辑。
    qint64 m_watchPosMs = -1;
    int m_watchStalls = 0;
    // 帧率上限(0=跟随视频原生帧率)。默认 24：高于 24fps 的素材按比例限帧，
    // 实测可显著降低内存/显存/CPU(4K60 约 -40%，见 VIDEO_MEDIA_COMPATIBILITY_POLICY.md)。
    // 限帧方式由 m_keepSpeed 决定：保速丢帧(默认，画面速度不变)或慢动作(更省)。
    int m_targetFps = 24;
    // true=保速丢帧(默认) / false=慢动作。见 setKeepSpeed。
    bool m_keepSpeed = true;
    // 本次会话的自动限帧值(0=不自动限帧)。素材像素数高于屏幕物理像素时置为
    // kAutoFpsOversized，只影响本次播放，绝不写进用户配置；手动设过帧率上限
    // (m_targetFps>0)时以手动值为准，自动值让位。
    int m_autoFps = 0;

    // 错误恢复(阶段3)：每曲目独立失败计数；跳过一次即入失败名单(原地重试
    // 已在跳过前完成)，不再参与后续轮换——避免坏曲目每圈解码器重建的乒乓
    // 循环及后端重载阻塞。全部曲目进入名单才整体停播。起播/改列表/停止时清空。
    QHash<int, int> m_trackFails;
    QSet<int> m_deadTracks;
    QString m_lastErrorText;       // 同一错误只发一次状态
    int m_fileRetries = 0;         // 当前曲目有限重试计数(0/1=重试,≥2=跳过)

    // 长挂起释放：暂停持续超过阈值即卸载解码管线省显存/内存，
    // m_resumePosMs 记录暂停位置，恢复时经 LoadedMedia 跳回
    QElapsedTimer *m_suspendClock = nullptr;
    qint64 m_resumePosMs = -1;

    // Explorer 重启 / 窗口失效修复的节流
    QElapsedTimer *m_mountFixClock = nullptr;
    bool m_relayoutPending = false;

    QList<VideoOutput> m_outputs;
    QTimer *m_fullscreenTimer = nullptr;
    QTimer *m_reclaimTimer = nullptr;

    // 生命周期可观测性(阶段2)：会话 ID 与对象创建/销毁计数。仅诊断窗口使用，
    // 正常路径零开销；阶段7 由 videodiag 输出。
    quint64 m_playbackSessionId = 0;
    int m_playersCreated = 0, m_playersDestroyed = 0;
    int m_widgetsCreated = 0, m_widgetsDestroyed = 0;
    int m_audiosCreated = 0, m_audiosDestroyed = 0;

    QSize m_lastHintRes; // 阶段6 高分辨率提示去重(同一分辨率只提示一次)
    bool m_noVideoCheckPending = false; // 阶段3 延迟无视频轨检测的去重标志

    // 探针专用对象(独立于 m_outputs，不参与状态机；由父对象持有至进程退出)
    QMediaPlayer *m_probePlayer = nullptr;
    QAudioOutput *m_probeAudio = nullptr;
    QVideoWidget *m_probeWidget = nullptr;
};

#endif // VIDEOWALLPAPER_H

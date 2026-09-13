#ifndef VIDEOWALLPAPER_H
#define VIDEOWALLPAPER_H

#include <QObject>
#include <QHash>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>

class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QElapsedTimer;
class QTimer;
// Live wallpaper: plays videos in frameless windows mounted behind the desktop
// icons (WorkerW). Rendering goes through QVideoWidget (GPU path, no per-frame
// CPU conversion) so memory stays flat while it plays.
class VideoWallpaper : public QObject
{
    Q_OBJECT
public:
    enum ScreenMode { PrimaryScreen = 0, StretchAll = 1, MirrorAll = 2 };

    static VideoWallpaper &instance();

    const QStringList &playlist() const { return m_playlist; }
    void setPlaylist(const QStringList &files);
    void clearPlaylist();

    bool startPlaying(QString *error);
    void pauseResume();
    void stopAll();
    bool isPlaying() const;
    bool isManualPaused() const { return m_manualPaused; }
    int currentIndex() const { return m_index; }

    void setAutoLoop(bool on);
    void setRandom(bool on);
    void setPauseOnFullscreen(bool on);
    void setPauseOnBattery(bool on);
    void setReclaimMemory(bool on);
    void setAutostart(bool on);
    bool autostartEnabled() const;
    void setScreenMode(int mode);
    void setVolume(int percent);
    void setMonitorOn(bool on);
    void setTargetFps(int fps);
    bool isStarted() const { return m_started; }
    void evaluateSuspend();
    // 自动化内存实验探针(仅 YUMEIREN_PROBE_STAGE 环境变量驱动，正常运行不触发)：
    // B=仅创建 QMediaPlayer+QAudioOutput / C=B+设置媒体源 / D=C+play()(无视频窗口) /
    // E=C+创建并挂载 QVideoWidget(不播放)。用于把内存增量归因到具体阶段。
    void runProbeStage(const QString &stage);

signals:
    void playbackStateChanged(const QString &text);

private:
    explicit VideoWallpaper(QObject *parent = nullptr);
    ~VideoWallpaper() override;

    // 挂起原因(位掩码)：任一存在即暂停播放，全部消失且用户未手动暂停则恢复
    enum SuspendReason
    {
        SuspendNone = 0,
        SuspendFullscreen = 1,  // 前台全屏应用
        SuspendLocked = 2,      // 系统锁定
        SuspendMonitorOff = 4,  // 显示器关闭
        SuspendBattery = 8,     // 电池供电(用户开启后生效)
        SuspendCovered = 16     // 桌面被前台窗口完全遮挡(主屏模式)
    };

    struct VideoOutput
    {
        QVideoWidget *widget = nullptr;
        QMediaPlayer *player = nullptr;
        QAudioOutput *audio = nullptr;
        QRect logicalRect; // 期望的逻辑几何(所在屏幕/覆盖区域)，重挂载时换算物理坐标比对
    };
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
    bool m_autoLoop = true;
    bool m_random = false;
    bool m_pauseOnFullscreen = false;
    bool m_pauseOnBattery = false;
    bool m_reclaimMemory = true;
    int m_screenMode = PrimaryScreen;
    int m_volume = 0;

    // FrameScheduler 状态机
    bool m_started = false;        // 用户启动过且未停止
    bool m_manualPaused = false;   // 用户手动暂停(优先于自动恢复)
    bool m_monitorOn = true;       // 显示器电源状态(MainWindow 转发)
    int m_suspendReasons = 0;      // 当前生效的挂起原因
    int m_lastEmittedReasons = -1; // 去重：挂起原因不变时不重复发状态文本
    // 帧率上限(0=跟随视频原生帧率)。默认 24：高于 24fps 的素材按比例放慢播放，
    // 实测可显著降低内存/显存/CPU(4K60 约 -40%，见 VIDEO_MEDIA_COMPATIBILITY_POLICY.md)。
    // 注意语义是"慢动作"而非丢帧渲染。
    int m_targetFps = 24;

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

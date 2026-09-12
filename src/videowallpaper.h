#ifndef VIDEOWALLPAPER_H
#define VIDEOWALLPAPER_H

#include <QObject>
#include <QRect>
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
    void playIndex(int index);
    void nextTrack();
    void advanceOnError();
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
    int m_targetFps = 30;          // 帧率上限(0=跟随视频原生帧率)

    // 错误恢复：连续失败达到播放列表长度即整体停播，成功起播即清零
    int m_errorStreak = 0;
    QString m_lastErrorText;       // 同一错误只发一次状态

    // Explorer 重启 / 窗口失效修复的节流
    QElapsedTimer *m_mountFixClock = nullptr;
    bool m_relayoutPending = false;

    QList<VideoOutput> m_outputs;
    QTimer *m_fullscreenTimer = nullptr;
    QTimer *m_reclaimTimer = nullptr;
};

#endif // VIDEOWALLPAPER_H

// 动态网页壁纸：WebView2 直挂桌面层。
//
// **架构红线：任何帧都不经过本进程**。浏览器的合成面直接呈现在挂在 WorkerW 下的
// 原生窗口里，DWM 直接合成 —— 我们只负责生命周期、挂起与设置，绝不做逐帧抓取回
// 传（那是把一个核烧在 GPU→CPU 回读上的错误路线，见模块评审结论）。
//
// 资源策略（与视频壁纸同一套心智）：
//   - 五类挂起（全屏/遮挡/锁屏/熄屏/电池）→ TrySuspend(掉到接近零渲染)，恢复即回；
//   - 持续挂起超阈值 → 整树销毁(控制器/环境)，回桌面自动重建；
//   - 快照模式(分钟/小时)：两次刷新间控制器不可见 + 挂起，桌面显示最近一次截图，
//     刷新 = 短暂唤醒渲染 → CapturePreview → 回到不可见。准静态页面的常态占用≈0；
//   - 音频只有 静音/取消静音 两档(WebView2 的 IsMuted 无音量级)，0 音量即静音；
//   - rAF 限帧：向页面注入脚本把 requestAnimationFrame 压到设定帧率，页面自身动效
//     (canvas/WebGL)随之降载；CSS 动画不受它管，压不到合成器层。
#ifndef WEBWALLPAPER_H
#define WEBWALLPAPER_H

#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;
struct ICoreWebView2Settings;
struct IStream;

class QElapsedTimer;
class QTimer;
class QWidget;

class WebWallpaper : public QObject
{
    Q_OBJECT
public:
    static WebWallpaper &instance();

    // 刷新策略：0=实时渲染 1=快照·每分钟 2=快照·每小时
    enum RefreshMode { Realtime = 0, SnapshotMinute = 1, SnapshotHour = 2 };

    // —— 生命周期 ——
    // source 为空时用配置里保存的。url(http/https/file)或本地路径/文件名(相对 data/web)。
    bool start(QString *error, const QString &source = QString());
    void stop();
    bool isRunning() const { return m_running; }
    bool isSuspended() const { return m_suspendReasons != 0; }

    // —— 设置(全部即时生效并落盘) ——
    void setInteractive(bool on);
    void setVolume(int percent);      // 0=静音;>0 取消静音(WebView2 无音量级)
    void setZoomPercent(int percent); // 50~200
    void setRefreshMode(int mode);
    void setFpsCap(int fps);          // 0=跟随页面,24/30/60
    void navigateTo(const QString &source); // 运行中换页
    // 显示器电源状态(WM_POWERBROADCAST 投递，与视频/看板娘同一事件源)。
    void setMonitorOn(bool on);

    // 运行时可用性(设置页置灰用)："检查 + 版本号"。
    static bool runtimeAvailable(QString *version);

    // 启动恢复判据：上次退出时网页壁纸在跑(配置键 web/enabled)。
    bool wasRunningLastTime() const;

    // —— 查询(界面只读) ——
    QString source() const { return m_source; }
    int refreshMode() const { return m_refreshMode; }
    bool interactive() const { return m_interactive; }
    int volume() const { return m_volume; }
    int zoomPercent() const { return m_zoomPercent; }
    int fpsCap() const { return m_fpsCap; }
    QString stateText() const { return m_stateText; }

signals:
    void stateChanged(const QString &text); // 人类可读状态(底部状态栏)
    void runningChanged(bool running);
    void snapshotUpdated();                 // 快照模式下出了一张新截图(重绘用)

public:
    // —— 内部(回调经 static 跳板进来，外部别调) ——
    void onEnvironmentReady(HRESULT hr, ICoreWebView2Environment *env);
    void onControllerReady(HRESULT hr, ICoreWebView2Controller *controller);
    void onNavigationCompleted(HRESULT hr);
    void onCapturePreview(HRESULT hr, IStream *stream);

private:
    explicit WebWallpaper(QObject *parent = nullptr);
    ~WebWallpaper() override;
    Q_DISABLE_COPY(WebWallpaper)

    QWidget *ensureHostWindow();
    void attachController();
    void applySettings();
    void applyBounds();
    void applyInteractive();
    void applyAudio();
    void suspendNoop();
    void applySuspend(bool suspend);
    void destroyPipeline();
    void setState(const QString &text);
    QString resolveSource(const QString &source) const; // → url
    void loadSettings();
    void evaluateSuspend();
    void startSnapshotCycle();
    void stopSnapshotCycle();
    void captureSnapshot();
    void publishRuntimeState();

    QString m_source;
    bool m_running = false;
    bool m_shuttingDown = false;
    bool m_creating = false;     // 环境/控制器创建在途
    QString m_stateText;

    // —— 设置镜像(单一来源在配置；这里只是缓存) ——
    int m_refreshMode = Realtime;
    bool m_interactive = false;
    int m_volume = 0;  // 0=静音(默认：壁纸别出声，用户要声音自己开)
    int m_zoomPercent = 100;
    int m_fpsCap = 0;

    // —— COM 管线 ——
    ICoreWebView2Environment *m_environment = nullptr;
    ICoreWebView2Controller *m_controller = nullptr;
    ICoreWebView2 *m_webview = nullptr;
    ICoreWebView2Settings *m_settings = nullptr;
    void *m_webview3 = nullptr;  // ICoreWebView2_3：TrySuspend/Resume
    void *m_webview8 = nullptr;  // ICoreWebView2_8：IsMuted
    QWidget *m_host = nullptr;   // 挂到桌面的宿主窗口
    QImage m_snapshot;           // 快照模式显示的最后一帧
    qint64 m_navToken = -1;       // EventRegistrationToken{INT64}，卸载事件用
    qint64 m_newWindowToken = -1; // 拦弹窗事件的注销令牌

    // —— 挂起(与视频壁纸同构) ——
    QTimer *m_heartbeat = nullptr;
    QTimer *m_createWatchdog = nullptr;
    QTimer *m_snapshotTimer = nullptr;
    QElapsedTimer *m_suspendClock = nullptr;
    QElapsedTimer *m_mountFixClock = nullptr; // 重挂节流(10s)，防心跳盲挂造成闪动
    int m_suspendReasons = 0;
    bool m_suspendedByUs = false;
    bool m_releasedForSuspend = false;
    bool m_monitorOn = true;
};

#endif // WEBWALLPAPER_H

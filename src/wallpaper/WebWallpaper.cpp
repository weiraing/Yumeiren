// 动态网页壁纸核心实现。设计红线与资源策略见 WebWallpaper.h 顶部注释。
#include "wallpaper/WebWallpaper.h"

#include "app/ApplicationRuntimeState.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/CachePaths.h"
#include "core/Diagnostics.h"
#include "platform/windows/desktopmount.h"
#include "platform/windows/webview2boot.h"
#include "wallpaper/VideoWallpaper.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPaintEvent>
#include <QWidget>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <functional>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include "WebView2.h"

#define VW_ASSERT_GUI() Q_ASSERT(QThread::currentThread() == qApp->thread())
#endif

namespace {

// 挂起原因位与分级释放阈值 —— 判据与视频壁纸一致，阈值也沿用同一套。
constexpr int kSuspendLocked = 1;
constexpr int kSuspendMonitorOff = 2;
constexpr int kSuspendFullscreen = 4;
constexpr int kSuspendCovered = 8;
constexpr int kSuspendBattery = 16;

constexpr qint64 kReleaseHiddenMs = 5000;    // 锁屏/熄屏：恢复必然伴随人工动作
constexpr qint64 kReleaseBatteryMs = 30000;
constexpr qint64 kReleaseCoveredMs = 60000;  // 全屏/遮挡：alt-tab 回来不该撞上重载
constexpr qint64 kReleaseDefaultMs = 180000;

constexpr int kSnapshotWarmupMs = 1500;      // 快照刷新：唤醒渲染后等这一拍再截图

#ifdef Q_OS_WIN

// 最小 COM 完成回调：MinGW 下没有 WRL/uuidof 可依赖，手写三件套 + 构造时传入
// 自己的 IID(常量由 WebView2.h 以 __declspec(selectany) 提供，直接可链)。
template <typename T, typename ArgsT>
class ComHandler final : public T
{
public:
    using Fn = std::function<void(HRESULT, ArgsT)>;
    ComHandler(Fn fn, const IID &iid) : fn_(std::move(fn)), iid_(iid) {}

    STDMETHODIMP QueryInterface(REFIID riid, void **out) override
    {
        if (!out)
            return E_POINTER;
        if (riid == iid_ || riid == IID_IUnknown) {
            *out = static_cast<T *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG r = --ref_;
        if (r == 0)
            delete this;
        return r;
    }
    // ArgsT 传「形参类型本身」：接口指针给 Xxx*，标量/字符串(LPCWSTR/BOOL)按值。
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ArgsT args) override
    {
        if (fn_)
            fn_(errorCode, args);
        return S_OK;
    }

private:
    Fn fn_;
    IID iid_;
    ULONG ref_ = 1;
};

// 单参完成回调：CapturePreview 这类只回错误码的 handler，模板多传一个参数
// 就会因 Invoke 形状不合而停留在抽象类。
template <typename T>
class ComHandler1 final : public T
{
public:
    using Fn = std::function<void(HRESULT)>;
    ComHandler1(Fn fn, const IID &iid) : fn_(std::move(fn)), iid_(iid) {}

    STDMETHODIMP QueryInterface(REFIID riid, void **out) override
    {
        if (!out)
            return E_POINTER;
        if (riid == iid_ || riid == IID_IUnknown) {
            *out = static_cast<T *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG r = --ref_;
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode) override
    {
        if (fn_)
            fn_(errorCode);
        return S_OK;
    }

private:
    Fn fn_;
    IID iid_;
    ULONG ref_ = 1;
};

// 事件处理器(NavigationCompleted 这类)与完成回调的 Invoke 形状不同：带 sender。
template <typename T, typename ArgsT>
class ComEventHandler final : public T
{
public:
    using Fn = std::function<void(ArgsT *)>;
    ComEventHandler(Fn fn, const IID &iid) : fn_(std::move(fn)), iid_(iid) {}

    STDMETHODIMP QueryInterface(REFIID riid, void **out) override
    {
        if (!out)
            return E_POINTER;
        if (riid == iid_ || riid == IID_IUnknown) {
            *out = static_cast<T *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG r = --ref_;
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ArgsT *args) override
    {
        Q_UNUSED(sender)
        if (fn_)
            fn_(args);
        return S_OK;
    }

private:
    Fn fn_;
    IID iid_;
    ULONG ref_ = 1;
};

// 宿主窗口：实时渲染时被 WebView2 的子窗口盖住，paint 什么都不画也行；快照模式
// 控制器不可见，由它把最近一次截图铺满。黑色打底而不是透明：壁纸底上透出桌面
// 反而像「加载坏了」。
class WebHostWidget : public QWidget
{
public:
    WebHostWidget()
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
        setMouseTracking(false);
    }
    void setSnapshot(const QImage &img)
    {
        m_snapshot = img;
        update();
    }
    const QImage &snapshot() const { return m_snapshot; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!m_snapshot.isNull())
            p.drawImage(rect(), m_snapshot);
    }

private:
    QImage m_snapshot;
};

// rAF 限帧注入脚本。cap 经 PostWebMessageAsJson 热更新，不用重导航。
// 只压得住走 requestAnimationFrame 的页面动效(canvas/WebGL/JS 动画)；CSS 过渡/
// 合成器动画不经过 rAF，压不到 —— 这是「尽力限帧」而不是硬上限。
const wchar_t kFpsCapBootstrap[] = LR"(
(function(){
  if (!window.__yumeirenRafPatched) {
    window.__yumeirenRafPatched = true;
    window.__yumeirenFpsCap = 0;
    var last = 0;
    var orig = window.requestAnimationFrame.bind(window);
    window.requestAnimationFrame = function(cb){
      function step(t){
        if (!window.__yumeirenFpsCap || t - last >= 1000/window.__yumeirenFpsCap - 2){
          last = t; cb(t);
        } else {
          orig(step);
        }
      }
      return orig(step);
    };
    try {
      window.chrome.webview.addEventListener('message', function(e){
        var d = e.data;
        if (d && typeof d.__yumeirenFpsCap === 'number')
          window.__yumeirenFpsCap = d.__yumeirenFpsCap;
      });
    } catch (err) {}
  }
})();
)";

qint64 suspendReleaseThresholdMs(int reasons)
{
    if (reasons & (kSuspendLocked | kSuspendMonitorOff))
        return kReleaseHiddenMs;
    if (reasons & kSuspendBattery)
        return kReleaseBatteryMs;
    if (reasons & (kSuspendFullscreen | kSuspendCovered))
        return kReleaseCoveredMs;
    return kReleaseDefaultMs;
}

QString reasonText(int reasons)
{
    if (reasons & kSuspendCovered)
        return QStringLiteral("桌面被完全遮挡，已暂停网页壁纸");
    if (reasons & kSuspendFullscreen)
        return QStringLiteral("检测到全屏应用，已暂停网页壁纸");
    if (reasons & kSuspendLocked)
        return QStringLiteral("系统已锁定，已暂停网页壁纸");
    if (reasons & kSuspendMonitorOff)
        return QStringLiteral("显示器已关闭，已暂停网页壁纸");
    if (reasons & kSuspendBattery)
        return QStringLiteral("电池模式，已暂停网页壁纸");
    return QString();
}

#endif // Q_OS_WIN

} // namespace

WebWallpaper &WebWallpaper::instance()
{
    static WebWallpaper w;
    return w;
}

WebWallpaper::WebWallpaper(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_WIN
    m_suspendClock = new QElapsedTimer();
    m_heartbeat = new QTimer(this);
    m_heartbeat->setInterval(1000);
    connect(m_heartbeat, &QTimer::timeout, this, &WebWallpaper::evaluateSuspend);
    m_createWatchdog = new QTimer(this);
    m_createWatchdog->setSingleShot(true);
    m_createWatchdog->setInterval(20000);
    connect(m_createWatchdog, &QTimer::timeout, this, [this] {
        if (m_creating) {
            m_creating = false;
            setState(QStringLiteral("网页壁纸启动超时(浏览器内核未响应)"));
            videodiag::log(videodiag::Level::Warning,
                           QStringLiteral("WebView2 创建超时，已放弃本次启动"),
                           QStringLiteral("WebWallpaper"));
            destroyPipeline();
        }
    });
    m_snapshotTimer = new QTimer(this);
    m_snapshotTimer->setSingleShot(true);
    connect(m_snapshotTimer, &QTimer::timeout, this, &WebWallpaper::captureSnapshot);
#endif
    loadSettings();
}

WebWallpaper::~WebWallpaper()
{
    m_shuttingDown = true;
    destroyPipeline();
}

bool WebWallpaper::runtimeAvailable(QString *version)
{
    return fbswin::webview2Available(version);
}

bool WebWallpaper::wasRunningLastTime() const
{
    return AppConfig::instance()
        .value(QString::fromLatin1(ConfigKeys::Web::Enabled), false).toBool();
}

void WebWallpaper::loadSettings()
{
    AppConfig &config = AppConfig::instance();
    m_refreshMode = config.value(QString::fromLatin1(ConfigKeys::Web::RefreshMode), 0).toInt();
    m_interactive = config.value(QString::fromLatin1(ConfigKeys::Web::Interactive), true).toBool();
    m_volume = config.value(QString::fromLatin1(ConfigKeys::Web::Volume), 0).toInt();
    m_zoomPercent = config.value(QString::fromLatin1(ConfigKeys::Web::Zoom), 100).toInt();
    m_fpsCap = config.value(QString::fromLatin1(ConfigKeys::Web::FpsCap), 0).toInt();
    m_source = config.value(QString::fromLatin1(ConfigKeys::Web::Source)).toString();
}

QWidget *WebWallpaper::ensureHostWindow()
{
    if (m_host)
        return m_host;
    m_host = new WebHostWidget;
    return m_host;
}

QString WebWallpaper::resolveSource(const QString &source) const
{
    QString s = source.trimmed();
    if (s.isEmpty())
        s = m_source;
    if (s.isEmpty())
        return s;
    if (s.startsWith(QLatin1String("http://")) || s.startsWith(QLatin1String("https://"))
        || s.startsWith(QLatin1String("file://")))
        return s;
    // 本地：绝对路径原样转 file://；相对路径/文件名相对 data/web 解析。
    QFileInfo info(s);
    if (!info.isAbsolute())
        info = QFileInfo(QCoreApplication::applicationDirPath()
                         + QStringLiteral("/data/web/") + s);
    return QUrl::fromLocalFile(info.absoluteFilePath()).toString();
}

bool WebWallpaper::start(QString *error, const QString &source)
{
#ifdef Q_OS_WIN
    VW_ASSERT_GUI();
    if (m_shuttingDown) {
        if (error)
            *error = QStringLiteral("正在退出，无法启动网页壁纸");
        return false;
    }
    QString version;
    if (!fbswin::webview2Available(&version)) {
        if (error)
            *error = QStringLiteral("WebView2 运行时不可用：") + version;
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("网页壁纸启动失败：") + version,
                       QStringLiteral("WebWallpaper"));
        return false;
    }

    const QString resolved = resolveSource(source);
    if (resolved.isEmpty()) {
        if (error)
            *error = QStringLiteral("还没有设置网页地址或本地页面");
        return false;
    }
    // 网页壁纸与视频壁纸共用同一块桌面层，谁启动谁独占。
    if (VideoWallpaper::instance().isStarted())
        VideoWallpaper::instance().stopAll();

    const bool sourceChanged = resolved != m_source;
    m_source = resolved;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Source), m_source);

    // 已在跑：只换页(本地文件内容变了也可用「应用」重触发)。
    if (m_running && m_webview) {
        if (sourceChanged) {
            m_webview->Navigate(reinterpret_cast<LPCWSTR>(m_source.utf16()));
            setState(QStringLiteral("切换页面中…"));
        }
        if (error)
            *error = QString();
        return true;
    }
    if (m_creating)
        return true; // 创建在途，本次只更新了 source

    m_running = true; // 意图先行：让界面/托盘立刻反映「网页壁纸已启动」
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Enabled), true);
    publishRuntimeState();
    setState(QStringLiteral("正在打开页面…"));
    emit runningChanged(true);

    m_host = ensureHostWindow();
    m_host->setGeometry(QGuiApplication::primaryScreen()->geometry());
    m_host->show();
    fbswin::mountBehindIcons(m_host, m_host->geometry());

    // 浏览器用户数据目录：登录态/缓存都落在 .cache/web-profile(清缓存可带走)。
    m_creating = true;
    m_createWatchdog->start();
    const HRESULT hr = fbswin::webview2CreateEnvironment(
        CachePaths::webProfile(), nullptr,
        new ComHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                       ICoreWebView2Environment *>(
            [this](HRESULT code, ICoreWebView2Environment *env) {
                onEnvironmentReady(code, env);
            },
            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler));
    if (FAILED(hr)) {
        m_creating = false;
        m_createWatchdog->stop();
        m_running = false;
        AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Enabled), false);
        publishRuntimeState();
        emit runningChanged(false);
        if (error)
            *error = QStringLiteral("WebView2 环境创建调用失败(0x%1)").arg(uint(hr), 8, 16, QChar('0'));
        return false;
    }
    m_heartbeat->start();
    if (error)
        *error = QString();
    return true;
#else
    Q_UNUSED(error)
    Q_UNUSED(source)
    return false;
#endif
}

void WebWallpaper::stop()
{
#ifdef Q_OS_WIN
    VW_ASSERT_GUI();
    m_running = false;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Enabled), false);
    destroyPipeline();
    m_heartbeat->stop();
    m_createWatchdog->stop();
    m_snapshotTimer->stop();
    setState(QStringLiteral("网页壁纸已停止"));
    publishRuntimeState();
    emit runningChanged(false);
#endif
}

void WebWallpaper::onEnvironmentReady(HRESULT code, ICoreWebView2Environment *env)
{
#ifdef Q_OS_WIN
    m_creating = false;
    m_createWatchdog->stop();
    if (m_shuttingDown || !m_running) {
        if (env)
            env->Release();
        return;
    }
    if (FAILED(code) || !env) {
        if (env)
            env->Release(); // 失败路径也要把回调移交的引用还掉
        m_running = false;
        AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Enabled), false);
        publishRuntimeState();
        emit runningChanged(false);
        setState(QStringLiteral("网页壁纸启动失败(浏览器内核创建失败)"));
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("WebView2 环境创建失败 hr=0x%1").arg(uint(code), 8, 16, QChar('0')),
                       QStringLiteral("WebWallpaper"));
        return;
    }
    // 回调移交的引用归本模块所有，本模块析构/重建时统一 Release(见
    // destroyPipeline)。不 AddRef 的话运行时在 Invoke 返回后释放它自己的那份，
    // 之下再调 put_IsVisible 就是 use-after-free(实测段错误就在这一步)。
    m_environment = env;
    env->AddRef();
    env->CreateCoreWebView2Controller(
        reinterpret_cast<HWND>(m_host->winId()),
        new ComHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                       ICoreWebView2Controller *>(
            [this](HRESULT hr, ICoreWebView2Controller *controller) {
                onControllerReady(hr, controller);
            },
            IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler));
#endif
}

void WebWallpaper::onControllerReady(HRESULT code, ICoreWebView2Controller *controller)
{
#ifdef Q_OS_WIN
    if (m_shuttingDown || !m_running) {
        if (controller) {
            controller->Close();
            controller->Release();
        }
        destroyPipeline();
        return;
    }
    if (FAILED(code) || !controller) {
        if (controller)
            controller->Release();
        m_running = false;
        AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Enabled), false);
        publishRuntimeState();
        emit runningChanged(false);
        setState(QStringLiteral("网页壁纸启动失败(无法挂载到桌面)"));
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("WebView2 控制器创建失败 hr=0x%1").arg(uint(code), 8, 16, QChar('0')),
                       QStringLiteral("WebWallpaper"));
        return;
    }
    m_controller = controller;
    controller->AddRef(); // 同上：回调移交的引用归本模块所有
    attachController();
#endif
}

void WebWallpaper::attachController()
{
#ifdef Q_OS_WIN
    m_controller->get_CoreWebView2(&m_webview);
    if (!m_webview) {
        setState(QStringLiteral("网页壁纸启动失败(内核不可用)"));
        return;
    }
    m_webview->get_Settings(&m_settings);
    m_webview->QueryInterface(IID_ICoreWebView2_3, &m_webview3); // TrySuspend/Resume
    m_webview->QueryInterface(IID_ICoreWebView2_8, &m_webview8); // IsMuted

    applySettings();
    applyBounds();
    applyInteractive();
    if (m_suspendReasons != 0)
        applySuspend(true);

    // 弹窗一律拦下：壁纸页面跳出新窗口是对桌面的入侵， handled 后什么都不发生。
    m_webview->add_NewWindowRequested(
        new ComEventHandler<ICoreWebView2NewWindowRequestedEventHandler,
                            ICoreWebView2NewWindowRequestedEventArgs>(
            [](ICoreWebView2NewWindowRequestedEventArgs *args) {
                if (args)
                    args->put_Handled(TRUE);
            },
            IID_ICoreWebView2NewWindowRequestedEventHandler),
        reinterpret_cast<EventRegistrationToken *>(&m_newWindowToken));

    // 页面加载完成：出图、起快照节奏。首图等 2s 让首帧真正绘制。
    m_webview->add_NavigationCompleted(
        new ComEventHandler<ICoreWebView2NavigationCompletedEventHandler,
                            ICoreWebView2NavigationCompletedEventArgs>(
            [this](ICoreWebView2NavigationCompletedEventArgs *args) {
                BOOL success = FALSE;
                if (args)
                    args->get_IsSuccess(&success);
                onNavigationCompleted(success ? S_OK : E_FAIL);
            },
            IID_ICoreWebView2NavigationCompletedEventHandler),
        reinterpret_cast<EventRegistrationToken *>(&m_navToken));

    // rAF 限帧引导脚本：每个新文档(含刷新/跳转)都会自动注入。
    m_webview->AddScriptToExecuteOnDocumentCreated(
        kFpsCapBootstrap,
        new ComHandler<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, LPCWSTR>(
            [](HRESULT, LPCWSTR) {},
            IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler));

    m_controller->put_ZoomFactor(m_zoomPercent / 100.0);
    applyAudio();
    startSnapshotCycle();

    m_webview->Navigate(reinterpret_cast<LPCWSTR>(m_source.utf16()));
    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("网页壁纸已挂载: %1 (refresh=%2 interactive=%3)")
                       .arg(m_source).arg(m_refreshMode).arg(m_interactive),
                   QStringLiteral("WebWallpaper"));
#endif
}

void WebWallpaper::applySettings()
{
#ifdef Q_OS_WIN
    if (!m_settings)
        return;
    // 壁纸形态：不要开发工具、右键菜单、状态栏 —— 弹出来的任何浮层都不属于桌面。
    m_settings->put_AreDevToolsEnabled(FALSE);
    m_settings->put_AreDefaultContextMenusEnabled(FALSE);
    m_settings->put_IsStatusBarEnabled(FALSE);
    m_settings->put_AreDefaultScriptDialogsEnabled(FALSE);
#endif
}

void WebWallpaper::applyBounds()
{
#ifdef Q_OS_WIN
    if (!m_controller || !m_host)
        return;
    const qreal dpr = m_host->devicePixelRatioF();
    const RECT bounds{0, 0, int(m_host->width() * dpr), int(m_host->height() * dpr)};
    m_controller->put_Bounds(bounds);
#endif
}

void WebWallpaper::applyInteractive()
{
#ifdef Q_OS_WIN
    if (!m_host || !m_host->testAttribute(Qt::WA_WState_Created))
        return;
    // 仅展示 = 鼠标穿透(WS_EX_TRANSPARENT)，与看板娘的鼠标穿透同一套底层开关。
    const HWND hwnd = reinterpret_cast<HWND>(m_host->winId());
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (m_interactive)
        ex &= ~WS_EX_TRANSPARENT;
    else
        ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
#endif
}

void WebWallpaper::applyAudio()
{
#ifdef Q_OS_WIN
    // WebView2 只有静音/取消静音两档，没有音量级；0 音量即静音。
    if (!m_webview8)
        return;
    auto *w8 = static_cast<ICoreWebView2_8 *>(m_webview8);
    w8->put_IsMuted(m_volume <= 0 ? TRUE : FALSE);
#endif
}

// TrySuspend 的完成回调没什么可做的，但 COM 异步调用要求 handler 非空。
void WebWallpaper::suspendNoop()
{
#ifdef Q_OS_WIN
    if (m_webview3)
        static_cast<ICoreWebView2_3 *>(m_webview3)->TrySuspend(
            new ComHandler<ICoreWebView2TrySuspendCompletedHandler, BOOL>(
                [](HRESULT, BOOL) {},
                IID_ICoreWebView2TrySuspendCompletedHandler));
#endif
}

void WebWallpaper::onNavigationCompleted(HRESULT code)
{
#ifdef Q_OS_WIN
    if (m_shuttingDown || !m_running)
        return;
    if (FAILED(code)) {
        setState(QStringLiteral("页面加载失败，已保持黑屏；请检查地址或网络"));
        return;
    }
    setState(m_refreshMode == Realtime
                 ? QStringLiteral("网页壁纸运行中")
                 : QStringLiteral("网页壁纸快照模式运行中"));
    if (m_refreshMode != Realtime)
        QTimer::singleShot(2000, this, &WebWallpaper::captureSnapshot);
#endif
}

// —— 快照模式：两次刷新间控制器不可见(浏览器近乎零渲染)，刷新 = 短暂唤醒 →
// CapturePreview → 回到不可见。实时模式整条链路不参与。 ——

void WebWallpaper::startSnapshotCycle()
{
#ifdef Q_OS_WIN
    if (m_refreshMode == Realtime) {
        m_snapshotTimer->stop();
        if (m_controller)
            m_controller->put_IsVisible(TRUE);
        return;
    }
    const int intervalMs = m_refreshMode == SnapshotMinute ? 60000 : 3600000;
    m_snapshotTimer->start(intervalMs);
#endif
}

void WebWallpaper::stopSnapshotCycle()
{
    m_snapshotTimer->stop();
}

void WebWallpaper::captureSnapshot()
{
#ifdef Q_OS_WIN
    if (m_shuttingDown || !m_running || !m_webview || m_refreshMode == Realtime)
        return;
    if (m_suspendReasons != 0)
        return; // 本来就看不见，这轮不截，等下一轮
    // 唤醒 → 等一拍渲染 → 截图 → 回到不可见。TrySuspend 挂起态先 Resume。
    if (m_webview3)
        static_cast<ICoreWebView2_3 *>(m_webview3)->Resume();
    if (m_controller)
        m_controller->put_IsVisible(TRUE);
    QTimer::singleShot(kSnapshotWarmupMs, this, [this] {
        if (m_shuttingDown || !m_running || !m_webview)
            return;
        // IStream 由 CapturePreview 写入 PNG，完成回调里读回。
        IStream *stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream)
            return;
        m_webview->CapturePreview(
            COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream,
            new ComHandler1<ICoreWebView2CapturePreviewCompletedHandler>(
                [this, stream](HRESULT code) {
                    onCapturePreview(code, stream);
                },
                IID_ICoreWebView2CapturePreviewCompletedHandler));
    });
#endif
}

void WebWallpaper::onCapturePreview(HRESULT code, IStream *stream)
{
#ifdef Q_OS_WIN
    QImage image;
    if (SUCCEEDED(code) && stream) {
        STATSTG stat{};
        if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0) {
            QByteArray bytes(int(stat.cbSize.QuadPart), Qt::Uninitialized);
            ULONG read = 0;
            LARGE_INTEGER zero{};
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            if (SUCCEEDED(stream->Read(bytes.data(), ULONG(bytes.size()), &read)) && read > 0)
                image.loadFromData(reinterpret_cast<const uchar *>(bytes.constData()),
                                    int(read), "PNG");
        }
    }
    stream->Release();
    if (m_shuttingDown)
        return;
    if (!image.isNull()) {
        m_snapshot = image;
        if (m_host)
            static_cast<WebHostWidget *>(m_host)->setSnapshot(image);
        emit snapshotUpdated();
    } else {
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("网页快照截图失败 hr=0x%1").arg(uint(code), 8, 16, QChar('0')),
                       QStringLiteral("WebWallpaper"));
    }
    // 回到省电态：不可见 + 挂起(若有 _3)。实时模式不走这条路径。
    if (m_refreshMode != Realtime && m_running && !m_suspendReasons) {
        if (m_controller)
            m_controller->put_IsVisible(FALSE);
        if (m_webview3)
            static_cast<ICoreWebView2_3 *>(m_webview3)->TrySuspend(nullptr);
    }
#endif
}

// —— 挂起：判据与视频壁纸一致；策略 = 立刻停渲染(TrySuspend)，持续超阈值整树
// 销毁，回桌面自动重建。 ——

void WebWallpaper::evaluateSuspend()
{
#ifdef Q_OS_WIN
    if (!m_running || m_shuttingDown || m_creating)
        return;

    int reasons = 0;
    if (fbswin::isFullscreenWindowPresent())
        reasons |= kSuspendFullscreen;
    if (fbswin::isDesktopCoveredByWindow())
        reasons |= kSuspendCovered;
    if (fbswin::isWorkstationLocked())
        reasons |= kSuspendLocked;
    if (!m_monitorOn)
        reasons |= kSuspendMonitorOff;
    if (fbswin::isOnBattery())
        reasons |= kSuspendBattery;
    m_suspendReasons = reasons;

    // 挂载健康检查复用心跳：explorer 重启/窗口失效时重新挂载；宿主原生窗口被
    // 连带销毁时只能整树重建。
    if (m_host) {
        if (!IsWindow(reinterpret_cast<HWND>(m_host->winId()))) {
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("网页壁纸宿主窗口失效，整树重建"),
                           QStringLiteral("WebWallpaper"));
            destroyPipeline();
            QString err;
            start(&err); // 重建(互斥 stopAll 对已停的视频壁纸是空操作)
            return;
        }
        const qreal dpr = m_host->devicePixelRatioF();
        const QRect phys(int(m_host->geometry().x() * dpr),
                         int(m_host->geometry().y() * dpr),
                         int(m_host->geometry().width() * dpr),
                         int(m_host->geometry().height() * dpr));
        if (!fbswin::isWindowMounted(m_host, phys))
            fbswin::mountBehindIcons(m_host, m_host->geometry());
    }

    const bool wasSuspended = m_suspendedByUs;
    const bool shouldSuspend = reasons != 0;
    if (shouldSuspend && !wasSuspended) {
        applySuspend(true);
        m_suspendClock->restart();
        const QString text = reasonText(reasons);
        if (!text.isEmpty())
            setState(text);
        publishRuntimeState();
    } else if (!shouldSuspend && wasSuspended) {
        applySuspend(false);
        setState(m_refreshMode == Realtime ? QStringLiteral("网页壁纸运行中")
                                           : QStringLiteral("网页壁纸快照模式运行中"));
        publishRuntimeState();
    }

    // 持续挂起超阈值：整树销毁，回桌面时由恢复分支重建。
    if (shouldSuspend && m_suspendClock->isValid()
        && m_suspendClock->elapsed() >= suspendReleaseThresholdMs(reasons)
        && !m_releasedForSuspend) {
        m_releasedForSuspend = true;
        destroyPipeline();
        setState(QStringLiteral("暂停较久，已释放网页壁纸资源；回到桌面自动恢复"));
        videodiag::log(videodiag::Level::Info,
                       QStringLiteral("网页壁纸长挂起释放: reasons=0x%1")
                           .arg(reasons, 0, 16),
                       QStringLiteral("WebWallpaper"));
    }
    if (!shouldSuspend)
        m_releasedForSuspend = false;
#endif
}

void WebWallpaper::applySuspend(bool suspend)
{
#ifdef Q_OS_WIN
    m_suspendedByUs = suspend;
    if (!m_controller)
        return;
    if (suspend) {
        // 快照模式本来就不可见，只挂起；实时模式两个都做。
        suspendNoop();
        m_controller->put_IsVisible(FALSE);
    } else {
        if (m_webview3)
            static_cast<ICoreWebView2_3 *>(m_webview3)->Resume();
        // 快照模式恢复后仍回不可见(显示的是截图)，由刷新节奏短暂唤醒。
        m_controller->put_IsVisible(m_refreshMode == Realtime ? TRUE : FALSE);
    }
#endif
}

void WebWallpaper::destroyPipeline()
{
#ifdef Q_OS_WIN
    if (m_webview && m_navToken >= 0) {
        EventRegistrationToken token{m_navToken};
        m_webview->remove_NavigationCompleted(token);
        m_navToken = -1;
    }
    if (m_controller) {
        m_controller->Close(); // 连带拆掉子窗口，之后宿主只剩自己的黑底/截图
        m_controller->Release();
        m_controller = nullptr;
    }
    if (m_settings) {
        m_settings->Release();
        m_settings = nullptr;
    }
    if (m_webview) {
        m_webview->Release();
        m_webview = nullptr;
    }
    m_webview3 = nullptr;
    m_webview8 = nullptr;
    if (m_environment) {
        m_environment->Release();
        m_environment = nullptr;
    }
    if (m_host) {
        fbswin::unmountWindow(m_host);
        m_host->hide();
        m_host->deleteLater();
        m_host = nullptr;
    }
    m_snapshot = QImage();
    m_suspendedByUs = false;
#endif
}

// —— 设置 setters：即时生效 + 落盘 ——

void WebWallpaper::setInteractive(bool on)
{
    m_interactive = on;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Interactive), on);
    applyInteractive();
}

void WebWallpaper::setVolume(int percent)
{
    m_volume = qBound(0, percent, 100);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Volume), m_volume);
    applyAudio();
}

void WebWallpaper::setZoomPercent(int percent)
{
    m_zoomPercent = qBound(50, percent, 200);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Zoom), m_zoomPercent);
#ifdef Q_OS_WIN
    if (m_controller)
        m_controller->put_ZoomFactor(m_zoomPercent / 100.0);
#endif
}

void WebWallpaper::setRefreshMode(int mode)
{
    m_refreshMode = qBound(0, mode, 2);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::RefreshMode), m_refreshMode);
    startSnapshotCycle();
    if (m_running && m_refreshMode == Realtime && m_controller && !m_suspendReasons)
        m_controller->put_IsVisible(TRUE);
    setState(m_running ? (m_refreshMode == Realtime
                              ? QStringLiteral("网页壁纸运行中")
                              : QStringLiteral("网页壁纸快照模式运行中"))
                       : m_stateText);
}

void WebWallpaper::setFpsCap(int fps)
{
    m_fpsCap = (fps == 0 || (fps >= 10 && fps <= 60)) ? fps : 0;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::FpsCap), m_fpsCap);
#ifdef Q_OS_WIN
    if (!m_webview)
        return;
    // 热更新：注入的引导脚本监听这个消息。
    m_webview->PostWebMessageAsJson(
        reinterpret_cast<LPCWSTR>(
            QStringLiteral("{\"__yumeirenFpsCap\":%1}").arg(m_fpsCap).utf16()));
#endif
}

void WebWallpaper::navigateTo(const QString &source)
{
#ifdef Q_OS_WIN
    const QString resolved = resolveSource(source);
    if (resolved.isEmpty())
        return;
    m_source = resolved;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Source), m_source);
    if (m_webview) {
        m_webview->Navigate(reinterpret_cast<LPCWSTR>(m_source.utf16()));
        setState(QStringLiteral("切换页面中…"));
    }
#endif
}

// —— 杂项 ——

void WebWallpaper::setState(const QString &text)
{
    if (m_stateText == text)
        return;
    m_stateText = text;
    emit stateChanged(text);
}

void WebWallpaper::setMonitorOn(bool on)
{
    if (m_monitorOn == on)
        return;
    m_monitorOn = on;
    evaluateSuspend(); // 不等下一拍心跳
}

void WebWallpaper::publishRuntimeState()
{
    ApplicationRuntimeState::instance().setWallpaperState(
        m_running, m_running && m_suspendReasons != 0);
}

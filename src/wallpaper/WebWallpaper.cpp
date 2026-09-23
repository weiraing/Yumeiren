// 动态网页壁纸核心实现。设计红线与资源策略见 WebWallpaper.h 顶部注释。
#include "wallpaper/WebWallpaper.h"

#include "app/ApplicationRuntimeState.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/CachePaths.h"
#include "core/Diagnostics.h"
#include "core/SuspendPolicy.h"
#include "platform/windows/desktopmount.h"
#include "platform/windows/webview2boot.h"
#include "wallpaper/VideoWallpaper.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QSet>
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
#include <tlhelp32.h>
#include <cwchar>
#include <cstring>
#include "WebView2.h"

#define VW_ASSERT_GUI() Q_ASSERT(QThread::currentThread() == qApp->thread())
#endif

namespace {

// 挂起原因位与分级释放阈值 —— 各位的阈值沿用视频壁纸那套。
// ⚠️ 网页壁纸**不做**「全屏/遮挡自动暂停」(2026-09-23 用户要求连同设置项一起移除)，
// 所以这里没有 4 / 8 两个挂起位；视频壁纸那边还有一套同名的，别混。
constexpr int kSuspendLocked = 1;
constexpr int kSuspendMonitorOff = 2;
constexpr int kSuspendBattery = 16;


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

// 浏览器启动参数：关后台联网与组件更新(壁纸不需要)，HTTP 磁盘缓存限幅 128MB
// —— user-data 目录默认会无限涨。
constexpr wchar_t kBrowserArguments[] =
    L"--disable-background-networking --disable-component-update --disk-cache-size=134217728";
// 运行时兼容版本下限：低于它的 Evergreen 会被拒绝创建环境。有意低于 vendored
// SDK 的 152.0.4191.47 —— 本模块只用到 ICoreWebView2_8 及更早的接口，放宽一档
// 可以多覆盖旧的运行时安装。将来用到更新接口时再同步抬高(参考
// third_party/webview2/include/WebView2EnvironmentOptions.h 的
// CORE_WEBVIEW_TARGET_PRODUCT_VERSION)。
constexpr wchar_t kTargetCompatibleVersion[] = L"148.0.3967.54";

// ICoreWebView2EnvironmentOptions 的最小实现：只实现 v1 的 8 个属性(顺序必须与
// MIDL 声明一致，乱序=虚表错位)，运行时对 Options2+ 的 QI 拿到 E_NOINTERFACE 后
// 会退回默认值 —— 这是接口契约允许的。手写而不引入 WRL：MinGW 侧 WRL 不可靠。
class EnvironmentOptions final : public ICoreWebView2EnvironmentOptions
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **out) override
    {
        if (!out)
            return E_POINTER;
        if (riid == IID_ICoreWebView2EnvironmentOptions || riid == IID_IUnknown) {
            *out = static_cast<ICoreWebView2EnvironmentOptions *>(this);
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

    STDMETHODIMP get_AdditionalBrowserArguments(LPWSTR *value) override
    { return copyOut(kBrowserArguments, value); }
    STDMETHODIMP put_AdditionalBrowserArguments(LPCWSTR) override { return S_OK; }
    STDMETHODIMP get_Language(LPWSTR *value) override { return copyOut(nullptr, value); }
    STDMETHODIMP put_Language(LPCWSTR) override { return S_OK; }
    STDMETHODIMP get_TargetCompatibleBrowserVersion(LPWSTR *value) override
    { return copyOut(kTargetCompatibleVersion, value); }
    STDMETHODIMP put_TargetCompatibleBrowserVersion(LPCWSTR) override { return S_OK; }
    STDMETHODIMP get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL *allow) override
    {
        if (!allow)
            return E_POINTER;
        *allow = FALSE;
        return S_OK;
    }
    STDMETHODIMP put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override { return S_OK; }

private:
    static HRESULT copyOut(LPCWSTR src, LPWSTR *out)
    {
        if (!out)
            return E_POINTER;
        // Language 的默认值是空字符串，不是 nullptr；WebView2 运行时会直接读取
        // 返回的字符串，传 nullptr 进去会在 wcslen 处触发访问冲突并终止进程。
        if (!src)
            src = L"";
        const size_t bytes = (wcslen(src) + 1) * sizeof(wchar_t);
        *out = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (!*out)
            return E_OUTOFMEMORY;
        memcpy(*out, src, bytes);
        return S_OK;
    }
    ULONG ref_ = 1;
};

// msedgewebview2 子进程(浏览器主进程 + 它派生的渲染/GPU 进程)全部降到「低于正常」
// 优先级：前台应用永远优先拿 CPU，壁纸的解码/合成只能在空闲档位里跑。
void applyWebView2ChildPriority()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    const DWORD ourPid = GetCurrentProcessId();
    auto isWebview = [](const wchar_t *name) {
        return _wcsicmp(name, L"msedgewebview2.exe") == 0;
    };
    // 第一遍：找到属于本进程的浏览器主进程
    QSet<quint32> browsers;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (isWebview(pe.szExeFile) && pe.th32ParentProcessID == ourPid)
                browsers.insert(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    if (browsers.isEmpty()) {
        CloseHandle(snap);
        return;
    }
    // 第二遍：主进程 + 其渲染/GPU 子进程一并降级
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!isWebview(pe.szExeFile))
                continue;
            if (browsers.contains(pe.th32ProcessID)
                || browsers.contains(pe.th32ParentProcessID)) {
                if (HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE,
                                           pe.th32ProcessID)) {
                    SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

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
    // 分级阈值见 core/SuspendPolicy.h(此前网页壁纸是三份拷贝里唯一没有测试压档的，
    // 现补上 YUMEIREN_WEB_SUSPEND_MS，与视频/看板娘同名机制对齐)。
    const auto graded = [&](qint64 ms) {
        return suspendpolicy::thresholdWithEnvOverride("YUMEIREN_WEB_SUSPEND_MS", ms);
    };
    if (reasons & (kSuspendLocked | kSuspendMonitorOff))
        return graded(suspendpolicy::kHiddenMs);
    if (reasons & kSuspendBattery)
        return graded(suspendpolicy::kBatteryMs);
    return graded(suspendpolicy::kDefaultMs);
}

// 快照节拍间隔；排程点有三处(周期入口/失败重排/挂起跳过)，统一从这里取。
int snapshotIntervalMs(int refreshMode)
{
    return refreshMode == WebWallpaper::SnapshotMinute ? 60000 : 3600000;
}

QString reasonText(int reasons)
{
    if (reasons & kSuspendLocked)
        return QStringLiteral("系统已锁定，已暂停网页壁纸");
    if (reasons & kSuspendMonitorOff)
        return QStringLiteral("显示器已关闭，已暂停网页壁纸");
    if (reasons & kSuspendBattery)
        return QStringLiteral("电池模式，已暂停网页壁纸");
    return QString();
}

#endif // Q_OS_WIN

QString findIndexDocument(const QString &directory)
{
    const QDir dir(directory);
    const QFileInfoList files =
        dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &preferred :
         {QStringLiteral("index.html"), QStringLiteral("index.htm")}) {
        for (const QFileInfo &file : files) {
            if (file.fileName().compare(preferred, Qt::CaseInsensitive) == 0)
                return file.absoluteFilePath();
        }
    }
    return QString();
}

// 快照静态页在缓存里的落盘位置(writeSnapshotPage 写、attachController 查)。
QString snapshotPageFile()
{
    return QDir(CachePaths::webSnapshot()).filePath(QStringLiteral("page.html"));
}

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
    m_mountFixClock = new QElapsedTimer();
    m_mountFixClock->start(); // 首次挂载健康检查即刻可用
    m_heartbeat = new QTimer(this);
    m_heartbeat->setInterval(1000);
    connect(m_heartbeat, &QTimer::timeout, this, &WebWallpaper::evaluateSuspend);
    m_createWatchdog = new QTimer(this);
    m_createWatchdog->setSingleShot(true);
    m_createWatchdog->setInterval(20000);
    connect(m_createWatchdog, &QTimer::timeout, this, [this] {
        if (!m_creating)
            return;
        m_creating = false;
        setState(QStringLiteral("网页壁纸启动超时(浏览器内核未响应)"));
        applog::log(applog::Level::Warning,
                       QStringLiteral("WebView2 创建超时，已放弃本次启动"),
                       QStringLiteral("WebWallpaper"));
        // 与其他失败路径对称地整场复位：只拆管线不清 m_running 的话，界面会一直
        // 显示「运行中/停止」状态而管线已死，且永远不会自愈或自动重试。
        destroyPipeline();
        m_running = false;
        AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Enabled), false);
        m_heartbeat->stop();
        m_snapshotTimer->stop();
        publishRuntimeState();
        emit runningChanged(false);
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
    return winhelper::webview2Available(version);
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
    // 兜底 false = 「网页展示」(鼠标穿透)，与 AppConfig 的布尔默认表一致
    // (2026-09-23 用户定案，原为 true)。
    m_interactive = config.value(QString::fromLatin1(ConfigKeys::Web::Interactive), false).toBool();
    m_volume = config.value(QString::fromLatin1(ConfigKeys::Web::Volume), 0).toInt();
    m_zoomPercent = config.value(QString::fromLatin1(ConfigKeys::Web::Zoom), 100).toInt();
    m_fpsCap = config.value(QString::fromLatin1(ConfigKeys::Web::FpsCap), 30).toInt();
    m_source = config.value(QString::fromLatin1(ConfigKeys::Web::Source)).toString();
}

QWidget *WebWallpaper::ensureHostWindow()
{
    if (m_host)
        return m_host;
    m_host = new WebHostWidget;
    return m_host;
}

QString WebWallpaper::resolveSource(const QString &source, QString *error) const
{
    if (error)
        error->clear();
    QString s = source.trimmed();
    if (s.isEmpty())
        s = m_source;
    if (s.isEmpty()) {
        if (error)
            *error = QStringLiteral("还没有设置网页地址或本地页面");
        return s;
    }
    if (s.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || s.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        return s;
    }

    // 本地：绝对路径转 file://；相对路径/文件名相对 data/web 解析。目录来源
    // 按完整 Web 项目处理，自动找 index.html 或 index.htm。
    QFileInfo info;
    if (s.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) {
        const QUrl url(s);
        if (!url.isLocalFile())
            return s;
        info = QFileInfo(url.toLocalFile());
    } else {
        info = QFileInfo(s);
    }
    if (!info.isAbsolute()) {
        info = QFileInfo(QCoreApplication::applicationDirPath()
                         + QStringLiteral("/data/web/") + s);
    }
    info = QFileInfo(QDir::cleanPath(info.absoluteFilePath()));
    if (!info.exists()) {
        if (error)
            *error = QStringLiteral("本地网页不存在：%1")
                         .arg(QDir::toNativeSeparators(info.absoluteFilePath()));
        return QString();
    }
    if (info.isDir()) {
        const QString entry = findIndexDocument(info.absoluteFilePath());
        if (entry.isEmpty()) {
            if (error)
                *error = QStringLiteral("所选 Web 项目目录缺少 index.html 或 index.htm：%1")
                             .arg(QDir::toNativeSeparators(info.absoluteFilePath()));
            return QString();
        }
        info = QFileInfo(entry);
    }
    if (!info.isFile()) {
        if (error)
            *error = QStringLiteral("所选来源不是网页文件：%1")
                         .arg(QDir::toNativeSeparators(info.absoluteFilePath()));
        return QString();
    }
    return QUrl::fromLocalFile(info.absoluteFilePath()).toString();
}

// 带用户缩放地导航到真实来源；首挂/换页/快照刷新/恢复实时共用。
void WebWallpaper::navigateReal()
{
#ifdef Q_OS_WIN
    m_snapshotShowing = false; // 离开静态快照页(刷新/换页/切实时共用)
    if (m_controller)
        m_controller->put_ZoomFactor(m_zoomPercent / 100.0);
    if (m_webview)
        m_webview->Navigate(reinterpret_cast<LPCWSTR>(m_resolvedSource.utf16()));
#endif
}

// 导航到本地静态快照页(稳态)。缩放必须归位 100%：截图是按用户缩放渲染后的
// 结果，再叠一层缩放会把画面放大变形。
void WebWallpaper::navigateSnapshotPage()
{
#ifdef Q_OS_WIN
    // 导航在途即按稳态记账：完成回调只补状态文本，不再触发出图。
    m_snapshotShowing = true;
    m_snapshotRefreshing = false;
    if (m_controller)
        m_controller->put_ZoomFactor(1.0);
    if (m_webview && !m_snapshotPageUrl.isEmpty())
        m_webview->Navigate(reinterpret_cast<LPCWSTR>(m_snapshotPageUrl.utf16()));
#endif
}

// 快照模式标记一次刷新在途：导航完成后由 onNavigationCompleted 接力出图。
void WebWallpaper::beginSnapshotRefresh()
{
    if (m_refreshMode == Realtime)
        return;
    m_snapshotShowing = false;
    m_snapshotRefreshing = true;
}

// 把最近一张快照落成 .cache/web-snapshot/{snapshot.png,page.html}。失败时调用方
// 保持实时渲染等下一拍重试，绝不让桌面黑屏。
bool WebWallpaper::writeSnapshotPage()
{
    const QString dir = CachePaths::webSnapshot();
    if (!QDir().mkpath(dir))
        return false;
    const QString png = QDir(dir).filePath(QStringLiteral("snapshot.png"));
    if (!m_snapshot.save(png, "PNG"))
        return false;
    // 时间戳同时落在文档 URL 与 <img> 上：文件名不变，浏览器缓存同名资源时
    // 也能拿到新图，稳态刷新才不会停在旧一帧。
    const QString stamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray page = QStringLiteral(
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
        "html,body{margin:0;height:100%;background:#000;overflow:hidden}"
        "img{position:fixed;left:0;top:0;width:100vw;height:100vh}"
        "</style></head><body><img src=\"snapshot.png?t=%1\"></body></html>")
        .arg(stamp).toUtf8();
    QFile file(QDir(dir).filePath(QStringLiteral("page.html")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (file.write(page) != page.size())
        return false;
    m_snapshotPageUrl = QUrl::fromLocalFile(file.fileName()).toString()
                        + QStringLiteral("?t=") + stamp;
    return true;
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
    if (!winhelper::webview2Available(&version)) {
        if (error)
            *error = QStringLiteral("WebView2 运行时不可用：") + version;
        applog::log(applog::Level::Warning,
                       QStringLiteral("网页壁纸启动失败：") + version,
                       QStringLiteral("WebWallpaper"));
        return false;
    }

    QString requested = source.trimmed();
    if (requested.isEmpty())
        requested = m_source;
    const QString resolved = resolveSource(requested, error);
    if (resolved.isEmpty()) {
        if (error && error->isEmpty())
            *error = QStringLiteral("还没有设置网页地址或本地页面");
        return false;
    }
    // 网页壁纸与视频壁纸共用同一块桌面层，谁启动谁独占。视频侧 startPlaying
    // 会 stop() 掉网页壁纸并清 web/enabled；这里反向接管也要清掉它的恢复标志
    // —— 否则「看过视频→改用网页→退出」的用户，下次启动会被恢复成视频壁纸。
    if (VideoWallpaper::instance().isStarted()) {
        VideoWallpaper::instance().stopAll();
        AppConfig::instance().setValue(
            QString::fromLatin1(ConfigKeys::Video::WasPlaying), false);
    }

    const bool sourceChanged = resolved != m_resolvedSource;
    // 运行态用解析后的 URL，配置和界面保留用户选择的原始相对路径/URL。
    m_source = requested;
    m_resolvedSource = resolved;
    if (sourceChanged)
        m_snapshotPageUrl.clear(); // 缓存静态页属于旧来源：作废，重建后先走真实页
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Source), m_source);

    // 已在跑：只换页(本地文件内容变了也可用「应用」重触发)。
    if (m_running && m_webview) {
        if (sourceChanged) {
            beginSnapshotRefresh(); // 快照模式：换页后出图再回到静态页
            navigateReal();
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
    // 先拿到桌面挂载点再挂载：g_workerW 为空时 SetParent(hwnd,null) 会把窗口挂成
    // 普通顶层窗口，之后心跳里的「未挂载→重挂」就变成每秒一次的 z 序搅动(桌面闪动)。
    if (!winhelper::ensureWorker()) {
        m_running = false;
        AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Enabled), false);
        publishRuntimeState();
        emit runningChanged(false);
        if (error)
            *error = QStringLiteral("未能获取桌面挂载点，无法挂载网页壁纸");
        return false;
    }
    m_host->show();
    winhelper::mountBehindIcons(m_host, m_host->geometry());

    // 浏览器用户数据目录：登录态/缓存都落在 .cache/web-profile(清缓存可带走)。
    m_creating = true;
    m_releasedForSuspend = false; // 全新管线，旧的「长挂起已释放待重建」作废
    m_createWatchdog->start();
    // 创建回调带代数：stop() 后立刻 start() 时，上一代迟到的回调必须丢弃，
    // 否则新旧两代环境/控制器先后落到成员上(COM 泄漏 + 双管线叠加)。
    const quint32 epoch = m_epoch;
    auto *options = new EnvironmentOptions;
    const HRESULT hr = winhelper::webview2CreateEnvironment(
        CachePaths::webProfile(), options,
        new ComHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                       ICoreWebView2Environment *>(
            [this, epoch](HRESULT code, ICoreWebView2Environment *env) {
                if (epoch != m_epoch) { // 管线已被停止/超时/重建，回调过期
                    if (env)
                        env->Release();
                    return;
                }
                onEnvironmentReady(code, env);
            },
            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler));
    options->Release(); // API 已持自己的引用，调用返回即可归还我们的那份
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
    // 创建在途也要清：滞留的 m_creating 会让下一次 start 走「创建在途」分支
    // 空转返回 true，网页壁纸从此起不来。迟到的创建回调由代数鉴别丢弃。
    m_creating = false;
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
        applog::log(applog::Level::Warning,
                       QStringLiteral("WebView2 环境创建失败 hr=0x%1").arg(uint(code), 8, 16, QChar('0')),
                       QStringLiteral("WebWallpaper"));
        return;
    }
    // 回调移交的引用归本模块所有，本模块析构/重建时统一 Release(见
    // destroyPipeline)。不 AddRef 的话运行时在 Invoke 返回后释放它自己的那份，
    // 之下再调 put_IsVisible 就是 use-after-free(实测段错误就在这一步)。
    m_environment = env;
    env->AddRef();
    const quint32 epoch = m_epoch; // 控制器回调同样按代鉴别
    env->CreateCoreWebView2Controller(
        reinterpret_cast<HWND>(m_host->winId()),
        new ComHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                       ICoreWebView2Controller *>(
            [this, epoch](HRESULT hr, ICoreWebView2Controller *controller) {
                if (epoch != m_epoch) { // 等控制器期间管线已被回收
                    if (controller) {
                        controller->Close();
                        controller->Release();
                    }
                    return;
                }
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
        applog::log(applog::Level::Warning,
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

    applyAudio();
    applyWebView2ChildPriority();
    startSnapshotCycle();

    const bool cachedPageReady = !m_snapshotPageUrl.isEmpty()
        && QFile::exists(snapshotPageFile());
    if (m_refreshMode != Realtime && cachedPageReady) {
        // 上一拍落盘的静态快照页还在缓存(同会话整树重建/同来源恢复)：直接落
        // 稳态，真实页不必先空跑一拍；下一节拍照常刷新。
        navigateSnapshotPage();
    } else {
        beginSnapshotRefresh(); // 快照模式：首图等导航完成后再出图
        navigateReal();
    }
    applog::log(applog::Level::Info,
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
    // 仅展示 = 鼠标穿透，**只靠 WS_EX_TRANSPARENT**。
    //
    // ⚠️ 千万别顺手把 WS_EX_LAYERED 加回来。mountBehindIcons() 特意剥掉了它(见
    // desktopmount.cpp 那段注释)：Win11 的 DWM **不合成跨进程挂载的分层子窗口**，
    // 加上去的表现是「挂载日志一切正常、窗口就是永远不出现在桌面上」—— 极难查。
    // 2026-09-23 踩过：把「网页展示」改成默认档后立刻复现(23:10 那四次挂载全是
    // interactive=0)；翻历史日志才看清所有能用的挂载都是 interactive=1，且唯一一次
    // interactive=0 在 4 秒后就被改回 1。
    //
    // ⚠️ 所以这里**无论哪一档都先把 LAYERED 剥掉**：它一旦被加上，DWM 就不再合成这个
    // 窗口，而且**切回「网页交互」也不会自己恢复** —— 那一档只清 TRANSPARENT，不动 LAYERED。
    const HWND hwnd = reinterpret_cast<HWND>(m_host->winId());
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    ex &= ~LONG(WS_EX_LAYERED);
    if (m_interactive)
        ex &= ~WS_EX_TRANSPARENT;
    else
        ex |= WS_EX_TRANSPARENT;
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
    applyWebView2ChildPriority(); // 换页会拉起新的渲染进程，再降一次级

    if (m_refreshMode != Realtime && m_snapshotShowing && !m_snapshotRefreshing) {
        // 静态快照页落稳。这条完成绝不能触发出图 —— 否则「截→写→导航→
        // 立刻再截」自激励成 2s 一轮的无限循环，快照节拍形同虚设。
        setState(runtimeStateText());
        return;
    }

    if (FAILED(code)) {
        setState(QStringLiteral("页面加载失败，已保持黑屏；请检查地址或网络"));
        // 这一拍的节拍已被消费：失败也要就地重排，否则一次失败就让快照
        // 周期永久停摆(恢复可见后再也不刷新)。
        if (m_refreshMode != Realtime)
            m_snapshotTimer->start(snapshotIntervalMs(m_refreshMode));
        return;
    }
    setState(runtimeStateText());
    if (m_refreshMode != Realtime) {
        // 真实页导航完成(DOM 就绪)：等一拍首帧绘制后出图。首挂与刷新在途
        // 的导航都汇聚到这一条热路径，不再各自排程。
        QTimer::singleShot(kSnapshotWarmupMs, this, &WebWallpaper::doCapturePreview);
    }
#endif
}

// —— 快照模式：稳态停在本地静态快照页(整页一张图，无脚本无动画，空闲渲染
// ≈0，桌面由 Chromium 自己的合成面呈现截图)；刷新 = 回真实页重新渲染 →
// CapturePreview → 改写静态页并导航回去。实时模式整条链路不参与。 ——

void WebWallpaper::startSnapshotCycle()
{
#ifdef Q_OS_WIN
    if (m_refreshMode == Realtime) {
        m_snapshotTimer->stop();
        // 挂起期间(锁定/遮挡/熄屏/电池)不能强行可见 —— 可见性归 applySuspend 管；
        // 快照稳态或挂起可能停在 TrySuspend 冻结态，切回实时必须先 Resume。
        if (m_controller && !m_suspendReasons) {
            if (m_webview3)
                static_cast<ICoreWebView2_3 *>(m_webview3)->Resume();
            m_controller->put_IsVisible(TRUE);
        }
        return;
    }
    // 节拍在 onCapturePreview 里重排(单发定时器)：这保证「上一拍没截好」不会
    // 堆积下一拍。这里只负责排下一拍；首图走 onNavigationCompleted 的热路径。
    m_snapshotTimer->start(snapshotIntervalMs(m_refreshMode));
#endif
}

// 节拍入口：从稳态唤醒出真实页重新渲染，或对已在屏的真实页直接出图。
void WebWallpaper::captureSnapshot()
{
#ifdef Q_OS_WIN
    if (m_shuttingDown || !m_running || !m_webview || m_refreshMode == Realtime)
        return;
    if (m_suspendReasons != 0) {
        // 本来就看不见，这轮不截。单发定时器已消费掉这一拍，必须就地重排
        // —— 否则挂起期间快照节奏停摆，恢复可见后再也不刷新。
        m_snapshotTimer->start(snapshotIntervalMs(m_refreshMode));
        return;
    }
    if (m_webview3)
        static_cast<ICoreWebView2_3 *>(m_webview3)->Resume(); // 挂起态先唤醒(兜底)
    if (m_controller)
        m_controller->put_IsVisible(TRUE);
    if (m_snapshotShowing) {
        // 稳态在静态快照页上：回真实页重新渲染，完成后由 onNavigationCompleted
        // 接力出图(统一走 kSnapshotWarmupMs 热路径)。
        beginSnapshotRefresh();
        navigateReal();
        return;
    }
    // 真实页已在屏(首次挂载/上一拍没截好)：等一拍渲染直接出图。
    QTimer::singleShot(kSnapshotWarmupMs, this, &WebWallpaper::doCapturePreview);
#endif
}

// 出图步：真实页已渲染，发起 CapturePreview，完成回调里落盘并回稳态。
void WebWallpaper::doCapturePreview()
{
#ifdef Q_OS_WIN
    if (m_shuttingDown || !m_running || !m_webview || m_refreshMode == Realtime)
        return;
    // IStream 由 CapturePreview 写入 PNG，完成回调里读回。
    IStream *stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        // 系统级异常拿不到流：重排下一拍，别让节奏就此断掉。
        if (m_running)
            m_snapshotTimer->start(snapshotIntervalMs(m_refreshMode));
        return;
    }
    m_webview->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream,
        new ComHandler1<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, stream](HRESULT code) {
                onCapturePreview(code, stream);
            },
            IID_ICoreWebView2CapturePreviewCompletedHandler));
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
    // 本次出图流程结束(无论成败)：在途标记落地，后续导航分派不再受它影响。
    m_snapshotRefreshing = false;
    // 先排下一拍：截图失败/全黑时保持实时渲染等下一拍重试，绝不把桌面留在
    // 黑屏上 —— 那会被当成「壁纸退出了」。
    if (m_refreshMode != Realtime && m_running)
        m_snapshotTimer->start(snapshotIntervalMs(m_refreshMode));

    // 全黑检测：抽样像素全 0 视为「页面还没画出首帧」，这一帧不能上墙。
    bool blank = !image.isNull();
    if (blank) {
        const int step = qMax(1, qMin(image.width(), image.height()) / 16);
        blank = true;
        for (int y = 0; y < image.height() && blank; y += step)
            for (int x = 0; x < image.width() && blank; x += step)
                if (image.pixel(x, y) != 0xff000000)
                    blank = false;
    }

    if (image.isNull()) {
        applog::log(applog::Level::Warning,
                       QStringLiteral("网页快照截图失败 hr=0x%1").arg(uint(code), 8, 16, QChar('0')),
                       QStringLiteral("WebWallpaper"));
    } else if (blank) {
        applog::log(applog::Level::Info,
                       QStringLiteral("网页快照是全黑(页面未出首帧)，保持实时渲染并等下一拍"),
                       QStringLiteral("WebWallpaper"));
    } else {
        m_snapshot = image;
        if (m_host)
            static_cast<WebHostWidget *>(m_host)->setSnapshot(image);
        // 快照稳态：落盘后导航到静态快照页 —— 整页就是一张图，无脚本无动画，
        // Chromium 空闲渲染≈0，桌面由它自己的合成面呈现截图。不能靠 TrySuspend
        // 省电：契约要求控制器 IsVisible=false 才生效，而隐藏控制器会让宿主
        // GDI 绘制退出 DWM 合成、桌面露出原壁纸(实测；见 WebHostWidget 注释)。
        if (m_refreshMode != Realtime && m_running && m_webview) {
            if (writeSnapshotPage())
                navigateSnapshotPage();
            else
                applog::log(applog::Level::Warning,
                               QStringLiteral("快照静态页写入失败，本拍保持实时渲染"),
                               QStringLiteral("WebWallpaper"));
        }
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

    // 渲染进程会随页面陆续拉起，每 30s 把新出现的 webview 子进程再降一级。
    if (++m_priorityTick >= 30) {
        m_priorityTick = 0;
        applyWebView2ChildPriority();
    }

    int reasons = 0;
    // 全屏/遮挡**不挂起**：网页壁纸原先有个「全屏自动暂停」开关，2026-09-23 连同设置项一起
    // 移除。保持渲染的好处是退出全屏立刻可见，不会出现「退出全屏后壁纸消失很久」的重建空窗。
    // ⚠️ 视频壁纸那边仍有这个开关，别把两边的判据当成一套。
    if (winhelper::isWorkstationLocked())
        reasons |= kSuspendLocked;
    if (!m_monitorOn)
        reasons |= kSuspendMonitorOff;
    if (winhelper::isOnBattery())
        reasons |= kSuspendBattery;
    m_suspendReasons = reasons;

    // 挂载健康检查复用心跳：explorer 重启/窗口失效时重新挂载；宿主原生窗口被
    // 连带销毁时只能整树重建。
    if (m_host) {
        if (!IsWindow(reinterpret_cast<HWND>(m_host->winId()))) {
            applog::log(applog::Level::Info,
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
        if (!winhelper::isWindowMounted(m_host, phys)) {
            // 重挂节流(10s)：WorkerW 短暂不可用/挂载未生效时，每秒重试会把
            // z 序搅得桌面闪动。m_mountFixClock 构造时已启动，首次检查即刻可用。
            if (!m_mountFixClock->isValid() || m_mountFixClock->elapsed() >= 10000) {
                m_mountFixClock->restart();
                applog::log(applog::Level::Info,
                               QStringLiteral("网页壁纸重新挂载到桌面层"),
                               QStringLiteral("WebWallpaper"));
                winhelper::mountBehindIcons(m_host, m_host->geometry());
            }
        }
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
        setState(runtimeStateText());
        publishRuntimeState();
    }

    // 持续挂起超阈值：整树销毁，回桌面时由恢复分支重建。
    if (shouldSuspend && m_suspendClock->isValid()
        && m_suspendClock->elapsed() >= suspendReleaseThresholdMs(reasons)
        && !m_releasedForSuspend) {
        m_releasedForSuspend = true;
        destroyPipeline();
        setState(QStringLiteral("暂停较久，已释放网页壁纸资源；回到桌面自动恢复"));
        applog::log(applog::Level::Info,
                       QStringLiteral("网页壁纸长挂起释放: reasons=0x%1")
                           .arg(reasons, 0, 16),
                       QStringLiteral("WebWallpaper"));
    }
    if (!shouldSuspend) {
        // 长挂起已整树销毁过：原因解除后必须重建管线，否则壁纸就此消失 ——
        // 表现正是「挂了一会就自己没了」。m_releasedForSuspend 只能在重建真正
        // 发起时清掉：先清标志再撞上 5s 节流窗口的话，标志一丢就永远不会再重建。
        const bool rebuildDue = m_running && !m_creating && !m_controller
            && (!m_suspendClock->isValid() || m_suspendClock->elapsed() >= 5000);
        if (m_releasedForSuspend && rebuildDue) {
            m_releasedForSuspend = false;
            applog::log(applog::Level::Info,
                           QStringLiteral("挂起解除，重建网页壁纸管线"),
                           QStringLiteral("WebWallpaper"));
            QString err;
            start(&err);
        }
        m_suspendClock->invalidate();
    }
#endif
}

void WebWallpaper::applySuspend(bool suspend)
{
#ifdef Q_OS_WIN
    m_suspendedByUs = suspend;
    if (!m_controller)
        return;
    if (suspend) {
        // 顺序是契约：TrySuspend 要求 IsVisible=false，否则直接返回
        // ERROR_INVALID_STATE 静默失败。先藏再挂 —— 锁屏/熄屏/遮挡/全屏下
        // 本来就不可见；电池模式下桌面短暂露出原壁纸，换来的是渲染真停
        // (与「已暂停网页壁纸」的状态语义一致)。
        m_controller->put_IsVisible(FALSE);
        suspendNoop();
    } else {
        if (m_webview3)
            static_cast<ICoreWebView2_3 *>(m_webview3)->Resume();
        m_controller->put_IsVisible(TRUE);
    }
#endif
}

void WebWallpaper::destroyPipeline()
{
#ifdef Q_OS_WIN
    // 管线代数推进：在途的环境/控制器创建回调即刻过期(回调侧按代比对)。
    ++m_epoch;
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
        winhelper::unmountWindow(m_host);
        m_host->hide();
        m_host->deleteLater();
        m_host = nullptr;
    }
    m_snapshot = QImage();
    m_suspendedByUs = false;
    m_snapshotShowing = false;
    m_snapshotRefreshing = false;
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
    const bool wasShowing = m_snapshotShowing;
    // 实时分支的 Resume/可见性、快照分支的节拍都由它统一处理(挂起时不动可见性)。
    startSnapshotCycle();
    if (m_running && m_refreshMode == Realtime && wasShowing)
        navigateReal(); // 从快照稳态切回实时：回真实页，别把截图当「实时」挂着
    setState(m_running ? runtimeStateText() : m_stateText);
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

bool WebWallpaper::navigateTo(const QString &source, QString *error)
{
    if (error)
        error->clear();
#ifdef Q_OS_WIN
    if (!m_running) {
        if (error)
            *error = QStringLiteral("网页壁纸尚未运行");
        return false;
    }
    QString requested = source.trimmed();
    if (requested.isEmpty())
        requested = m_source;
    const QString resolved = resolveSource(requested, error);
    if (resolved.isEmpty())
        return false;
    m_source = requested;
    m_resolvedSource = resolved;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Source), m_source);
    if (m_webview) {
        beginSnapshotRefresh(); // 快照模式下换页后重新出图再回稳态
        navigateReal();
        setState(QStringLiteral("切换页面中…"));
    }
    return true;
#else
    if (error)
        *error = QStringLiteral("网页壁纸仅支持 Windows");
    return false;
#endif
}

// —— 杂项 ——

// 运行态状态文本：环境挂起文本由 reasonText 在挂起建立时设置，解除后回到这里。
QString WebWallpaper::runtimeStateText() const
{
    return m_refreshMode == Realtime ? QStringLiteral("网页壁纸运行中")
                                     : QStringLiteral("网页壁纸快照模式运行中");
}

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

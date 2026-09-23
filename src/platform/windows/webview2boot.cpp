#include "platform/windows/webview2boot.h"

#include <QCoreApplication>
#include <string>
#include <QDir>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include "WebView2.h"

namespace fbswin {

namespace {

// 延迟加载：主程序不硬依赖 Loader DLL（它由构建部署到 exe 目录）。句柄进程级缓存。
HMODULE loaderHandle()
{
    static HMODULE handle = []() -> HMODULE {
        const QString path =
            QCoreApplication::applicationDirPath() + QStringLiteral("/WebView2Loader.dll");
        return LoadLibraryW(reinterpret_cast<const wchar_t *>(path.utf16()));
    }();
    return handle;
}

using CreateEnvFn = HRESULT(STDMETHODCALLTYPE *)(
    LPCWSTR browserExecutableFolder, LPCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);
using VersionFn = HRESULT(STDMETHODCALLTYPE *)(LPCWSTR browserExecutableFolder, LPWSTR *version);

CreateEnvFn createEnvFn()
{
    static CreateEnvFn fn = []() -> CreateEnvFn {
        if (HMODULE lib = loaderHandle())
            return reinterpret_cast<CreateEnvFn>(
                GetProcAddress(lib, "CreateCoreWebView2EnvironmentWithOptions"));
        return nullptr;
    }();
    return fn;
}

VersionFn versionFn()
{
    static VersionFn fn = []() -> VersionFn {
        if (HMODULE lib = loaderHandle())
            return reinterpret_cast<VersionFn>(
                GetProcAddress(lib, "GetAvailableCoreWebView2BrowserVersionString"));
        return nullptr;
    }();
    return fn;
}

} // namespace

bool webview2Available(QString *version)
{
    VersionFn fn = versionFn();
    if (!fn) {
        if (version)
            *version = QStringLiteral("缺少 WebView2Loader.dll");
        return false;
    }
    LPWSTR raw = nullptr;
    const HRESULT hr = fn(nullptr, &raw);
    if (FAILED(hr) || !raw) {
        if (version)
            *version = QStringLiteral("系统未安装 WebView2 运行时(0x%1)")
                           .arg(uint(hr), 8, 16, QChar('0'));
        return false;
    }
    if (version)
        *version = QString::fromWCharArray(raw);
    CoTaskMemFree(raw);
    return true;
}

HRESULT webview2CreateEnvironment(const QString &userDataFolder,
                                  ICoreWebView2EnvironmentOptions *options,
                                  ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler)
{
    CreateEnvFn fn = createEnvFn();
    if (!fn)
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    // 完成回调经调用线程的消息循环派发 —— 主程序里就是 GUI 线程的事件循环。
    // CreateCoreWebView2EnvironmentWithOptions 返回前会同步读取路径；这里保留一份
    // 独立字符串，避免临时 Qt 字符串生命周期成为边界风险。
    const std::wstring path = userDataFolder.toStdWString();
    return fn(nullptr, path.c_str(), options, handler);
}

} // namespace fbswin

#else // !Q_OS_WIN

namespace fbswin {
bool webview2Available(QString *version)
{
    if (version)
        *version = QStringLiteral("仅 Windows 支持");
    return false;
}
HRESULT webview2CreateEnvironment(const QString &,
                                  ICoreWebView2EnvironmentOptions *,
                                  ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)
{
    return E_NOTIMPL;
}
} // namespace fbswin

#endif

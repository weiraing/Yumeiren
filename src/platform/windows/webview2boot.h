// WebView2 加载器封装：Loader DLL 在运行期 LoadLibrary，接口全走 COM。
//
// 为什么不链导入库：MinGW 没法直接用 SDK 的 .lib，而导出的两个 C 函数用
// GetProcAddress 拿反而最稳（见 webprobe 探针）。Loader DLL 由构建部署到 exe 目录
// （cmake/yumeiren_deploy_webview2），浏览器运行时本体是系统组件（Edge WebView2
// Runtime），缺失时 available() 返回 false，由上层按「后端不可用」处理。
#ifndef WEBVIEW2BOOT_H
#define WEBVIEW2BOOT_H

#include <QString>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
struct ICoreWebView2EnvironmentOptions;

namespace fbswin {

// 运行时是否可用；可用时 *version 带回浏览器版本号(可传 nullptr)。
bool webview2Available(QString *version);

// 创建环境(异步)。返回值只表示「调用本身」成败(S_OK 也要等完成回调才算就绪)。
// userDataFolder 传绝对路径；options 可为 nullptr。
HRESULT webview2CreateEnvironment(const QString &userDataFolder,
                                  ICoreWebView2EnvironmentOptions *options,
                                  ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);

} // namespace fbswin

#endif // WEBVIEW2BOOT_H

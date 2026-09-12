#include "desktopmount.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QWidget>

#include <windows.h>

// Desktop hosts icons inside SHELLDLL_DefView on a WorkerW window. After
// sending 0x052C to Progman, an extra WorkerW is spawned BEHIND that one; our
// windows are parented to it, so video renders behind the icons but above the
// plain wallpaper.
namespace fbswin {

namespace {

HWND g_workerW = nullptr;

HWND findDefViewHost()
{
    // SHELLDLL_DefView(桌面图标层)通常在 Progman 里，个别系统在某个 WorkerW 里
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman && FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr))
        return progman;
    for (HWND w = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr); w;
         w = FindWindowExW(nullptr, w, L"WorkerW", nullptr))
        if (FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr))
            return w;
    return nullptr;
}

HWND findWorkerW()
{
    // 挂载点优先级：
    //   (1) DefView 宿主之后的全屏顶层 WorkerW —— 经典布局；
    //   (2) Progman 的全屏 WorkerW 子窗口 —— 部分 Win11 构建把 0x052C 生成的
    //       WorkerW 挂在 Progman 下面，顶层枚举根本看不到它；
    //   (3) 找不到就由调用方回落到 Progman 本体。
    // 尺寸校验会拒绝 explorer 顺手的 202x56 迷你 WorkerW：窗口挂进去会被
    // 裁剪到什么都看不见，而各项状态检查还都显示"健康"。
    RECT full = {GetSystemMetrics(SM_XVIRTUALSCREEN),
                 GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    full.right = full.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    full.bottom = full.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const auto coversDesktop = [&](HWND h) {
        RECT r;
        return h && GetWindowRect(h, &r) && r.left <= full.left && r.top <= full.top
               && r.right >= full.right && r.bottom >= full.bottom;
    };

    const HWND host = findDefViewHost();
    if (host) {
        for (HWND w = FindWindowExW(nullptr, host, L"WorkerW", nullptr); w;
             w = FindWindowExW(nullptr, w, L"WorkerW", nullptr))
            if (coversDesktop(w))
                return w;
    }
    const HWND progman = FindWindowW(L"Progman", nullptr);
    for (HWND w = progman ? FindWindowExW(progman, nullptr, L"WorkerW", nullptr)
                          : nullptr;
         w; w = FindWindowExW(progman, w, L"WorkerW", nullptr))
        if (coversDesktop(w))
            return w;
    return nullptr;
}

} // namespace

bool isWorkerValid()
{
    return g_workerW && IsWindow(g_workerW);
}

bool isProgmanFallback()
{
    // FindWindowW 的标题参数传 nullptr 才能匹配无标题窗口
    return g_workerW && g_workerW == FindWindowW(L"Progman", nullptr);
}

bool hasRealWorker()
{
    return isWorkerValid() && !isProgmanFallback();
}

bool ensureWorker()
{
    // explorer 重启后旧 WorkerW 句柄失效，需要重新查找；Progman 兜底 mount
    // 在部分 Win11 构建上 DWM 不合成(壁纸永不显示)，所以兜底状态下每次都
    // 重新尝试找到真正的 WorkerW，找到即迁移。
    if (hasRealWorker())
        return true;
    const bool firstTry = g_workerW == nullptr;
    g_workerW = nullptr;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman)
        return false;
    // The shell spawns the WorkerW asynchronously - poll for it. Bounds are
    // kept tight because this can run on the GUI thread during a health fix;
    // worst case ~3s on the first call, ~0.7s on the throttled re-evaluations.
    const int attempts = firstTry ? 10 : 2;
    for (int attempt = 0; attempt < attempts && !g_workerW; ++attempt) {
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 300, nullptr);
        g_workerW = findWorkerW();
        if (!g_workerW)
            Sleep(50);
    }
    if (!g_workerW)
        g_workerW = progman; // fallback: still renders behind the icons
    return true;
}

void mountBehindIcons(QWidget *window, const QRect &logicalTarget)
{
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const qreal dpr = window->devicePixelRatioF();
    SetParent(hwnd, g_workerW);
    // 挂到 WorkerW 后坐标是相对虚拟桌面原点的物理像素；次屏的 logicalTarget
    // 按其自身 devicePixelRatio 换算才能落到正确的屏幕（此前硬编码 0,0 会把
    // 所有副屏输出叠到主屏左上角）。
    SetWindowPos(hwnd, HWND_BOTTOM,
                 int(logicalTarget.x() * dpr), int(logicalTarget.y() * dpr),
                 int(logicalTarget.width() * dpr), int(logicalTarget.height() * dpr),
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // 关键：去掉 WS_EX_LAYERED。Qt 的 WindowTransparentForInput 会附带
    // layered 样式，而 Win11 的 DWM 不合成跨进程挂载的分层子窗口——窗口
    // 状态一切正常(IsWindowVisible/alpha=255/在播)却永远不出现在桌面上。
    // 点击穿透只依赖 WS_EX_TRANSPARENT，剥离 layered 对透明度无影响。
    const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex & ~LONG(WS_EX_LAYERED));
}

void unmountWindow(QWidget *window)
{
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    SetParent(hwnd, nullptr);
}

bool isWindowMounted(QWidget *window, const QRect &physicalRect)
{
    if (!isWorkerValid())
        return false;
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    RECT r;
    return GetParent(hwnd) == g_workerW && IsWindowVisible(hwnd)
           && GetWindowRect(hwnd, &r)
           && r.left == physicalRect.x() && r.top == physicalRect.y()
           && r.right - r.left == physicalRect.width()
           && r.bottom - r.top == physicalRect.height();
}

bool isForegroundFullscreen()
{
    const HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    // 自家窗口(壁纸 QVideoWidget 是屏幕大小的 Qt::Tool 窗口，可能被系统置为
    // 前台)不构成“全屏应用”，否则壁纸会把自己误判挂起。
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid == GetCurrentProcessId())
        return false;
    RECT r;
    if (!GetWindowRect(fg, &r))
        return false;
    // GetWindowRect 是物理像素；QScreen::geometry() 是逻辑像素，必须按各屏
    // devicePixelRatio 换算后再比较，否则在非 100% 缩放下永远不相等。
    // 宽高即 right-left，不能 +1。比较留 ±2px 容差：DPI 不感知进程创建的
    // 贴边窗口经虚拟化取整常有 ±1px 偏差；普通最大化窗口带 11px 隐形边框
    // 膨胀，不会落入容差内造成误判。
    const QRect wr(r.left, r.top, r.right - r.left, r.bottom - r.top);
    const auto closeEnough = [](int a, int b) { return qAbs(a - b) <= 2; };
    for (QScreen *s : QGuiApplication::screens()) {
        const qreal dpr = s->devicePixelRatio();
        const QRect phys(int(s->geometry().x() * dpr), int(s->geometry().y() * dpr),
                         int(s->geometry().width() * dpr),
                         int(s->geometry().height() * dpr));
        if (closeEnough(wr.x(), phys.x()) && closeEnough(wr.y(), phys.y())
            && closeEnough(wr.width(), phys.width())
            && closeEnough(wr.height(), phys.height()))
            return true;
    }
    return false;
}

bool isWorkstationLocked()
{
    // 锁屏时输入桌面切换到 Winlogon，OpenInputDesktop 会失败
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!desk)
        return true;
    CloseDesktop(desk);
    return false;
}

bool isOnBattery()
{
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s))
        return false;
    if (s.BatteryFlag & 128)
        return false; // 无电池(台式机)
    return s.ACLineStatus == 0;
}

void trimProcessMemory()
{
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
}

} // namespace fbswin

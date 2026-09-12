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

HWND findWorkerW()
{
    HWND worker = nullptr;
    EnumWindows([](HWND top, LPARAM lp) -> BOOL {
        HWND defView = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defView) {
            *reinterpret_cast<HWND *>(lp) =
                FindWindowExW(nullptr, top, L"WorkerW", nullptr);
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&worker));
    return worker;
}

} // namespace

bool isWorkerValid()
{
    return g_workerW && IsWindow(g_workerW);
}

bool ensureWorker()
{
    // explorer 重启后旧 WorkerW 句柄失效，需要重新查找
    if (isWorkerValid())
        return true;
    g_workerW = nullptr;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman)
        return false;
    // The shell spawns the WorkerW asynchronously - poll for it. Bounds are
    // kept tight because this can run on the GUI thread during a health fix;
    // worst case ~3s instead of freezing the UI for half a minute.
    for (int attempt = 0; attempt < 10 && !g_workerW; ++attempt) {
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

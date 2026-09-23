#include "desktopmount.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWidget>

#include <vector>

#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>

// 渲染顺序：图标层 -> 壁纸 WorkerW -> 纯色背景。向 Progman 发 0x052C 会让 shell 额外生成一个位于图标层之后的 WorkerW，挂到它下面即插在图标与纯色之间
namespace winhelper {

namespace {

HWND g_workerW = nullptr;

HWND findDefViewHost()
{
    // DefView 宿主通常在 Progman 里，个别系统在某个 WorkerW 里
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
    // 优先级：图标层之后的全屏顶层 WorkerW；其次 Progman 下的全屏 WorkerW 子窗口(部分 Win11 的 0x052C WorkerW 挂在 Progman 下，顶层枚举看不到)。必须校验尺寸：挂进 explorer 那个 202x56 迷你 WorkerW 会被裁到什么都看不见，状态检查却全"健康"
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
    // 标题参数传 nullptr 才能匹配无标题窗口
    return g_workerW && g_workerW == FindWindowW(L"Progman", nullptr);
}

bool hasRealWorker()
{
    return isWorkerValid() && !isProgmanFallback();
}

bool ensureWorker()
{
    // 兜底(Progman)在部分 Win11 构建上不被 DWM 合成、壁纸永不显示，故落兜底后每次重查真 WorkerW、找到即迁移；explorer 重启使旧句柄失效时同样靠这里重查
    if (hasRealWorker())
        return true;
    const bool firstTry = g_workerW == nullptr;
    g_workerW = nullptr;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman)
        return false;
    // shell 异步生成，需轮询；重试次数刻意压小(本函数跑在 GUI 线程：首次最坏 ~3s，节流后 ~0.7s)
    const int attempts = firstTry ? 10 : 2;
    for (int attempt = 0; attempt < attempts && !g_workerW; ++attempt) {
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 300, nullptr);
        g_workerW = findWorkerW();
        if (!g_workerW)
            Sleep(50);
    }
    if (!g_workerW)
        g_workerW = progman; // 兜底：仍能在图标之后渲染
    return true;
}

void mountBehindIcons(QWidget *window, const QRect &logicalTarget)
{
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const qreal dpr = window->devicePixelRatioF();
    SetParent(hwnd, g_workerW);
    // WorkerW 坐标系是相对虚拟桌面原点的物理像素：logicalTarget 必须按 dpr 换算，否则副屏叠到主屏左上角
    SetWindowPos(hwnd, HWND_BOTTOM,
                 int(logicalTarget.x() * dpr), int(logicalTarget.y() * dpr),
                 int(logicalTarget.width() * dpr), int(logicalTarget.height() * dpr),
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // 必须剥离 WS_EX_LAYERED(Qt 的 WindowTransparentForInput 会带上它)：Win11 的 DWM 不合成跨进程挂载的分层子窗口，状态一切正常却永不出现在桌面上。点击穿透只靠 WS_EX_TRANSPARENT
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
    // 必须用 GetAncestor(GA_PARENT) 而非 GetParent()：壁纸窗口带 WS_POPUP，GetParent 返回的是 owner(我们为空)而非父窗口 → 恒 NULL，本函数永不成立 → mountIsStale() 恒真 → 每 10 秒重挂一次，重挂会让窗口短暂移出 DWM 合成，用户看到壁纸每 11 秒闪一下
    return GetAncestor(hwnd, GA_PARENT) == g_workerW && IsWindowVisible(hwnd)
           && GetWindowRect(hwnd, &r)
           && r.left == physicalRect.x() && r.top == physicalRect.y()
           && r.right - r.left == physicalRect.width()
           && r.bottom - r.top == physicalRect.height();
}

bool isSelfOrShellProcess(HWND hwnd)
{
    // 自家进程与 explorer 桌面层(Progman/WorkerW/DefView 都是全屏矩形)不算遮挡，否则看着桌面也会停播
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId())
        return true;
    wchar_t imagePath[MAX_PATH] = {};
    DWORD pathLen = MAX_PATH;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        QueryFullProcessImageNameW(proc, 0, imagePath, &pathLen);
        CloseHandle(proc);
    }
    const wchar_t *base = imagePath;
    for (const wchar_t *p = imagePath; *p; ++p)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    return _wcsicmp(base, L"explorer.exe") == 0;
}

namespace {

// cloaked = DWM 没在合成它，屏幕上根本没这个窗口(别的虚拟桌面上的窗口、被挂起的 UWP)；不排除会"被桌面 2 的全屏应用停掉桌面 1 的壁纸"
bool isDwmCloaked(HWND hwnd)
{
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                           sizeof(cloaked)))
           && cloaked != FALSE;
}

// 在可见顶层窗口里找盖住 target(物理像素)的窗口：exactFit=true 要求逐边贴合(全屏)，false 只需包含(遮挡)。判据刻意与"谁在前台"无关：全屏应用前面压个小窗口时桌面依然不可见
HWND findCoveringWindow(const RECT &target, bool exactFit)
{
    struct Ctx
    {
        const RECT *target;
        bool exactFit;
        HWND hit;
    } ctx{&target, exactFit, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto *c = reinterpret_cast<Ctx *>(lp);
            // 最小化窗口在 WS_VISIBLE 意义上仍"可见"，先按状态剔掉
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
                return TRUE;
            RECT r;
            if (!GetWindowRect(hwnd, &r))
                return TRUE;
            const RECT &t = *c->target;
            // 别把这个 lambda 叫 near —— windef.h 里 near/far 是空宏
            const auto closeEnough = [](LONG a, LONG b) {
                return qAbs(int(a - b)) <= 2;
            };
            if (c->exactFit) {
                // 宽高即 right-left，不能 +1；±2px 容差给 DPI 不感知进程的贴边窗口。最大化窗口带 11px 隐形边框外扩，不会误判成全屏
                if (!closeEnough(r.left, t.left) || !closeEnough(r.top, t.top)
                    || !closeEnough(r.right - r.left, t.right - t.left)
                    || !closeEnough(r.bottom - r.top, t.bottom - t.top))
                    return TRUE;
            } else if (r.left > t.left || r.top > t.top || r.right < t.right
                       || r.bottom < t.bottom) {
                return TRUE;
            }
            // 矩形对上后才做这两项较贵的检查
            if (isSelfOrShellProcess(hwnd) || isDwmCloaked(hwnd))
                return TRUE;
            c->hit = hwnd;
            return FALSE; // 找到即停
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.hit;
}

} // namespace

bool isFullscreenWindowPresent()
{
    // 只认前台窗口所在那块屏，免得多屏时副屏全屏应用把主屏壁纸一起停掉
    const HWND fg = GetForegroundWindow();
    HMONITOR mon = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!mon) {
        // 没有前台窗口(刚切到桌面/锁屏前)：退回主屏
        POINT origin{0, 0};
        mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi))
        return false;
    // rcMonitor 与 GetWindowRect 同为物理像素，无需 QScreen 的 dpr 换算，也就没有取整误差
    return findCoveringWindow(mi.rcMonitor, true) != nullptr;
}

bool isDesktopCoveredByWindow()
{
    RECT wa;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
        return false;
    // SPI_GETWORKAREA 按物理像素返回；盖满主屏工作区即桌面完全不可见(最大化窗口带边框外扩，恰好落进包含关系)。工作区只有主屏一个
    return findCoveringWindow(wa, false) != nullptr;
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

// 按 RelationProcessorCore 反查物理核分组，每组取编号最小的逻辑处理器，凑够 maxCores 个；返回 0 表示不可用，调用方回退到"前 N 个逻辑号"
static DWORD_PTR distinctCoreAffinity(int maxCores)
{
    DWORD_PTR procMask = 0;
    DWORD_PTR sysMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) || !procMask)
        return 0;

    DWORD bytes = 0;
    if (GetLogicalProcessorInformation(nullptr, &bytes)
        || GetLastError() != ERROR_INSUFFICIENT_BUFFER || !bytes)
        return 0;
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION>
        info(bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetLogicalProcessorInformation(info.data(), &bytes))
        return 0;

    DWORD_PTR picked = 0;
    int cores = 0;
    for (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION &e : info) {
        if (e.Relationship != RelationProcessorCore)
            continue;
        const DWORD_PTR inCore = DWORD_PTR(e.ProcessorMask) & procMask & ~picked;
        if (!inCore)
            continue; // 该物理核剩余的可用逻辑号已选过
        picked |= inCore & (~inCore + 1); // 取该核里编号最小的逻辑处理器
        if (++cores >= maxCores)
            break;
    }
    return cores >= maxCores ? picked : 0;
}

bool applyProcessAffinityLimit(int maxCores)
{
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors <= static_cast<DWORD>(maxCores))
        return false; // 核数本就不多，不限制
    // 取"每物理核一个逻辑号"的掩码而非前 N 个逻辑号：SMT 兄弟核相邻成对枚举(0/1 同核)，前 N 个逻辑号会把解码线程挤在少数物理核上互抢执行端口。逻辑核总数不变，吞吐近一倍
    const DWORD_PTR distinct = distinctCoreAffinity(maxCores);
    const DWORD_PTR mask = distinct ? distinct : ((1ULL << maxCores) - 1);
    return SetProcessAffinityMask(GetCurrentProcess(), mask) != FALSE;
}

bool acquireSingleInstanceLock()
{
    // Local\ 前缀 = 每会话命名空间。进程退出(含强杀/崩溃)时内核自动释放互斥锁，不留残段
    HANDLE m = CreateMutexW(nullptr, TRUE, L"Local\\Yumeiren.single-instance");
    if (!m)
        return true; // 极罕见的创建失败不阻止启动
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return false;
    }
    // 故意不关闭句柄：锁的生命周期 = 本进程生命周期
    return true;
}

bool activateExistingInstanceWindow(const QString &mainWindowTitle, QString *reason)
{
    auto fail = [reason](const QString &r) {
        if (reason)
            *reason = r;
        return false;
    };
    // 1) 找本产品任一二进制的已运行实例(排除本进程)：互斥锁全产品共享，只按自身 exe 名找会漏配
    const QString selfExe = [] {
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return QString::fromWCharArray(buf);
    }();
    const QString selfName = selfExe.section(QLatin1Char('\\'), -1).toLower();
    const QStringList productExes = { QStringLiteral("yumeiren.exe"),
                                      QStringLiteral("yumeirentest.exe") };
    Q_UNUSED(selfName);
    DWORD targetPid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                const QString exe =
                    QString::fromWCharArray(pe.szExeFile).toLower();
                if (productExes.contains(exe)
                    && pe.th32ProcessID != GetCurrentProcessId()) {
                    targetPid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (!targetPid)
        return fail(QStringLiteral("未找到已运行的产品进程"));

    // 2) 该实例的顶层主窗口：标题精确匹配。刻意不要求 IsWindowVisible —— 隐藏到托盘的窗口仍在，当成"没有实例"会让二次启动只见弹窗、窗口永远叫不醒
    const std::wstring wantTitle = mainWindowTitle.toStdWString();
    struct Ctx { DWORD pid; const std::wstring *title; HWND main; long area; bool visible; } ctx{
        targetPid, &wantTitle, nullptr, 0, false};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c->pid)
            return TRUE;
        if (GetAncestor(hwnd, GA_ROOT) != hwnd)
            return TRUE;
        wchar_t title[128] = {};
        GetWindowTextW(hwnd, title, 128);
        if (*c->title == title) {
            // 可见优先、其次面积最大：兜底 MessageBox 标题与应用名相同，主窗口远大于它，免误中
            RECT r;
            if (!GetWindowRect(hwnd, &r))
                return TRUE;
            const long area = (r.right - r.left) * (r.bottom - r.top);
            const bool vis = IsWindowVisible(hwnd) != FALSE;
            if (!c->main || (vis && !c->visible)
                || (vis == c->visible && area > c->area)) {
                c->main = hwnd;
                c->area = area;
                c->visible = vis;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.main)
        return fail(QStringLiteral("已运行实例(pid=%1)未找到标题匹配的主窗口").arg(targetPid));

    if (!ctx.visible) {
        // 隐藏到托盘：不能直接 ShowWindow(Qt 状态会失真)，让已运行实例自己唤醒
        const unsigned int showMsg = showMainWindowMessage();
        if (!showMsg || !PostMessageW(ctx.main, showMsg, 0, 0))
            return fail(QStringLiteral("已运行实例(pid=%1)在托盘中，但唤醒消息发送失败")
                            .arg(targetPid));
        if (reason)
            *reason = QStringLiteral("已请求从托盘恢复");
        return true;
    }

    // 3) 最小化则还原；置顶/取消置顶各一拍以绕过前台锁
    if (IsIconic(ctx.main))
        ShowWindow(ctx.main, SW_RESTORE);
    SetWindowPos(ctx.main, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(ctx.main, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(ctx.main);
    if (reason)
        *reason = QStringLiteral("已激活");
    return true;
}

unsigned int showMainWindowMessage()
{
    // 注册名固定，跨进程拿到同一消息号；失败返回 0
    static const unsigned int msg = static_cast<unsigned int>(
        RegisterWindowMessageW(L"Yumeiren.ShowMainWindow"));
    return msg;
}

void trimProcessMemory()
{
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
}

} // namespace winhelper

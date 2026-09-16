#include "desktopmount.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QWidget>

#include <windows.h>
#include <tlhelp32.h>
#include <vector>

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

bool isSelfOrShellProcess(HWND hwnd)
{
    // 自家进程与 explorer(Progman/WorkerW/DefView 等桌面层都是全屏矩形)都不算
    // "前台全屏/遮挡"——用户看着桌面时壁纸必须照常播放
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

bool isForegroundFullscreen()
{
    const HWND fg = GetForegroundWindow();
    if (!fg || isSelfOrShellProcess(fg))
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

bool isDesktopCovered()
{
    const HWND fg = GetForegroundWindow();
    if (!fg || isSelfOrShellProcess(fg))
        return false;

    RECT r;
    if (!GetWindowRect(fg, &r))
        return false;
    RECT wa;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
        return false;
    // SPI_GETWORKAREA 在本进程按物理像素返回；前台窗口盖满主屏工作区即视为
    // 桌面被完全遮挡(最大化普通窗口带边框外扩，恰好落进包含关系)
    return r.left <= wa.left && r.top <= wa.top && r.right >= wa.right
           && r.bottom >= wa.bottom;
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

// 用 RelationProcessorCore 的 ProcessorMask 反查物理核分组，每组取编号最小的
// 逻辑处理器，凑够 maxCores 个物理核。返回 0 表示不可用(API 失败、掩码为空等)，
// 由调用方回退到「前 N 个逻辑号」。结果一定是当前进程亲和性掩码的子集。
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
    // 挑「每个物理核只占一个逻辑号」的掩码，而不是直接取前 N 个逻辑号。
    // 本机(锐龙 16C32T)的 SMT 兄弟核相邻成对枚举：0/1 同属一个物理核，
    // 旧掩码 0b1111 只有 2 个物理核 + 2 个超线程，解码线程全挤在两个核上
    // 互抢执行端口与 L1/L2。逻辑核个数不变，QThread::idealThreadCount 仍是 N，
    // 既保留原有降线程/降内存收益，又拿到接近一倍的真实吞吐。
    const DWORD_PTR distinct = distinctCoreAffinity(maxCores);
    const DWORD_PTR mask = distinct ? distinct : ((1ULL << maxCores) - 1);
    return SetProcessAffinityMask(GetCurrentProcess(), mask) != FALSE;
}

bool acquireSingleInstanceLock()
{
    // Local\ 前缀=每会话命名空间(同登录会话内唯一)。持有句柄的进程退出时，
    // 内核自动销毁互斥锁——强杀/崩溃都不会留下残段。
    HANDLE m = CreateMutexW(nullptr, TRUE, L"Local\\Yumeiren.single-instance");
    if (!m)
        return true; // 极罕见的创建失败不阻止启动
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return false;
    }
    // 故意不关闭句柄：锁的生命周期=本进程生命周期
    return true;
}

bool activateExistingInstanceWindow(const QString &mainWindowTitle, QString *reason)
{
    auto fail = [reason](const QString &r) {
        if (reason)
            *reason = r;
        return false;
    };
    // 1) 找到本产品任一二进制的已运行实例(排除本进程)。互斥锁是全产品共享的
    //    (Yumeiren.single-instance)，运行中的可能是 Yumeiren.exe 也可能是
    //    YumeirenTest.exe——只按自身 exe 名找会漏配(实测复现：跨 exe 二次启动弹"已在运行")。
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

    // 2) 该实例的顶层主窗口：标题精确匹配(壁纸窗口无标题，不会误中)。
    //    这里不能要求 IsWindowVisible —— 隐藏到托盘后窗口还在，只是不可见，
    //    一旦把这种状态当成「没有实例」，用户二次启动就会看到
    //    「虞美人已经在运行」弹窗而窗口永远叫不醒。
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
            // 可见窗口优先，其次取面积最大：兜底弹窗(MessageBox)标题与应用名相同，
            // 主窗口(990x780)远大于它，避免旧弹窗残留在场时误中
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
        // 隐藏到托盘：让已运行实例自己唤醒(见 showMainWindowMessage 注释)。
        const unsigned int showMsg = showMainWindowMessage();
        if (!showMsg || !PostMessageW(ctx.main, showMsg, 0, 0))
            return fail(QStringLiteral("已运行实例(pid=%1)在托盘中，但唤醒消息发送失败")
                            .arg(targetPid));
        if (reason)
            *reason = QStringLiteral("已请求从托盘恢复");
        return true;
    }

    // 3) 最小化则还原；置顶一拍再还原以绕过前台锁(本实例由用户点击启动，
    //    本身具备前台激活权限，双保险)
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
    // 注册名固定即可，跨进程拿到的是同一个消息号；注册失败返回 0，调用方兜底。
    static const unsigned int msg = static_cast<unsigned int>(
        RegisterWindowMessageW(L"Yumeiren.ShowMainWindow"));
    return msg;
}

void trimProcessMemory()
{
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
}

} // namespace fbswin

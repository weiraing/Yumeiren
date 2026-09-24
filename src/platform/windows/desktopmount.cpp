#include "desktopmount.h"

#include <QFileInfo>
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

// 常驻的「唤起收信窗口」（见 winhelper::WakeupListener，独立线程上自建窗口）。
//
// ⚠️ 这个窗口**不在 GUI 线程、也不是 message-only**：两种做法实测在**主窗口 hide() 后
//    一起被销毁**（`IsWindow(hwnd) == 0`、`FindWindowW` 归 0），跨进程就找不到投递目标。
//    最终方案是独立线程 + 自有消息循环，窗口生死只跟进程绑定。
//
// ⚠️ 它仍然**不被 EnumWindows 列出**（不可见 + 0×0 顶层窗口，桌面窗口树里没有），
//    所以只能走 `FindWindowW(类名)` 直接命中。类名是与 wakeuplistener.cpp 的**契约**，
//    改一处要改两处。
HWND findWakeupListener(DWORD pid)
{
    HWND w = FindWindowW(L"Yumeiren.WakeupListener", nullptr);
    if (!w)
        return nullptr;
    DWORD wpid = 0;
    GetWindowThreadProcessId(w, &wpid);
    // 多实例场景下类名可能撞车，认领前必须核对 pid
    return wpid == pid ? w : nullptr;
}

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
    const int px = int(logicalTarget.x() * dpr);
    const int py = int(logicalTarget.y() * dpr);
    const int pw = int(logicalTarget.width() * dpr);
    const int ph = int(logicalTarget.height() * dpr);

    // 必须剥离 WS_EX_LAYERED(Qt 的 WindowTransparentForInput 会带上它)：Win11 的 DWM 不合成跨进程挂载的分层子窗口，状态一切正常却永不出现在桌面上
    const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE) & ~LONG(WS_EX_LAYERED);

    // 挂成 WorkerW 的子窗口：桌面最稳的一档，图标正常、绝对不抢鼠标。
    SetParent(hwnd, g_workerW);
    // WorkerW 坐标系是相对虚拟桌面原点的物理像素：logicalTarget 必须按 dpr 换算，否则副屏叠到主屏左上角
    SetWindowPos(hwnd, HWND_BOTTOM, px, py, pw, ph,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
}

void unmountWindow(QWidget *window)
{
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    SetParent(hwnd, nullptr);
}

bool isWindowMounted(QWidget *window, const QRect &physicalRect)
{
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    RECT r;

    if (!isWorkerValid())
        return false;
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

// 工具窗口 + 分层：铺满全屏却肉眼看不见的系统浮层，最典型是触摸键盘的手写画布
// (TabTip.exe 的 ShellHandwritingCanvas，本机实测恒为可见、恒不 cloaked、矩形恒铺满显示器)。
// 这类窗口既非最小化也无 cloaked 可剔除，混进遮挡判据会让壁纸「一启动就自动暂停」。
// 用两个标志相与而不是单看 WS_EX_TOOLWINDOW：真正的全屏应用(游戏/播放器)不会同时
// 置 WS_EX_LAYERED，单看工具位会漏判它们。
bool isInvisibleOverlay(HWND hwnd)
{
    const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    return (ex & WS_EX_TOOLWINDOW) && (ex & WS_EX_LAYERED);
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
            if (isSelfOrShellProcess(hwnd) || isDwmCloaked(hwnd)
                || isInvisibleOverlay(hwnd))
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
    // 1) 找本产品的已运行实例(排除本进程)：互斥锁全产品共享，所以判据**不能是 exe 文件名**
    //    —— 本产品的二进制有多个名字(虞美人.exe / YumeirenTest.exe / 开发时的 rename)。
    //    曾因此硬编码 "yumeiren.exe"/"yumeirentest.exe"，而正式版叫**虞美人.exe**，
    //    于是每次都报「未找到已运行的产品进程」→ 弹「虞美人已经在运行」而不是唤起窗口。
    //    改用**同目录**判定：本产品的所有二进制都从同一个构建目录启动。
    const QString selfExe = [] {
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return QString::fromWCharArray(buf);
    }();
    const QString selfDir = QFileInfo(selfExe).absolutePath().toLower();
    const QString selfName = QFileInfo(selfExe).fileName().toLower();
    DWORD targetPid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == GetCurrentProcessId())
                    continue;
                // 只认 .exe，别把同目录的 dll/exe 辅助进程算进来
                const QString exe = QString::fromWCharArray(pe.szExeFile).toLower();
                if (!exe.endsWith(QStringLiteral(".exe")))
                    continue;
                // 用进程路径（而不是仅文件名）比目录：同名 exe 在别处跑的不算
                HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                          pe.th32ProcessID);
                if (!proc)
                    continue;
                wchar_t path[MAX_PATH] = {};
                DWORD len = MAX_PATH;
                const bool ok = QueryFullProcessImageNameW(proc, 0, path, &len);
                CloseHandle(proc);
                if (!ok)
                    continue;
                const QString otherDir =
                    QFileInfo(QString::fromWCharArray(path)).absolutePath().toLower();
                if (otherDir == selfDir) {
                    targetPid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    Q_UNUSED(selfName);
    if (!targetPid)
        return fail(QStringLiteral("未找到已运行的产品进程"));

    // 2) 该实例的顶层主窗口：标题精确匹配 + **类名限定**。刻意不要求 IsWindowVisible
    //    —— 隐藏到托盘的窗口仍在，当成"没有实例"会让二次启动只见弹窗、窗口永远叫不醒。
    //
    // ⚠️ 类名限定是必须的（2026-09-24 踩过）：主窗口 hide() 时 Qt 会额外建一个**可见的**
    //    顶层辅助窗口 `QtXXXXQWindowToolSaveBitsOwnDC`，标题同样是 `Yumeiren`、面积还不小。
    //    只按"可见优先 + 面积最大"挑，就会选中这个辅助窗口 → `ctx.visible` 为真 →
    //    走同步激活分支去 SetForegroundWindow 一个不是主窗口的东西 → 主窗口纹丝不动，
    //    二次启动却报"已激活"。表现就是"日志说唤起了、窗口没出来"。
    //    Qt 主窗口的类名固定是 `Qt<版本>QWindowIcon`，用前缀+后缀匹配，别写死版本号。
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
        const auto isMainClass = [hwnd] {
            wchar_t cls[128] = {};
            GetClassNameW(hwnd, cls, 128);
            const std::wstring c(cls);
            // `Qt<版本>QWindowIcon`（本工程是 C++17，没有 wstring::ends_with）
            const std::wstring suffix = L"QWindowIcon";
            return c.size() > suffix.size() && c.rfind(L"Qt", 0) == 0
                   && c.compare(c.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        if (!isMainClass())
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
    if (!ctx.main || !ctx.visible) {
        // 主窗口隐藏（或压根找不到句柄）时，**统一改走常驻收信口**：
        //   - hide() 后 Qt 主窗口的原生 HWND 虽然还活着（`IsWindow=1`），但 `vis=0`，
        //     直接 ShowWindow/SetForegroundWindow 会跟 Qt 内部状态打架（Qt 仍以为它是
        //     隐藏的，后续 show() 变 no-op）。让已运行实例自己走 showFromTray() 最干净。
        //   - 完全找不到主窗口的情况（实测在部分时序下 EnumWindows 扫不到）也走这里。
        // 2026-09-24：这正是用户报「再打开软件弹框、窗口叫不出来」的根因 ——
        // 旧代码在这里 fail()，于是走了弹 MessageBox 的兜底分支。
        const unsigned int showMsg = showMainWindowMessage();
        if (!showMsg)
            return fail(QStringLiteral("唤起消息注册失败"));
        HWND listener = findWakeupListener(targetPid);
        if (!listener)
            return fail(QStringLiteral("已运行实例(pid=%1)隐藏中，且未找到收信窗口")
                            .arg(targetPid));
        // ⚠️ 必须用 PostMessageW（异步、立即返回），**不能**用 SendMessageTimeoutW +
        //    HWND_BROADCAST —— 那条路会同步等所有顶层窗口应答，实测把二次启动卡死
        //    在单实例守卫里（日志停在守卫那一行之后就没有下文了）。
        if (!PostMessageW(listener, showMsg, 0, 0))
            return fail(QStringLiteral("已运行实例(pid=%1)隐藏中，投递唤醒消息失败")
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

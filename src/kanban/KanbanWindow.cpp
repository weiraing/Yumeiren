// 看板娘窗口实现。
#include "kanban/KanbanWindow.h"

#include "core/Diagnostics.h"
#include "kanban/KanbanRenderer.h"
#include "kanban/KanbanSoftwareView.h"
#ifdef YUMEIREN_WITH_LIVE2D
#include "kanban/KanbanOpenGLView.h"
#endif

#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace kanban {

// 原生亚克力毛玻璃：未公开 API，失败就返回 false，调用方退化成「只有半透明底色」。
// tintAbgr 是 0xAABBGGRR（注意通道顺序，不是常见的 ARGB），alpha 即「玻璃效果度」。
//
// ⚠️⚠️ `SetWindowCompositionAttribute` 的第二个参数**不是** `AccentPolicy *`，而是
// `WINCOMPATTRDATA *`：属性号 + 数据指针 + 数据长度。第一版把 `AccentPolicy *` 直接
// 传进去，函数于是把我们的 `state=4` 当成属性号、`flags=2` 当成数据指针 —— 要么去解
// 引用地址 0x2，要么当场拒绝，两种下场都是返回 FALSE。表现是「玻璃效果度怎么拖都没
// 反应」，而日志里只有一句「系统不支持」，很容易误判成用户系统太老。
// （2026-09-23 翻用户日志发现每次弹菜单都打这一句，才回头查到签名。）
#ifdef Q_OS_WIN
namespace {

struct AccentPolicy
{
    UINT state;          // 0=关, 4=ACCENT_ENABLE_ACRYLICBEHIND
    UINT flags;
    UINT gradientColor;  // 0xAABBGGRR
    UINT animationId;
};

struct WinCompAttrData
{
    DWORD attribute;     // 19 = WCA_ACCENT_POLICY
    PVOID data;
    ULONG dataSize;
};

} // namespace
#endif

bool applyWindowAcrylic(WId windowId, unsigned tintAbgr)
{
#ifdef Q_OS_WIN
    const auto hwnd = reinterpret_cast<HWND>(windowId);
    if (!hwnd)
        return false;
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
        return false;
    using Fn = BOOL (WINAPI *)(HWND, WinCompAttrData *);
    const auto fn = reinterpret_cast<Fn>(
        GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (!fn)
        return false;

    AccentPolicy policy{};
    policy.state = tintAbgr != 0 ? 4 /*ACCENT_ENABLE_ACRYLICBEHIND*/ : 0 /*ACCENT_DISABLED*/;
    // ⚠️⚠️ flags 必须是 0，不能是 2 —— 这一格是「玻璃效果不起效」的真凶。
    // flags=2 时亚克力会变成一层**几乎不透明的膜**：实测把整屏铺成纯黑/纯白/纯红，
    // 菜单内部只跟着背景动 2~10%；flags=0 才是真正的半透明磨砂，背景透出 60%+。
    // 两者的 tint 响应曲线都是线性的、只是斜率差 8 倍 —— 光看截图会误判成
    // 「tint 调得不够淡」，其实是 flags 这一位。取证脚本 tmp/_acr_backdrop.py
    // （换底色看菜单内部跟不跟着变）。2026-09-23 定位。
    policy.flags = 0;
    policy.gradientColor = tintAbgr;
    policy.animationId = 0;

    WinCompAttrData data{};
    data.attribute = 19; // WCA_ACCENT_POLICY
    data.data = &policy;
    data.dataSize = sizeof(policy);
    return fn(hwnd, &data) != FALSE;
#else
    Q_UNUSED(windowId)
    Q_UNUSED(tintAbgr)
    return false;
#endif
}

// 把窗口整个客户区变成「一块玻璃板」：客户区按自身 alpha 与桌面合成，**不加模糊**。
//
// ⚠️ 为什么玻璃关着也必须挂点什么：QMenu 的原生窗口
// （`cls='Qt6102QWindowPopupDropShadowSaveBits'`）**不是分层窗口**（exstyle 里没有
// `WS_EX_LAYERED`）—— `WA_TranslucentBackground` 并没有把它变成分层窗口。
// 于是 QSS 里 `QMenu { background: #AARRGGBB }` 的 alpha **只能把颜色朝黑色稀释**，
// 桌面透不出来。实测 `tr=80, gl=0` 时菜单内部在纯黑底和纯白底上是**同一个值** 48.3
// （= 0.2×239），一眼可见是跟黑色混的。挂上玻璃板之后同一档位能真透出桌面。
//
// ⚠️ 为什么不用 `AccentPolicy.state = 6`：实测 state=6（枚举里的非法值
// ACCENT_INVALID_STATE）与 DwmExtendFrameIntoClientArea **逐像素完全一致**
// （bgAlpha=0 时两者都是 m=99.3%、棋盘跳变 177.7‰），但 6 是非法枚举 ——
// 靠它等于赌微软的实现细节，哪天补上校验就静默失效。`DwmExtendFrameIntoClientArea`
// 是有文档的 API，同样是「透出且不模糊」。
//
// 取证：`tmp/_acr_live.py`（从外部进程对菜单 HWND 施加手法再量像素）、
// `tmp/_acr_statescan.py`（扫 state 0..6）。判据要看**菜单中部一行的灰度剖面**：
// 玻璃板下是 `245 245 153 153 245 245 …`（棋盘清晰穿过来），不挂则是平的 `153 153 …`。
// ⚠️ 别只看「跳变‰」—— 那个指标在低对比度下会失灵（bgAlpha=163 时同一张图只剩 2.8‰，
// 但画面明明一样锐利）。2026-09-23 定位。
bool applyGlassSheet(WId windowId)
{
#ifdef Q_OS_WIN
    const auto hwnd = reinterpret_cast<HWND>(windowId);
    if (!hwnd)
        return false;
    // -1 = 「整块客户区都是玻璃」，Windows 的约定写法。
    MARGINS margins{-1, -1, -1, -1};
    return SUCCEEDED(DwmExtendFrameIntoClientArea(hwnd, &margins));
#else
    Q_UNUSED(windowId)
    return false;
#endif
}

namespace {

constexpr const char *kModule = "KanbanWindow";
constexpr int kDragThresholdPx = 4;   // 超过这个位移算拖动，不算点击
constexpr int kScreenMarginPx = 24;   // 首次落位离屏幕右/下边的留白
constexpr int kMinWindowEdgePx = 120; // 缩放下限：防止滚轮把窗口搓没
// 菜单透明度上限。与 KanbanControllerSettings.cpp 的 kMaxMenuTransparencyPercent 同值：
// 那边管配置读写的钳制，这边管窗口自身的钳制，两处都写死是因为它们分属不同层，
// 互相 include 反而把依赖搞乱。改了记得同步。
constexpr int kMaxMenuTransparencyPercent = 80;

// 矩形是否至少有一部分落在某块屏幕的可用区里：识别「上次存的位置已不在任何屏幕上」。
bool isOnAnyScreen(const QRect &rect)
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (screen && screen->availableGeometry().intersects(rect)) {
            return true;
        }
    }
    return false;
}

// 原 `installOpacityCorrection()` 已删：那条 25ms 轮询机制是为了压过
// `QMenu::popup()` 的渐变回弹（tmp/_t1 / _t2 实测），现在 applyMenuStyle 改走
// QSS 背景 alpha —— 一次性设好、弹出即最终，跳变自然没了。用户的需求
// 「透明度只调背景、不调文字」也是 QSS 路线才能做到（setWindowOpacity 作用于
// 合成层，文字会被一起拉透）。想追溯这条路时看 MEMORY-kanban.md 「右键菜单外观」。
} // namespace

KanbanWindow::KanbanWindow(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("KanbanWindow"));
    // 无边框 + 逐像素透明 + 不进任务栏 + 不抢焦点。
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

#ifdef Q_OS_WIN
    // 资源管理器重启会丢样式，订阅 TaskbarCreated 广播重贴置顶/穿透。
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
#endif

    videodiag::logObjectEvent("create", this, QStringLiteral("kanban window"));
}

KanbanWindow::~KanbanWindow()
{
    videodiag::logObjectEvent("destroy", this, QStringLiteral("kanban window"));
}

// —— 视图选型 ——

void KanbanWindow::attachRenderer(KanbanRenderer *renderer)
{
    m_renderer = renderer;
    const bool wantGl = m_renderer && m_renderer->usesOpenGL();
#ifndef YUMEIREN_WITH_LIVE2D
    // 本构建未接入 SDK 时 GL 渲染器不会被选中；真走到这里说明接线错了，留日志而非静默黑屏。
    if (wantGl) {
        videodiag::log(videodiag::Level::Error,
                       QStringLiteral("[KanbanWindow] 渲染器要求 GL 宿主，但本构建未编译 GL 视图"),
                       QLatin1String(kModule));
    }
#endif
    if (wantGl != m_glHost || !m_view) {
        m_glHost = wantGl;
        ensureView();
    } else if (m_renderer) {
        // 同类视图换渲染器：只换指针并同步尺寸。
        if (auto *sw = qobject_cast<KanbanSoftwareView *>(m_view)) {
            sw->setRenderer(m_renderer);
        }
#ifdef YUMEIREN_WITH_LIVE2D
        else if (auto *gl = qobject_cast<KanbanOpenGLView *>(m_view)) {
            gl->setRenderer(m_renderer);
        }
#endif
    }
    // 视图可能刚被 ensureView() 重建，宿主指针统一在此交回渲染器。
    m_renderer->setGlHost(dynamic_cast<KanbanGlHost *>(m_view));
}

void KanbanWindow::detachRenderer()
{
    // 视图必须一起松手：控制器随后立即 m_renderer.reset()，而窗口只是排期删除、子视图
    // 还活着 —— 视图再来一次 paintGL 或析构时都会踩到已释放内存(实测表现为点「取消」
    // 后进程 0xC0000005 退出)。
    releaseViewRenderer();
    m_renderer = nullptr;
}

// 视图与窗口对渲染器的引用必须同生共死，漏一处剩下的那份就是野指针。
void KanbanWindow::releaseViewRenderer()
{
    if (!m_view) {
        return;
    }
    if (auto *sw = qobject_cast<KanbanSoftwareView *>(m_view)) {
        sw->setRenderer(nullptr);
    }
#ifdef YUMEIREN_WITH_LIVE2D
    else if (auto *gl = qobject_cast<KanbanOpenGLView *>(m_view)) {
        gl->setRenderer(nullptr);
    }
#endif
}

void KanbanWindow::ensureView()
{
    if (m_view) {
        // 先摘出布局再删：布局持有指针，直接 delete 会让布局短暂指空。
        static_cast<QVBoxLayout *>(layout())->removeWidget(m_view);
        // deleteLater 后旧视图还能活到本轮事件循环结束，期间若来了一次 paintGL，它手里
        // 的渲染器可能已被销毁，所以先松手。
        releaseViewRenderer();
        m_view->deleteLater();
        m_view = nullptr;
    }

#ifdef YUMEIREN_WITH_LIVE2D
    if (m_glHost) {
        auto *gl = new KanbanOpenGLView(this);
        gl->setRenderer(m_renderer);
        connect(gl, &KanbanOpenGLView::contextReady, this, &KanbanWindow::glContextReady);
        m_view = gl;
    } else
#endif
    {
        auto *sw = new KanbanSoftwareView(this);
        sw->setRenderer(m_renderer);
        m_view = sw;
    }

    layout()->addWidget(m_view);
    // 交互代码只写一份：视图的事件冒泡到窗口前先用事件过滤器截获。
    m_view->installEventFilter(this);
    m_view->setMouseTracking(true);
    m_view->setAttribute(Qt::WA_Hover, true);
    m_view->show();
}

void KanbanWindow::requestFrame()
{
    if (m_view) {
        m_view->update();
    }
}

// —— 原生样式 ——

void KanbanWindow::setAlwaysOnTop(bool onTop)
{
    if (m_alwaysOnTop == onTop) {
        return;
    }
    m_alwaysOnTop = onTop;
    applyTopmostStyle();
    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("[KanbanWindow] 窗口置顶=%1").arg(onTop),
                   QLatin1String(kModule));
}

void KanbanWindow::applyTopmostStyle()
{
#ifdef Q_OS_WIN
    if (!testAttribute(Qt::WA_WState_Created)) {
        return; // HWND 还不存在，等 show 后再贴
    }
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        return;
    }
    // 用 SetWindowPos 而非改 Qt 窗口标志：改标志会让 Qt 销毁并重建 HWND，画面闪一下且
    // 子视图 GL 上下文也没了。
    SetWindowPos(hwnd, m_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}

void KanbanWindow::setMouseThrough(bool through)
{
    if (m_mouseThrough == through) {
        return;
    }
    m_mouseThrough = through;
    applyMouseThroughStyle();
    // 穿透后收不到鼠标事件，悬停态必须手工退出，否则状态机会卡在 Hover。
    if (through) {
        emit pointerLeft();
    }
    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("[KanbanWindow] 鼠标穿透=%1").arg(through),
                   QLatin1String(kModule));
}

void KanbanWindow::applyMouseThroughStyle()
{
#ifdef Q_OS_WIN
    if (!testAttribute(Qt::WA_WState_Created)) {
        return;
    }
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        return;
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (m_mouseThrough) {
        ex |= LONG_PTR(WS_EX_TRANSPARENT);
    } else {
        ex &= ~LONG_PTR(WS_EX_TRANSPARENT);
    }
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    // 样式改动需要一次 SetWindowPos 才落地(其余参数全是空操作)。
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
}

void KanbanWindow::setTransparencyPercent(int percent)
{
    m_transparencyPercent = qBound(0, percent, 80);
    setWindowOpacity((100 - m_transparencyPercent) / 100.0);
}

void KanbanWindow::setMenuAppearance(const QColor &menuBg, int menuTransparencyPercent,
                                     int glassLevel)
{
    m_menuBg = menuBg;
    m_menuTransparency = qBound(0, menuTransparencyPercent, kMaxMenuTransparencyPercent);
    m_menuGlass = qBound(0, glassLevel, 100);
}

// 套用右键菜单外观：底色/条目配色走样式表；背景由「QSS 实色层 + 亚克力模糊层」两层叠成，
// 透明度滑杆管整体不透明度、玻璃滑杆管实色层让位多少（详见下面第 4 条）。
//
// ⚠️⚠️ 四条踩过的坑，改动前先读完（每条都实测复现过）：
//
//  1. **透明度只能动背景、不能动文字 —— 用 QSS 背景的 alpha，不要用 `setWindowOpacity`。**
//     用户的需求是「调透明度时是调背景的，不是调整个菜单（含文字）」。`setWindowOpacity`
//     作用于合成层，整个窗口一起透，文字也会被拉透 —— 抓图验证（T4 见 tmp/_t4.png）：
//         setWindowOpacity(0.5) → 文字像素被混进红底，最亮 RGB 明显低于 255
//         QSS `background: rgba(..., 0.5)` → 文字像素仍 255,255,255，背景按 alpha 稀释
//     另一个副作用：`setWindowOpacity` 触发 QMenu 的「渐变回弹」（popup 后 40~80ms
//     被推回 1.0），必须轮询校正才压得住（T1 / T2 实测），弹出瞬间会闪一下默认状态；
//     QSS 是在 exec 之前一次性设好，**弹出即最终效果，无跳变**。
//     ⇒ 这里走 QSS alpha，不挂 aboutToShow，不设 setWindowOpacity。
//
//  2. **不做「全部默认就跳过」的早退。** 滑杆从 80 往回拖、玻璃从 0 往上拖，中途必然
//     经过「两项都是默认值」那一刻；早退会让样式表停在上一次的状态，看起来还是滑杆失灵。
//     代价是菜单失去系统原生的圆角阴影（与已知的 setMask 硬台阶同源），换确定。
//
//  3. **底色别写死在 QSS 里当唯一来源。** 亚克力只负责模糊与 tint，用户选的底色
//     要**自己画**才能“所见即所得”。窗口走 Fusion 绘制 + WA_TranslucentBackground，
//     菜单基色由 Fusion palette 给，不是 QSS 主题。硬写 #1c1d25 在浅色主题下会把
//     浅底深字的菜单改成深底 —— 用户只动了透明度，却看到整个配色变了。
//
//  4. **⚠️⚠️ 玻璃效果的真凶是 `AccentPolicy.flags`，不是 tint 调得淡不淡。**（2026-09-23）
//     现场是「透明度 / 玻璃效果两个滑杆怎么拖都没反应」。查下来是三件事叠在一起：
//       a) `flags = 2` 让亚克力变成一层几乎不透明的膜 —— 背景只透出 2~10%，
//          改成 0 才有 60%+；
//       b) QSS 那层实色底色（旧代码钳到 ≤190）盖在模糊上，把模糊整个挡没了；
//       c) 菜单后面多半是看板娘自己的浅色画面，模糊前后都是浅色，肉眼更分不出来。
//     验证方式（⚠️ 别用高频棋盘）：棋盘模糊后和不透明覆盖长得一模一样，判据分不出来。
//     要**换整块纯色底**（黑/白/红）看菜单内部跟不跟着变 —— 会变才是真半透明。
//     脚本 tmp/_acr_backdrop.py（定性）与 tmp/_acr_tint.py（标定 tint 响应曲线）。
//     ⚠️ 另：`--menu-style` 那几条断言读的是 QSS 字符串，**证明不了屏幕上透没透**，
//     别拿它当「玻璃生效」的证据。
void KanbanWindow::applyMenuStyle(QMenu *menu)
{
    if (!menu)
        return;

    // 显式开 translucent：Qt 的 QMenu 默认不开。⚠️ 但它**不足以**让 QSS 背景的 alpha
    // 跟桌面混 —— QMenu 的原生窗口不是分层窗口（见文件顶部 applyGlassSheet 那段）。
    // 它的作用只是在**窗口内部**合成时让 alpha 被尊重（tmp/_t5：开了之后背景从 191 降到 127）。
    menu->setAttribute(Qt::WA_TranslucentBackground, true);

    // 底色：用户选了就用用户的；没选则沿用当前 palette 的窗口色(跟着主题走)。
    const QColor base = m_menuBg.isValid() ? m_menuBg : menu->palette().color(QPalette::Window);

    // —— 菜单背景是**两层叠出来的**，两个滑杆各管一头 ——
    //   第 1 层：QSS 的实色背景（下面这段）
    //   第 2 层：亚克力毛玻璃（玻璃 > 0）或玻璃板（玻璃 = 0）—— 见文件顶部的
    //           applyWindowAcrylic / applyGlassSheet
    // 第 1 层是实心色，盖在第 2 层上就把模糊挡没了 —— 所以玻璃越强，第 1 层让位越多。
    //
    // 实测曲线（tmp/_acr_tint.py，黑底/白底各跑一遍解 final = c + m*backdrop）：
    //   亚克力 tint alpha =   0 → 背景透出 83%
    //                      64 → 61%
    //                     140 → 35%
    //                     192 → 18%
    //                     255 →  0%（整片实心，模糊被自己的颜色盖死）
    // 即「tint 越实 → 越看不见模糊」，所以第 2 层的 tint 有上限，见下面的 kAcrylicMaxTint。
    //
    //   transparency=0,  glass=0   → QSS alpha=255，不挂玻璃板（与改造前逐像素一致）
    //   transparency=80, glass=0   → QSS alpha=51 + 玻璃板 → 桌面真透出 80%（2026-09-23 修）
    //   transparency=0,  glass=100 → QSS alpha=0，底色全交给亚克力
    //   transparency=80, glass=100 → QSS alpha=0，亚克力 tint 也最淡（最透 + 最模糊）
    const double opacity = 1.0 - m_menuTransparency / 100.0; // 1.0(不透明) .. 0.2(最透)
    const double glass = m_menuGlass / 100.0;

    int bgAlpha = qRound(255 * opacity);
    if (m_menuGlass > 0)
        bgAlpha = qRound(bgAlpha * (1.0 - glass));

    QColor bg = base;
    bg.setAlpha(bgAlpha);

    // 文字色由底色亮度定，保持不透明(alpha=255)，与滑杆无关。
    const int lum = int(0.299 * base.red() + 0.587 * base.green() + 0.114 * base.blue());
    const QString text = lum < 128 ? QStringLiteral("#f2f4f8") : QStringLiteral("#17181c");
    const QString sel = lum < 128 ? QStringLiteral("rgba(140,180,255,0.35)")
                                  : QStringLiteral("rgba(90,130,220,0.25)");

    QStringList qss;
    qss << QStringLiteral("QMenu { background: %1; border: 1px solid rgba(128,128,128,0.4);"
                          " border-radius: 10px; padding: 6px; }")
               .arg(bg.name(QColor::HexArgb));
    qss << QStringLiteral("QMenu::item { background: transparent; color: %1;"
                          " padding: 6px 26px 6px 14px; border-radius: 6px; }")
               .arg(text);
    qss << QStringLiteral("QMenu::item:selected { background: %1; }").arg(sel);
    qss << QStringLiteral("QMenu::item:disabled { color: rgba(128,128,128,0.8); }");
    qss << QStringLiteral("QMenu::separator { height: 1px;"
                          " background: rgba(128,128,128,0.35); margin: 4px 8px; }");
    menu->setStyleSheet(qss.join(QLatin1Char('\n')));

#ifdef Q_OS_WIN
    if (m_menuGlass > 0) {
        // 亚克力 tint 的 alpha 承担「菜单自身颜色」那部分不透明度 —— QSS 层已经让位了，
        // 不补上这一层，玻璃拉满时菜单会淡到条目都看不清。
        // ⚠️ 通道顺序是 0xAABBGGRR（Windows 的老习惯），别按 ARGB 拼 —— 拼错的表现是
        // 底色变成“反色”（红蓝互换）。
        // ⚠️ 上限 140 不是随手写的：tint≥192 时背景只剩 18%、255 时归零，等于白挂亚克力。
        // 亚克力是 HWND 模糊层，必须挂在**已经存在的 HWND** 上；winId() 会顺带建窗口。
        constexpr int kAcrylicMaxTint = 140;
        const UINT alpha = UINT(qRound(kAcrylicMaxTint * opacity));
        const UINT abgr = (alpha << 24) | (UINT(bg.blue()) << 16)
                          | (UINT(bg.green()) << 8) | UINT(bg.red());
        if (!applyWindowAcrylic(menu->winId(), abgr)) {
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("[KanbanWindow] 菜单亚克力不可用(系统不支持)，保留半透明底色"),
                           QLatin1String(kModule));
        }
    } else if (bgAlpha < 255) {
        // 玻璃关着、但只要不是完全不透明，就得挂「玻璃板」—— 否则 QSS 的 alpha
        // **只朝黑色稀释**，透明度滑杆等于没接上桌面。这是「玻璃效果」之外的另一半：
        // 旧版只有 `m_menuGlass > 0` 才碰原生合成，于是 gl=0 时透明度全程失效。
        // bgAlpha==255（tr=0 且 gl=0，默认态）不挂，菜单外观与改造前逐像素一致。
        if (!applyGlassSheet(menu->winId())) {
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("[KanbanWindow] 菜单玻璃板不可用(系统不支持)，透明度只会朝黑稀释"),
                           QLatin1String(kModule));
        }
    }
#else
    Q_UNUSED(bg)
#endif
}

void KanbanWindow::setPausedVisual(bool paused)
{
    m_pausedVisual = paused;
    setCursor(paused ? Qt::ArrowCursor : Qt::OpenHandCursor);
    requestFrame();
}

void KanbanWindow::setModelDisplayName(const QString &)
{
    // 名称显示在主界面与托盘提示里，窗口本体不放任何说明性浮层。
}

void KanbanWindow::setInteractionEnabled(bool enabled)
{
    m_interactionEnabled = enabled;
    if (!enabled) {
        m_pressSeen = false;
        m_dragging = false;
        emit pointerLeft();
    }
}

void KanbanWindow::placeFromConfig(int x, int y, int width, int height)
{
    if (width >= kMinWindowEdgePx) {
        resize(width, qMax(height, kMinWindowEdgePx));
    }
    // 坐标未必还落在屏幕上(换分辨率、拔副屏、远程桌面重连)。判据取「与任一屏幕可用区
    // 有交集」而非「完全在屏幕内」—— 窗口露一半在边缘是合法用法。
    if (x >= 0 && y >= 0 && isOnAnyScreen(QRect(QPoint(x, y), size()))) {
        move(x, y);
        return;
    }
    // 首次启动(或位置已失效)：贴主屏可用区右下角，避开任务栏。
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) {
        return;
    }
    const QRect avail = screen->availableGeometry();
    // 形参 width/height 遮蔽了同名成员函数，这里要的是 resize 之后的实际尺寸。
    move(avail.right() - this->width() - kScreenMarginPx,
         avail.bottom() - this->height() - kScreenMarginPx);
}

void KanbanWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 布局会自动把子视图铺满。
    applyTopmostStyle();
    applyMouseThroughStyle();
}

// —— 交互：全部走事件过滤器，两个视图共用同一套语义 ——

bool KanbanWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_view || !event) {
        return QWidget::eventFilter(watched, event);
    }

    const QPointF viewPos = [](const QEvent *e) {
        if (e->type() == QEvent::Wheel) {
            return static_cast<const QWheelEvent *>(e)->position();
        }
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease
            || e->type() == QEvent::MouseButtonDblClick || e->type() == QEvent::MouseMove) {
            return static_cast<const QMouseEvent *>(e)->position();
        }
        return QPointF();
    }(event);

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && m_interactionEnabled) {
            m_pressSeen = true;
            m_dragging = false;
            m_pressGlobal = me->globalPosition().toPoint();
            m_dragOffset = m_pressGlobal - m_view->mapToGlobal(QPoint(0, 0));
            setCursor(Qt::ClosedHandCursor);
        }
        break;
    }
    case QEvent::MouseMove: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (m_interactionEnabled) {
            emit hoveredAt(viewPos);
        }
        if (!(me->buttons() & Qt::LeftButton) || !m_pressSeen) {
            break;
        }
        const QPoint global = me->globalPosition().toPoint();
        if (!m_dragging) {
            if ((global - m_pressGlobal).manhattanLength() < kDragThresholdPx) {
                break;
            }
            m_dragging = true;
            emit dragStarted();
        }
        move(global - m_dragOffset);
        break;
    }
    case QEvent::MouseButtonRelease: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton || !m_pressSeen) {
            break;
        }
        m_pressSeen = false;
        setCursor(m_pausedVisual ? Qt::ArrowCursor : Qt::OpenHandCursor);
        if (m_dragging) {
            m_dragging = false;
            emit dragFinished();
        } else if (m_interactionEnabled) {
            emit clicked(viewPos);
        }
        break;
    }
    case QEvent::MouseButtonDblClick: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && m_interactionEnabled) {
            emit doubleClicked(viewPos);
        }
        break;
    }
    case QEvent::HoverEnter:
        if (m_interactionEnabled) {
            emit pointerEntered();
        }
        break;
    case QEvent::HoverLeave:
        if (m_interactionEnabled) {
            emit pointerLeft();
        }
        break;
    case QEvent::Wheel: {
        auto *we = static_cast<QWheelEvent *>(event);
        const int steps = we->angleDelta().y() / 120;
        if (steps != 0 && m_interactionEnabled) {
            emit scaleStepped(steps);
        }
        we->accept();
        return true; // 已处理，避免继续冒泡到窗口触发别的默认行为
    }
    case QEvent::ContextMenu: {
        auto *ce = static_cast<QContextMenuEvent *>(event);
        // 刻意不给父对象：窗口若在 menu.exec() 期间被销毁，Qt 会顺着父子关系 delete 掉栈
        // 上的 menu，对栈地址 free 属于必崩写法。
        QMenu menu;
        QAction *pauseAct = menu.addAction(QStringLiteral("暂停 / 恢复"));
        connect(pauseAct, &QAction::triggered, this, &KanbanWindow::pauseResumeRequested);
        QAction *nextAct = menu.addAction(QStringLiteral("切换动作"));
        // 可播动作不足两个就置灰：只有一个时「切换」等于原地重播。判据收在渲染器基类。
        nextAct->setEnabled(m_renderer && m_renderer->canPlayNextMotion());
        connect(nextAct, &QAction::triggered, this, &KanbanWindow::playNextRequested);
        QAction *exprAct = menu.addAction(QStringLiteral("切换表情"));
        // 没有表情的模型就把入口置灰(点了没反应比灰掉更让人怀疑坏了)，数量问当前渲染器。
        exprAct->setEnabled(m_renderer && m_renderer->expressionCount() > 0);
        connect(exprAct, &QAction::triggered, this, &KanbanWindow::nextExpressionRequested);
        QAction *modelAct = menu.addAction(QStringLiteral("切换模型"));
        connect(modelAct, &QAction::triggered, this, &KanbanWindow::nextModelRequested);

        menu.addSeparator();
        QAction *throughAct = menu.addAction(QStringLiteral("鼠标穿透"));
        throughAct->setCheckable(true);
        throughAct->setChecked(m_mouseThrough);
        // 发「请求」而非直接调 setMouseThrough：只有控制器那条路会同步成员、落盘配置并
        // 回写设置页复选框。
        connect(throughAct, &QAction::toggled, this, &KanbanWindow::mouseThroughRequested);
        QAction *topAct = menu.addAction(QStringLiteral("窗口置顶"));
        topAct->setCheckable(true);
        topAct->setChecked(m_alwaysOnTop);
        // 同上：置顶也是配置键 + 设置页复选框两处要同步。
        connect(topAct, &QAction::toggled, this, &KanbanWindow::alwaysOnTopRequested);
        menu.addSeparator();

        // 视线追踪刻意不放进这个菜单：档位是「一次定好、长期不动」的偏好，已在设置页与
        // 托盘两处，再来一份只会让菜单变长还容易误点。
        QAction *settingAct = menu.addAction(QStringLiteral("打开设置"));
        connect(settingAct, &QAction::triggered, this, &KanbanWindow::settingsRequested);

        // 「取消看板娘」不能直接在 exec() 里发出去：接收方会销毁本窗口，而此刻我们还在
        // menu.exec() 的嵌套事件循环里；先记下意图，等 exec 返回后再发。
        bool quitAsked = false;
        QAction *quitAct = menu.addAction(QStringLiteral("取消看板娘"));
        connect(quitAct, &QAction::triggered, this, [&quitAsked] { quitAsked = true; });

        // 外观必须在 exec **之前**套好：applyMenuStyle 要写 QSS 背景、还要把亚克力挂到
        // 菜单的 HWND 上，等 exec 之后菜单已经合成上屏，再改就来不及了。
        //（早期版本给的理由是"applyMenuStyle 里挂了 aboutToShow 钩子"—— 那个钩子早就
        //  随 setWindowOpacity 路线一起删了，理由不再成立，但"exec 之前套"这个结论仍然对。
        //  另：「尺寸变了透明度会失效」那个说法也实测证伪过，别再照抄。）
        applyMenuStyle(&menu);

        menu.exec(ce->globalPos());
        if (quitAsked) {
            emit quitRequested();
        }
        return true;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void KanbanWindow::closeEvent(QCloseEvent *event)
{
    // 窗口是无边框 Qt::Tool，能走到这里的只有系统关机、任务栏「关闭窗口」这类外部请求，
    // 一律吞掉：放行会让窗口消失而控制器仍以为它在跑，且无恢复入口。
    event->ignore();
}

bool KanbanWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType == QStringLiteral("windows_generic_MSG") && message && m_taskbarCreatedMsg) {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message == UINT(m_taskbarCreatedMsg)) {
            applyTopmostStyle();
            applyMouseThroughStyle();
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("[KanbanWindow] 收到 TaskbarCreated，已重贴窗口样式"),
                           QLatin1String(kModule));
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
#endif
    Q_UNUSED(result);
    return QWidget::nativeEvent(eventType, message, result);
}

} // namespace kanban

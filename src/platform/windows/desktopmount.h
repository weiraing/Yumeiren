#ifndef DESKTOPMOUNT_H
#define DESKTOPMOUNT_H

// 视频壁纸的 Win32 平台层：WorkerW 挂载 + 挂起策略依赖的探针。刻意不引入 Qt 具体类型头文件。

#include <QtCore/qglobal.h>

class QWidget;
class QRect;
class QScreen;

namespace winhelper {

// 定位(必要时经 Progman 0x052C 唤起)图标层之后的 WorkerW；explorer 不在时返回 false
bool ensureWorker();
bool isWorkerValid();
// 只挂在 Progman 兜底上时为 false(部分 Win11 构建不合成该子窗口)；心跳会持续重查
bool hasRealWorker();

// 挂到图标层之后（桌面最底）。
// logicalTarget 是 Qt 逻辑坐标，内部按物理像素换算，否则副屏位置错误。
//
// **只有一种挂法**（2026-09-24 定案，取消原「网页交互」档）：SetParent 到 WorkerW，
// 成为桌面窗口的子窗口。最稳，图标正常、绝对不抢鼠标 —— 代价是**收不到任何鼠标消息**
// （子窗口在 DefView 的命中层之下，WindowFromPoint 恒命中 explorer 的 SHELLDLL_DefView）。
// 壁纸是"挂着看"的东西，这个代价是设计选择，不是缺陷。
//
// ⚠️ 别再试图"保持顶层窗口插到 Progman 之上"来让它吃鼠标 —— 那条路要显式剥掉
//    WS_EX_LAYERED，剥掉后**桌面图标会被壁纸盖住**，且顶层窗口的 z 序会被别的程序
//    扰动、必须每 10s 重挂维护。这是 2026-09-24 实测过的取舍，用户已明确不要。
void mountBehindIcons(QWidget *window, const QRect &logicalTarget);
void unmountWindow(QWidget *window);

// 每秒一次的廉价探针：窗口仍挂在当前 WorkerW 下、可见且恰好位于 physicalRect 时为 true
bool isWindowMounted(QWidget *window, const QRect &physicalRect);

// 「桌面被应用窗口完全遮住」的两档判定。都在物理像素下遍历可见顶层窗口，排除最小化、cloaked(DWM 未合成)以及自身进程与 explorer 桌面层(都是全屏矩形)，留 ±2px 容差；都刻意不看"谁在前台"(全屏应用前面压个小窗口时桌面依然不可见)。全屏档=某窗口精确铺满前台窗口所在那块屏(只看同屏，免副屏全屏应用停掉主屏壁纸)；遮挡档=某窗口完全盖住主屏工作区。
bool isFullscreenWindowPresent();
// 遮挡档：某窗口完全盖住主屏工作区，即最大化的普通窗口
bool isDesktopCoveredByWindow();
bool isWorkstationLocked();
bool isOnBattery();

// 把同 exe 已运行实例的主窗口(标题精确匹配、有实际尺寸)调到前台，最小化则先还原；找不到实例或主窗口返回 false，由调用方兜底
bool activateExistingInstanceWindow(const QString &mainWindowTitle,
                                    QString *reason = nullptr);
// 资源友好模式：亲和性限制到 maxCores 个逻辑核，优先分属不同物理核(避开 SMT 兄弟对)；实测(32核机,1080p30)内存-27%、显存-36%、CPU 不变。核数本就不多时返回 false
bool applyProcessAffinityLimit(int maxCores);

// 单实例唯一运行权：命名互斥锁，进程退出(含崩溃/强杀)时内核自动释放，不会像 QSharedMemory 那样残留。true=获得运行权(句柄故意不关=锁)；false=已有实例
bool acquireSingleInstanceLock();

// 主窗口隐藏到托盘时不能直接 ShowWindow(Qt 状态会失真)，改为发这条消息让已运行实例自己走"显示主窗口"的同一条路。注册失败返回 0
unsigned int showMainWindowMessage();

void trimProcessMemory();

} // namespace winhelper

#endif // DESKTOPMOUNT_H

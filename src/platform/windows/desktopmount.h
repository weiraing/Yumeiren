#ifndef DESKTOPMOUNT_H
#define DESKTOPMOUNT_H

// Win32 platform layer for the video wallpaper: WorkerW mounting and the
// one-shot system probes behind the suspend policy. All Qt types stay out of
// the .cpp include set where practical so the business module never touches
// window handles directly.

#include <QtCore/qglobal.h>

class QWidget;
class QRect;
class QScreen;

namespace fbswin {

// Locate (spawning it via the Progman 0x052C trick if needed) the WorkerW that
// sits behind the desktop icons. Returns false only when explorer is gone.
bool ensureWorker();
bool isWorkerValid();
// False while mounted to the Progman fallback (which some Win11 builds refuse
// to composite for cross-process children); the health tick keeps re-searching
// until a real WorkerW shows up.
bool hasRealWorker();

// Parent the window to the WorkerW and place it at logicalTarget (Qt logical
// coordinates, e.g. a QScreen::geometry()) converted to the WorkerW's physical
// pixel space, so secondary monitors land on the right screen.
void mountBehindIcons(QWidget *window, const QRect &logicalTarget);
void unmountWindow(QWidget *window);

// Cheap per-second health probe: true when the window is still parented to the
// current WorkerW, visible and sitting exactly at physicalRect.
bool isWindowMounted(QWidget *window, const QRect &physicalRect);

// 「桌面被应用窗口完全遮住」的两档判定。两条都**不再只看前台窗口**：
// 全屏应用前面压着一个小窗口(对话框/通知/输入法候选)时桌面依然不可见，
// 只看前台窗口会把这种情形误判成「已回到桌面」而恢复播放、白白解码。
// 两者都遍历可见的顶层窗口，排除最小化、DWM 未合成(cloaked：别的虚拟桌面上的、
// 被挂起的 UWP)以及自身进程与 explorer 桌面层(Progman/WorkerW/DefView 都是
// 全屏矩形，否则「看着桌面」也会被判成被遮挡)。比较在物理像素下进行
// (GetWindowRect 与 MONITORINFO 都不随 DPI 缩放)，留 ±2px 容差。
//
// 全屏档：某窗口精确铺满前台窗口所在的那块屏(borderless 或独占全屏)。
// 只看同屏，是为了让副屏上挂着的全屏应用不至于把主屏壁纸一起停掉。
bool isFullscreenWindowPresent();
// 遮挡档：某窗口完全盖住主屏工作区，即最大化的普通窗口 —— 此时壁纸完全不可见。
bool isDesktopCoveredByWindow();
bool isWorkstationLocked();
bool isOnBattery();

// Return idle pages to the OS. Cosmetic (Private Bytes unchanged) and pages
// back in on demand; only meaningful when the pipeline is torn down.
// 单实例二次启动体验：把同 exe 已运行实例的主窗口(标题精确匹配、有实际尺寸)
// 调到前台。最小化则先还原；找不到实例或主窗口返回 false，由调用方决定兜底行为。
bool activateExistingInstanceWindow(const QString &mainWindowTitle,
                                    QString *reason = nullptr);

// 资源友好模式：把进程亲和性限制到 maxCores 个逻辑核，并优先让它们分属不同
// 物理核(避开 SMT 兄弟对)。解码线程数跟随 QThread::idealThreadCount(受掩码
// 影响)，实测(32核机,1080p30)内存-27%、显存-36%、CPU不变。
// 逻辑核总数<=maxCores 时不做任何修改，返回 false。
bool applyProcessAffinityLimit(int maxCores);

// 单实例唯一运行权：命名互斥锁实现。进程退出(含强杀/崩溃)时内核自动释放，
// 从原理上杜绝 QSharedMemory 段残留导致"已经在运行"却无实例的问题。
// 返回 true=获得运行权(句柄故意保持打开=锁)；false=已有实例在运行。
bool acquireSingleInstanceLock();

// 主窗口隐藏到托盘后，二次启动只能枚举到一个不可见窗口，此时不能直接
// ShowWindow(Qt 不知道窗口又出现了，状态会失真)，改为给已运行实例发这条
// 应用级消息，由它自己走「显示主窗口」的同一条路。返回值注册失败时为 0。
unsigned int showMainWindowMessage();

void trimProcessMemory();

} // namespace fbswin

#endif // DESKTOPMOUNT_H

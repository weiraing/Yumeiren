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

// True when the foreground window covers a screen exactly (borderless or
// exclusive fullscreen). Comparison happens in physical pixels because
// GetWindowRect is not DPI-scaled; QScreen::geometry() is.
bool isForegroundFullscreen();
// True when the foreground window fully covers the primary monitor's work area
// (i.e. a maximized normal app): the desktop wallpaper is invisible then, so
// playback can pause without anyone noticing. Shell/explorer windows are
// excluded so looking AT the desktop never pauses it.
bool isDesktopCovered();
bool isWorkstationLocked();
bool isOnBattery();

// Return idle pages to the OS. Cosmetic (Private Bytes unchanged) and pages
// back in on demand; only meaningful when the pipeline is torn down.
// 单实例二次启动体验：把同 exe 已运行实例的主窗口(标题精确匹配、有实际尺寸)
// 调到前台。最小化则先还原；找不到实例或主窗口返回 false，由调用方决定兜底行为。
bool activateExistingInstanceWindow(const QString &mainWindowTitle,
                                    QString *reason = nullptr);

// 单实例唯一运行权：命名互斥锁实现。进程退出(含强杀/崩溃)时内核自动释放，
// 从原理上杜绝 QSharedMemory 段残留导致"已经在运行"却无实例的问题。
// 返回 true=获得运行权(句柄故意保持打开=锁)；false=已有实例在运行。
bool acquireSingleInstanceLock();

void trimProcessMemory();

} // namespace fbswin

#endif // DESKTOPMOUNT_H

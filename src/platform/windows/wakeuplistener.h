#ifndef WAKEUPLISTENER_H
#define WAKEUPLISTENER_H

// 二次启动「唤起主窗口」的常驻收信口。
//
// **为什么需要它**（2026-09-24 实测）：
//   主窗口 hide() 之后原生 HWND **还活着**（`IsWindow(hwnd) == 1`），但 `IsWindowVisible`
//   为 0；更要命的是 Qt 会同时新建一个**可见的**顶层辅助窗口
//   `QtXXXXQWindowToolSaveBitsOwnDC`，**标题与应用主窗口完全相同**（都是 `Yumeiren`）、
//   面积还不小 —— 于是二次启动那边「按标题找主窗口」会认错人，把那个辅助窗口
//   `SetForegroundWindow` 一下，日志报"已激活"，用户的主窗口纹丝不动。
//   （这正是「再打开软件只弹一句『虞美人已经在运行』／窗口叫不出来」的根因链。）
//
// **为什么要独立线程**：建在 GUI 线程上的窗口（普通窗口与 message-only 都试过）
//   会跟主窗口同生共死 —— 主窗口 hide() 时一起被销毁，`FindWindowW(类名)` 归 0。
//   所以这里开一条**独立线程**，线程里跑自己的消息循环，窗口生死只跟进程绑定。
//
// 收到唤起消息后，经队列连接切回本对象所属线程（GUI）发 wakeupRequested()。
//
// ⚠️ 窗口**类名 `Yumeiren.WakeupListener` 是与 desktopmount.cpp 的跨文件契约**
//    （那边 `findWakeupListener()` 按它 + pid 认领），改一处必须改两处。

#include <QObject>

class QThread;

namespace winhelper {

class WakeupListener : public QObject
{
    Q_OBJECT
public:
    explicit WakeupListener(QObject *parent = nullptr);
    ~WakeupListener() override;

    // 起线程、建窗口。窗口类名固定为 `Yumeiren.WakeupListener`，
    // 跨进程靠它 + pid 认领（见 desktopmount.cpp 的 findWakeupListener）。
    bool start();

    // 停线程、销毁窗口（退出时调）。
    void stop();

    bool isActive() const;

signals:
    // 收到「唤起主窗口」请求（已切回本对象所属线程，即 GUI 线程）
    void wakeupRequested();

private:
    QThread *m_thread = nullptr;
    // 线程侧工作对象的裸指针（类型定义在 .cpp 的匿名 namespace 里，此处不暴露）。
    // 只在本类的 start()/stop() 里触碰，且都在线程创建/等待的同步点上。
    void *m_worker = nullptr;
};

} // namespace winhelper

#endif // WAKEUPLISTENER_H

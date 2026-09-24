#include "wakeuplistener.h"

#include "core/Diagnostics.h"
#include "platform/windows/desktopmount.h"

#include <QThread>

#include <atomic>
#include <functional>

#ifdef Q_OS_WIN
// NOMINMAX 已由 MinGW 的 os_defines.h 定义，这里只做兜底，避免重定义警告
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace winhelper {

#ifdef Q_OS_WIN

// 类名是与 desktopmount.cpp 的 findWakeupListener() 约定的**跨文件契约**，改一处要改两处。
static const wchar_t *const kWakeupClass = L"Yumeiren.WakeupListener";
namespace {

// 线程侧工作对象：建窗口 + 跑自己的消息循环。只被本线程碰。
class WakeupWorker
{
public:
    std::function<void()> onWakeup; // 收到唤起请求时回调（内部已切回 GUI 线程）

    // 建窗口。**顶层**（parent=nullptr）普通窗口，0×0、无 WS_VISIBLE、
    // TOOLWINDOW|NOACTIVATE：不上屏、不进 Alt-Tab、不抢焦点。
    //
    // ⚠️ 三条实测结论（前两版都踩过）：
    //   ① 不能 message-only(HWND_MESSAGE)：那种窗口在主窗口 hide 后
    //      `FindWindowW(类名)` 返回 0，跨进程搜不到。
    //   ② 不能建在 GUI 线程上：跟主窗口同生共死，hide 时一起被销毁。
    //   ③ 必须独立线程：线程有自己的消息循环，窗口生死只跟进程绑定。
    bool createWindow()
    {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &WakeupWorker::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kWakeupClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWakeupClass,
                                 L"", 0, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        return m_hwnd != nullptr;
    }

    void destroyWindow()
    {
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

    // stop() 要往窗口投 WM_CLOSE 来结束线程消息循环，所以这里给个只读访问口
    // （成员本身保持私有：它只该被本线程和这一处跨线程读取碰）。
    HWND hwnd() const { return m_hwnd; }

    // 线程退出前必须走一遍：UnregisterClassW 要求**同线程**、
    // 且类不再有任何窗口（所以只能在 DestroyWindow 之后调）。
    void unregisterClass()
    {
        if (HINSTANCE inst = GetModuleHandleW(nullptr))
            UnregisterClassW(kWakeupClass, inst);
    }

    bool running = false; // 供线程外判断窗口是否已就绪，避免投早期 WM_CLOSE

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        WakeupWorker *self =
            reinterpret_cast<WakeupWorker *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
            self = static_cast<WakeupWorker *>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (msg == WM_CLOSE) {
            // stop() 用来结束本线程消息循环
            PostQuitMessage(0);
            return 0;
        }
        if (self && showMainWindowMessage() != 0 && msg == showMainWindowMessage()) {
            if (self->onWakeup)
                self->onWakeup(); // 只转发；真正的显示在 GUI 线程做
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

private:
    HWND m_hwnd = nullptr;
};

} // namespace

#endif // Q_OS_WIN

WakeupListener::WakeupListener(QObject *parent) : QObject(parent) {}

WakeupListener::~WakeupListener()
{
    stop();
}

bool WakeupListener::isActive() const
{
    return m_thread && m_thread->isRunning();
}

bool WakeupListener::start()
{
#ifdef Q_OS_WIN
    if (m_thread)
        return isActive();

    // 线程闭包里要回读「窗口是否建好」，用一个栈外的原子量传结果。
    // （窗口创建本身很快，但 start() 不该在窗口就绪前返回 true。）
    auto *ok = new std::atomic<bool>(false);
    m_thread = QThread::create([this, ok] {
        auto *worker = new WakeupWorker;
        worker->onWakeup = [this] {
            // 从监听线程切回本对象所属线程（GUI）再发信号
            QMetaObject::invokeMethod(this, [this] { emit wakeupRequested(); },
                                      Qt::QueuedConnection);
        };
        if (!worker->createWindow()) {
            const DWORD err = GetLastError();
            applog::log(applog::Level::Warning,
                        QStringLiteral("唤起收信窗口创建失败(err=%1)").arg(err),
                        QStringLiteral("UI"));
            delete worker;
            return;
        }
        // 先挂指针再置 running：stop() 只在 running 为真时才投 WM_CLOSE，
        // 避免窗口还没进消息循环就被 PostQuitMessage 掉（那条消息会被丢弃，
        // GetMessageW 随即返回 0，看似"正常退出"其实窗口从没活过）。
        m_worker = worker;
        worker->running = true;
        applog::log(applog::Level::Info,
                    QStringLiteral("唤起收信窗口已就绪(独立线程)"),
                    QStringLiteral("UI"));
        ok->store(true, std::memory_order_release);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        worker->destroyWindow();
        worker->unregisterClass();
        worker->running = false;
        m_worker = nullptr;
        delete worker;
    });
    m_thread->start();
    // 等窗口就绪（<50ms 的建窗口），最多 2 秒；超时也不算失败，
    // 只是二次启动可能撞上未就绪窗口 —— 记一条日志便于排查。
    for (int i = 0; i < 200 && !ok->load(std::memory_order_acquire); ++i)
        QThread::msleep(10);
    const bool ready = ok->load(std::memory_order_acquire);
    delete ok;
    if (!ready)
        applog::log(applog::Level::Warning,
                    QStringLiteral("唤起收信窗口在 2 秒内未就绪"),
                    QStringLiteral("UI"));
    return ready;
#else
    return false;
#endif
}

void WakeupListener::stop()
{
#ifdef Q_OS_WIN
    if (!m_thread)
        return;
    if (m_worker) {
        auto *worker = static_cast<WakeupWorker *>(m_worker);
        if (worker->running && worker->hwnd())
            PostMessageW(worker->hwnd(), WM_CLOSE, 0, 0);
    }
    if (!m_thread->wait(3000)) {
        // 窗口过程里除了 PostQuitMessage 什么都不做，走到这里基本只可能是
        // 消息循环还没起来。硬退一次，别让退出流程卡住。
        m_thread->quit();
        m_thread->wait(1000);
    }
    delete m_thread;
    m_thread = nullptr;
    m_worker = nullptr;
#endif
}

} // namespace winhelper

// 进程级全局热键：主窗口没有焦点(游戏全屏、最小化到托盘)也收得到。
//
// RegisterHotKey(nullptr, ...) 把热键挂在本线程(GUI 主线程)的消息队列上，WM_HOTKEY
// 经 QAbstractNativeEventFilter 收进来转成 activated() 信号 —— 因此 applySequence()
// 与析构都必须在 GUI 线程调用。同一实例同一时间只持有一组键：换键 = 先注销旧的再
// 注册新的，空序列 = 停用。
#ifndef GLOBALHOTKEY_H
#define GLOBALHOTKEY_H

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QKeySequence>

class QByteArray;

namespace fbswin {

class GlobalHotkey : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit GlobalHotkey(QObject *parent = nullptr);
    ~GlobalHotkey() override;

    // 注册 seq(如 "Ctrl+Alt+K")。空序列表示停用，返回 true。注册失败(组合不受支持，
    // 或键已被别的程序占用)返回 false，此时处于「无热键」状态，调用方负责提示。
    bool applySequence(const QKeySequence &seq);
    bool isActive() const { return m_registered; }

signals:
    void activated();

private:
    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           qintptr *result) override;
    void unregister();

    bool m_registered = false;
    unsigned m_vk = 0;        // Windows 虚拟键码
    unsigned m_modifiers = 0; // MOD_* 组合
};

} // namespace fbswin

#endif // GLOBALHOTKEY_H

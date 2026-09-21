#include "platform/windows/globalhotkey.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace fbswin {

namespace {

// RegisterHotKey 的 id 在「进程内挂在本线程队列」这个语境下自己说了算，取个不易
// 撞上别的 WM_HOTKEY 来源的值即可。
constexpr UINT kHotkeyId = 0x594D; // 'YM'

unsigned modFromModifiers(Qt::KeyboardModifiers mods)
{
    unsigned m = 0;
#ifdef Q_OS_WIN
    if (mods & Qt::ControlModifier)
        m |= MOD_CONTROL;
    if (mods & Qt::ShiftModifier)
        m |= MOD_SHIFT;
    if (mods & Qt::AltModifier)
        m |= MOD_ALT;
    if (mods & Qt::MetaModifier)
        m |= MOD_WIN;
#else
    Q_UNUSED(mods)
#endif
    return m;
}

// 只收常规壁纸软件会用到的键：字母/数字/F1~F24 加少数编辑键。多键序列(QKeySequence
// 的 "Ctrl+K, Ctrl+L" 形态)与纯修饰键在这里直接判不合法。
bool vkFromKey(Qt::Key key, unsigned *vk)
{
#ifdef Q_OS_WIN
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        *vk = UINT('A' + (key - Qt::Key_A));
        return true;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        *vk = UINT('0' + (key - Qt::Key_0));
        return true;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        *vk = VK_F1 + UINT(key - Qt::Key_F1);
        return true;
    }
    switch (key) {
    case Qt::Key_Space: *vk = VK_SPACE; return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: *vk = VK_RETURN; return true;
    case Qt::Key_Backspace: *vk = VK_BACK; return true;
    case Qt::Key_Tab: *vk = VK_TAB; return true;
    case Qt::Key_Escape: *vk = VK_ESCAPE; return true;
    case Qt::Key_Home: *vk = VK_HOME; return true;
    case Qt::Key_End: *vk = VK_END; return true;
    case Qt::Key_PageUp: *vk = VK_PRIOR; return true;
    case Qt::Key_PageDown: *vk = VK_NEXT; return true;
    default: return false;
    }
#else
    Q_UNUSED(key)
    Q_UNUSED(vk)
    return false;
#endif
}

} // namespace

GlobalHotkey::GlobalHotkey(QObject *parent) : QObject(parent)
{
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installNativeEventFilter(this);
}

GlobalHotkey::~GlobalHotkey()
{
    if (QCoreApplication *app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
    unregister();
}

void GlobalHotkey::unregister()
{
#ifdef Q_OS_WIN
    if (m_registered)
        UnregisterHotKey(nullptr, kHotkeyId);
#endif
    m_registered = false;
    m_vk = 0;
    m_modifiers = 0;
}

bool GlobalHotkey::applySequence(const QKeySequence &seq)
{
    unregister();
#ifdef Q_OS_WIN
    // 空序列 = 用户想停用，清干净就算成功。
    if (seq.isEmpty())
        return true;
    if (seq.count() != 1)
        return false;
    const QKeyCombination combo = seq[0];
    const unsigned mod = modFromModifiers(combo.keyboardModifiers());
    // 至少带 Ctrl/Alt/Win 之一：裸键或纯 Shift 的全局热键会把系统正常打字整个吞掉，
    // 这不是「注册不上」而是「不该注册」。
    if (!(mod & (MOD_CONTROL | MOD_ALT | MOD_WIN)))
        return false;
    unsigned vk = 0;
    if (!vkFromKey(combo.key(), &vk))
        return false;
    if (!RegisterHotKey(nullptr, kHotkeyId, mod, vk))
        return false; // 多半是被别的程序占了，ERROR_HOTKEY_ALREADY_REGISTERED
    m_registered = true;
    m_modifiers = mod;
    m_vk = vk;
    return true;
#else
    Q_UNUSED(seq)
    return false;
#endif
}

bool GlobalHotkey::nativeEventFilter(const QByteArray &eventType, void *message,
                                     qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType != QByteArrayLiteral("windows_generic_MSG") || !message)
        return false;
    auto *msg = static_cast<MSG *>(message);
    if (msg->message == WM_HOTKEY && msg->wParam == kHotkeyId) {
        emit activated();
        // 已消费：全局热键不该再落进 Qt 的键盘事件链。
        if (result)
            *result = 0;
        return true;
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return false;
}

} // namespace fbswin

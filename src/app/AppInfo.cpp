#include "app/AppInfo.h"

// 构建期生成的版本号宏（<build>/generated/YumeirenVersion.h）。
// 由 yumeiren_apply_version() 把这个目录加进 include path。
#include "YumeirenVersion.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <windows.h>

namespace {

// Value name under HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
constexpr wchar_t kAutostartValue[] = L"Yumeiren";

// 与 tools/icongen/make_icons.py 的 QT_SIZES 一致，也与 CMakeLists 的
// APP_RESOURCES 一致 —— 三处任何一处改了都要同步。
constexpr int kIconSizes[] = {16, 20, 24, 32, 48, 64, 128, 256};

HKEY openRunKey(REGSAM access)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, access, &key) != ERROR_SUCCESS)
        return nullptr;
    return key;
}

bool runValueExists(HKEY key, const wchar_t *name)
{
    DWORD type = 0, size = 0;
    return RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
}

} // namespace

namespace appinfo {

QString id()
{
    return QString::fromLatin1(kId);
}

QString displayName()
{
    return QStringLiteral("虞美人");
}

// 原生窗口标题(任务栏/Alt-Tab/任务管理器窗口行显示的就是它)。
// 与品牌名分离：管理器里进程行显示 FileDescription"虞美人"、窗口行显示"Yumeiren"
QString windowTitle()
{
    return QStringLiteral("Yumeiren");
}

QString version()
{
    // YumeirenVersion.h 由 cmake/Version.cmake 生成，值来自仓库根目录的 VERSION
    // 文件 / git 标签 / -DYUMEIREN_VERSION。这里只做转发，不参与任何拼装 ——
    // 版本号的构造逻辑只允许有一处。
    return QStringLiteral(YUMEIREN_VERSION_FULL);
}

QIcon appIcon()
{
    // 静态缓存：QIcon 的构造要读 8 个 PNG 并解压，托盘、标题栏、对话框都会来要，
    // 每次重建纯属浪费。QIcon 是隐式共享的，返回副本很便宜。
    static const QIcon icon = [] {
        QIcon result;
        for (const int size : kIconSizes) {
            const QString path =
                QStringLiteral(":/icons/yumeiren-%1.png").arg(size);
            if (QFile::exists(path))
                result.addFile(path, QSize(size, size));
        }
        return result;
    }();
    return icon;
}

QString localAppDataDir()
{
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/AppData/Local");
    return base;
}

QString dataRoot()
{
    return localAppDataDir() + QLatin1Char('/') + id();
}

void setAutostart(bool on)
{
    const QString exe = QCoreApplication::applicationFilePath();
    if (exe.isEmpty())
        return;
    HKEY key = openRunKey(KEY_SET_VALUE);
    if (!key)
        return;
    if (on) {
        const std::wstring quoted = L"\"" + exe.toStdWString() + L"\"";
        RegSetValueExW(key, kAutostartValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(quoted.c_str()),
                       DWORD((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAutostartValue);
    }
    RegCloseKey(key);
}

bool autostartEnabled()
{
    HKEY key = openRunKey(KEY_QUERY_VALUE);
    if (!key)
        return false;
    const bool enabled = runValueExists(key, kAutostartValue);
    RegCloseKey(key);
    return enabled;
}

} // namespace appinfo

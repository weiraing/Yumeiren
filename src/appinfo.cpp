#include "appinfo.h"

#include "config/AppConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <windows.h>

namespace {

// Value name under HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
constexpr wchar_t kAutostartValue[] = L"Yumeiren";
// Names used by the pre-rename builds; removed on the first run of this one.
const wchar_t *const kLegacyAutostartValues[] = {L"FolderBgStudio", L"YuMeiren"};

QStringList g_migrationNotes;

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

void removeRunValue(HKEY key, const wchar_t *name)
{
    if (runValueExists(key, name))
        RegDeleteValueW(key, name);
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

QString legacyDataRoot()
{
    return localAppDataDir() + QLatin1Char('/') + QString::fromLatin1(kLegacyId);
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

QStringList migrateLegacy()
{
    if (!g_migrationNotes.isEmpty())
        return g_migrationNotes;
    QStringList notes;
    const QString legacyId = QString::fromLatin1(kLegacyId);

    // 1) preferences: HKCU\Software\FolderBgStudio -> 统一配置(config/.ini)。
    //    The old keys stay in place so an older build still works.
    QSettings legacy(legacyId, legacyId);
    const QStringList keys = legacy.allKeys();
    AppConfig &current = AppConfig::instance();
    if (!keys.isEmpty() && current.allKeys().isEmpty()) {
        for (const QString &key : keys)
            current.setValue(key, legacy.value(key));
        current.save();
        notes << QStringLiteral("已导入旧版 %1 的 %2 项设置。").arg(legacyId).arg(keys.size());
    }

    // 2) cached backgrounds. The hook DLLs are re-extracted on the next apply,
    //    only the rendered images are worth carrying over.
    QDir from(legacyDataRoot() + QStringLiteral("/bg"));
    if (from.exists()) {
        QDir to(dataRoot() + QStringLiteral("/bg"));
        to.mkpath(QStringLiteral("."));
        int copied = 0;
        const QList<QFileInfo> files = from.entryInfoList(QDir::Files);
        for (const QFileInfo &fi : files) {
            const QString target = to.filePath(fi.fileName());
            if (QFileInfo::exists(target))
                continue;
            if (QFile::copy(fi.absoluteFilePath(), target))
                ++copied;
        }
        if (copied > 0)
            notes << QStringLiteral("已迁移 %1 个背景缓存文件。").arg(copied);
    }

    // 3) autostart: a legacy Run entry points at the pre-rename executable path.
    HKEY key = openRunKey(KEY_READ | KEY_WRITE);
    if (key) {
        bool legacyEntry = false;
        for (const wchar_t *name : kLegacyAutostartValues) {
            if (runValueExists(key, name)) {
                legacyEntry = true;
                break;
            }
        }
        if (legacyEntry) {
            const bool wasEnabled = autostartEnabled();
            for (const wchar_t *name : kLegacyAutostartValues)
                removeRunValue(key, name);
            if (wasEnabled)
                setAutostart(true);
            else
                notes << QStringLiteral("已清理旧版开机自启项。");
        }
        RegCloseKey(key);
    }

    g_migrationNotes = notes;
    return g_migrationNotes;
}

const QStringList &migrationNotes()
{
    return g_migrationNotes;
}

} // namespace appinfo

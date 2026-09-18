#ifndef APPINFO_H
#define APPINFO_H

#include <QSettings>
#include <QString>
#include <QStringList>

// Product identity in one place: the executable name, the settings keys, the
// data folder and the autostart entry all come from these constants so they
// cannot drift apart again.
//
//   Yumeiren / 虞美人   (renamed from the FolderBgStudio prototype)
namespace appinfo {

inline constexpr const char kId[] = "Yumeiren";
inline constexpr const char kLegacyId[] = "FolderBgStudio";

QString id();          // "Yumeiren"
QString displayName(); // 品牌名(侧边栏/自绘标题栏): "虞美人"
QString windowTitle(); // 原生窗口标题(任务管理器窗口行/单实例唤起匹配): "Yumeiren"

// 完整版本号，如 "1.0.0"（发布版）或 "1.0.0+67.g3f9a1c2.dirty"（本地开发版）。
// 数值由构建期生成（cmake/Version.cmake → <build>/generated/YumeirenVersion.h），
// 源头是仓库根目录的 VERSION 文件或 git 标签 —— 代码里不要硬写版本号。
QString version();

QString localAppDataDir();
QString dataRoot();        // %LOCALAPPDATA%\Yumeiren
QString legacyDataRoot();  // %LOCALAPPDATA%\FolderBgStudio (migration only)


// "Run at logon" entry in HKCU\...\CurrentVersion\Run.
void setAutostart(bool on);
bool autostartEnabled();

// Import what the pre-rename build left behind: settings keys, cached images
// and the autostart entry. Safe to call on every start; it only acts once.
// Returns human readable notes for the log line.
QStringList migrateLegacy();
const QStringList &migrationNotes();

} // namespace appinfo

#endif // APPINFO_H

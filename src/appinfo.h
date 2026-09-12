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
QString displayName(); // text shown in the title bar and the sidebar

QString localAppDataDir();
QString dataRoot();        // %LOCALAPPDATA%\Yumeiren
QString legacyDataRoot();  // %LOCALAPPDATA%\FolderBgStudio (migration only)

QSettings settings();      // HKCU\Software\Yumeiren\Yumeiren

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

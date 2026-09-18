#ifndef APPINFO_H
#define APPINFO_H

#include <QString>

// Product identity in one place: the executable name, the settings keys, the
// data folder and the autostart entry all come from these constants so they
// cannot drift apart again.
namespace appinfo {

inline constexpr const char kId[] = "Yumeiren";

QString id();          // "Yumeiren"
QString displayName(); // 品牌名(侧边栏/自绘标题栏): "虞美人"
QString windowTitle(); // 原生窗口标题(任务管理器窗口行/单实例唤起匹配): "Yumeiren"

// 完整版本号，如 "1.0.0"（发布版）或 "1.0.0+67.g3f9a1c2.dirty"（本地开发版）。
// 数值由构建期生成（cmake/Version.cmake → <build>/generated/YumeirenVersion.h），
// 源头是仓库根目录的 VERSION 文件或 git 标签 —— 代码里不要硬写版本号。
QString version();

QString localAppDataDir();
// %LOCALAPPDATA%\Yumeiren —— 只用来指认**本程序曾经使用过的**数据目录位置：Engine 拿它
// 拼出旧的 DLL 目录，好在界面上说清「旧目录里那份不会自动删，确认正常后可手工清理」。
// 程序自己的配置与缓存都在 <程序目录>/.cache 下（见 CachePaths）。
QString dataRoot();

// "Run at logon" entry in HKCU\...\CurrentVersion\Run.
void setAutostart(bool on);
bool autostartEnabled();

} // namespace appinfo

#endif // APPINFO_H

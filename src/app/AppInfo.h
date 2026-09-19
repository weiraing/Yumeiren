#ifndef APPINFO_H
#define APPINFO_H

#include <QIcon>
#include <QString>

// 产品标识集中在一处：可执行名、设置键、数据目录、自启动项都从这里取，免得再走散。
namespace appinfo {

inline constexpr const char kId[] = "Yumeiren";

QString id();
QString displayName(); // 品牌名(侧边栏/自绘标题栏): "虞美人"
QString windowTitle(); // 原生窗口标题(任务管理器窗口行/单实例唤起匹配): "Yumeiren"


QIcon appIcon();

// 完整版本号，如 "1.0.0" 或 "1.0.0+67.g3f9a1c2.dirty"(本地开发版)。构建期由
// cmake/Version.cmake 生成，源头是 VERSION 文件或 git 标签——代码里不要硬写。
QString version();

QString localAppDataDir();

QString dataRoot();

void setAutostart(bool on);
bool autostartEnabled();

} // namespace appinfo

#endif // APPINFO_H

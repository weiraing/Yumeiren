#include "engine/Engine.h"

#include "app/AppInfo.h"
#include "core/CachePaths.h"
#include "core/Diagnostics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QThread>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

namespace {
// COM CLSID of the FolderExtension implemented by each hook DLL.
constexpr char kClsidImage[]  = "{ED15A97D-FE3E-4CDE-98FF-BC46B02896B0}"; // ExplorerBgTool.dll (explorerTool)
constexpr char kClsidEffect[] = "{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}"; // ExplorerBlurMica.dll

QString boolStr(bool v) { return v ? QStringLiteral("true") : QStringLiteral("false"); }
}

Engine &Engine::instance()
{
    static Engine e;
    return e;
}

QString Engine::dllRoot()
{
    // Hook DLL 随缓存一起搬进程序目录：便携、且不再往 %LOCALAPPDATA% 写任何东西。
    // 位置只取 applicationDirPath()，与当前工作目录无关。
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("dll"));
}

QString Engine::legacyDllRoot()
{
    // 迁移前的旧位置，仅用于识别残留注册。
    return appinfo::dataRoot() + QStringLiteral("/dll");
}

QString Engine::imageDllDir()  { return dllRoot() + QStringLiteral("/ExplorerBgTool"); }
QString Engine::effectDllDir() { return dllRoot() + QStringLiteral("/ExplorerBlurMica"); }
QString Engine::imageDllPath()  { return imageDllDir() + QStringLiteral("/ExplorerBgTool.dll"); }
QString Engine::effectDllPath() { return effectDllDir() + QStringLiteral("/ExplorerBlurMica.dll"); }
QString Engine::imageIniPath()  { return imageDllDir() + QStringLiteral("/config.ini"); }
QString Engine::effectIniPath() { return effectDllDir() + QStringLiteral("/config.ini"); }
QString Engine::bgDir()   { return CachePaths::media(); }
QString Engine::processedImagePath() { return bgDir() + QStringLiteral("/bg_custom.png"); }
QString Engine::wallpaperPath() { return bgDir() + QStringLiteral("/current_wallpaper.jpg"); }
QString Engine::imagePoolDir() { return CachePaths::imagePool(); }

void Engine::ensureDataDirs()
{
    // 渲染出的背景图是可重新生成的缓存，落在 <程序目录>/.cache/media。
    QString cacheError;
    if (!CachePaths::ensureDirectories(&cacheError))
        videodiag::log(videodiag::Level::Error, cacheError, QStringLiteral("Cache"));
    QDir().mkpath(dllRoot());
    QDir().mkpath(imageDllDir());
    QDir().mkpath(effectDllDir());
}

bool Engine::extractDlls(QString *error)
{
    struct Entry { const char *res; QString target; };
    const Entry entries[] = {
        {":/dlls/ExplorerBgTool.dll", imageDllPath()},
        {":/dlls/ExplorerBlurMica.dll", effectDllPath()},
    };
    for (const Entry &e : entries) {
        QFile src(QString::fromLatin1(e.res));
        QFile dst(e.target);
        if (dst.exists() && dst.size() == src.size())
            continue; // already deployed
        if (!src.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("内置资源缺失: %1").arg(e.res);
            return false;
        }
        // A stale explorer.exe may still hold the old DLL; keep the old file
        // in that case - config.ini changes still take effect.
        if (dst.exists() && !dst.remove())
            continue;
        if (!dst.open(QIODevice::WriteOnly)) {
            if (error) *error = QStringLiteral("无法写入 %1").arg(e.target);
            return false;
        }
        dst.write(src.readAll());
        dst.close();
    }
    return true;
}

QString Engine::folderExtKey(const char *clsid)
{
    return QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\Drive\\shellex\\FolderExtensions\\")
        + QString::fromLatin1(clsid);
}

ComponentStatus Engine::queryStatus(const char *clsid, const QString &ourDll)
{
    ComponentStatus st;
    QSettings s(folderExtKey(clsid), QSettings::NativeFormat);
    QString path = s.value(QStringLiteral(".")).toString(); // default value
    if (path.isEmpty()) {
        // Some versions store the path in InprocServer32 of the CLSID key.
        QSettings c(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID\\")
                        + QString::fromLatin1(clsid) + QStringLiteral("\\InprocServer32"),
                    QSettings::NativeFormat);
        path = c.value(QStringLiteral(".")).toString();
    }
    st.path = path;
    if (path.isEmpty())
        return st;
    st.registered = true;
    const QString native = QDir::fromNativeSeparators(path.toLower());
    const QString ours = QDir::fromNativeSeparators(ourDll.toLower());
    st.ours = (native == ours);
    st.foreign = !st.ours;
    st.dangling = !QFileInfo::exists(path);
    // 路径迁移后旧注册会指向 %LOCALAPPDATA%\Yumeiren/dll：单独标出来，
    // 免得界面把它当成"其他程序占用"，用户不知道该重新点一次应用。
    const QString legacy = QDir::fromNativeSeparators(legacyDllRoot().toLower());
    st.stale = !st.ours
               && (native == legacy
                   || native.startsWith(legacy + QStringLiteral("/")));
    return st;
}

ComponentStatus Engine::imageStatus() const  { return queryStatus(kClsidImage, imageDllPath()); }
ComponentStatus Engine::effectStatus() const { return queryStatus(kClsidEffect, effectDllPath()); }

QString Engine::statusText(const ComponentStatus &st)
{
    if (!st.registered)
        return QStringLiteral("未启用");
    if (st.dangling)
        return QStringLiteral("残留注册(文件缺失)");
    if (st.ours)
        return QStringLiteral("已启用");
    if (st.stale)
        return QStringLiteral("旧目录注册(需重新应用)");
    return QStringLiteral("其他程序占用");
}

bool Engine::isElevated()
{
    return ::IsUserAnAdmin() != FALSE;
}

bool Engine::runRegsvr32(const QString &dll, bool unregister, QString *error)
{
    const QString exe = QDir::toNativeSeparators(
        QStringLiteral("%1\\System32\\regsvr32.exe").arg(qEnvironmentVariable("SystemRoot")));
    const QString nativeDll = QDir::toNativeSeparators(dll);
    // QProcess quotes arguments itself; do not pre-quote the path.
    QStringList args{QStringLiteral("/s")};
    if (unregister)
        args << QStringLiteral("/u");
    args << nativeDll;
    const QString params = unregister ? QStringLiteral("/s /u \"%1\"").arg(nativeDll)
                                      : QStringLiteral("/s \"%1\"").arg(nativeDll);

    if (isElevated()) {
        int code = QProcess::execute(exe, args);
        if (code != 0) {
            if (error) *error = QStringLiteral("regsvr32 失败(退出码 %1)").arg(code);
            return false;
        }
        return true;
    }

    // Not elevated: ask via UAC.
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    std::wstring exeW = exe.toStdWString();
    std::wstring parW = params.toStdWString();
    sei.lpFile = exeW.c_str();
    sei.lpParameters = parW.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        if (error) *error = QStringLiteral("需要管理员权限(用户取消了 UAC 或提权失败)");
        return false;
    }
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0) {
        if (error) *error = QStringLiteral("regsvr32 失败(退出码 %1)").arg(code);
        return false;
    }
    return true;
}

bool Engine::registerDllInternal(const char *clsid, const QString &dll, QString *error)
{
    if (!QFileInfo::exists(dll)) {
        if (error) *error = QStringLiteral("DLL 不存在: %1").arg(dll);
        return false;
    }
    // If the CLSID is registered to a missing file, remove the leftover first
    // (regsvr32 /u cannot load a file that no longer exists).
    ComponentStatus st = queryStatus(clsid, dll);
    if (st.registered && !st.ours && st.dangling) {
        QSettings s(folderExtKey(clsid), QSettings::NativeFormat);
        s.clear();
        QSettings c(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID\\")
                        + QString::fromLatin1(clsid),
                    QSettings::NativeFormat);
        c.remove(QString());
    }
    return runRegsvr32(dll, false, error);
}

bool Engine::unregisterDllInternal(const char *clsid, const QString &dll, QString *error)
{
    ComponentStatus st = queryStatus(clsid, dll);
    if (!st.registered)
        return true; // nothing to do
    if (st.ours && QFileInfo::exists(dll))
        return runRegsvr32(dll, true, error);
    // Foreign or dangling registration: delete the keys directly.
    QSettings s(folderExtKey(clsid), QSettings::NativeFormat);
    s.clear();
    QSettings c(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID\\")
                    + QString::fromLatin1(clsid),
                QSettings::NativeFormat);
    c.remove(QString());
    return !queryStatus(clsid, dll).registered;
}

bool Engine::registerImageDll(QString *error)
{
    return registerDllInternal(kClsidImage, imageDllPath(), error);
}

bool Engine::registerEffectDll(QString *error)
{
    return registerDllInternal(kClsidEffect, effectDllPath(), error);
}

bool Engine::unregisterImageDll(QString *error)
{
    return unregisterDllInternal(kClsidImage, imageDllPath(), error);
}

bool Engine::unregisterEffectDll(QString *error)
{
    return unregisterDllInternal(kClsidEffect, effectDllPath(), error);
}

bool Engine::writeIniFile(const QString &path, const QString &content, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("无法写入 %1").arg(path);
        return false;
    }
    f.write(content.toUtf8());
    f.close();
    return true;
}

bool Engine::writeImageConfig(const QString &imageDir, int posType, int imgAlpha,
                              bool folderExt, bool random, QString *error)
{
    ensureDataDirs();
    if (!extractDlls(error))
        return false;
    // Assembled by concatenation: user paths may contain '%' characters that
    // QString::arg would misinterpret.
    QString ini;
    ini += QStringLiteral("[load]\r\n");
    ini += QStringLiteral("folderExt=") + boolStr(folderExt) + QStringLiteral("\r\n");
    ini += QStringLiteral("noerror=true\r\n");
    ini += QStringLiteral("[image]\r\n");
    ini += QStringLiteral("random=") + boolStr(random) + QStringLiteral("\r\n");
    ini += QStringLiteral("custom=false\r\n");
    ini += QStringLiteral("posType=") + QString::number(qBound(0, posType, 6)) + QStringLiteral("\r\n");
    ini += QStringLiteral("imgAlpha=") + QString::number(qBound(0, imgAlpha, 255)) + QStringLiteral("\r\n");
    ini += QStringLiteral("folder=") + QDir::toNativeSeparators(
               QDir(imageDir).absolutePath()) + QStringLiteral("\r\n");
    return writeIniFile(imageIniPath(), ini, error);
}

bool Engine::writeEffectConfig(const EffectConfig &cfg, QString *error)
{
    ensureDataDirs();
    if (!extractDlls(error))
        return false;
    QString ini;
    ini += QStringLiteral("[config]\r\n");
    ini += QStringLiteral("effect=") + QString::number(cfg.effect) + QStringLiteral("\r\n");
    ini += QStringLiteral("clearAddress=") + boolStr(cfg.clearAddress) + QStringLiteral("\r\n");
    ini += QStringLiteral("clearBarBg=") + boolStr(cfg.clearBarBg) + QStringLiteral("\r\n");
    ini += QStringLiteral("clearWinUIBg=") + boolStr(cfg.clearWinUIBg) + QStringLiteral("\r\n");
    ini += QStringLiteral("showLine=") + boolStr(cfg.showLine) + QStringLiteral("\r\n");
    ini += QStringLiteral("[light]\r\n");
    ini += QStringLiteral("r=%1\r\ng=%2\r\nb=%3\r\na=%4\r\n")
               .arg(cfg.lightR).arg(cfg.lightG).arg(cfg.lightB).arg(cfg.lightA);
    ini += QStringLiteral("[dark]\r\n");
    ini += QStringLiteral("r=%1\r\ng=%2\r\nb=%3\r\na=%4\r\n")
               .arg(cfg.darkR).arg(cfg.darkG).arg(cfg.darkB).arg(cfg.darkA);
    return writeIniFile(effectIniPath(), ini, error);
}

bool Engine::restartExplorer(QString *error)
{
    Q_UNUSED(error);
    QProcess::execute(QStringLiteral("taskkill"),
                      {QStringLiteral("/f"), QStringLiteral("/im"), QStringLiteral("explorer.exe")});
    // Wait until the process is really gone so the restart is deterministic.
    for (int i = 0; i < 50; ++i) {
        bool alive = false;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = {};
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) { alive = true; break; }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        if (!alive)
            break;
        QThread::msleep(100);
    }
    QProcess::startDetached(QStringLiteral("explorer.exe"), QStringList());
    QThread::msleep(600);
    return true;
}

QString Engine::windowsProductName()
{
    QSettings s(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"),
                QSettings::NativeFormat);
    bool ok = false;
    const int build = s.value(QStringLiteral("CurrentBuildNumber")).toString().toInt(&ok);
    QString name = s.value(QStringLiteral("ProductName")).toString();
    if (name.isEmpty())
        name = QStringLiteral("Windows");
    // Win11 keeps ProductName "Windows 10 ..." in the registry; go by build.
    if (ok && build >= 22000)
        name = QStringLiteral("Windows 11");
    QString display = s.value(QStringLiteral("DisplayVersion")).toString();
    if (!display.isEmpty())
        name += QStringLiteral(" ") + display;
    return name;
}

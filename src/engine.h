#ifndef ENGINE_H
#define ENGINE_H

#include <QString>
#include <QStringList>

struct ComponentStatus
{
    bool registered = false;      // CLSID present in registry
    bool ours = false;            // registered path == our dll path
    bool foreign = false;         // registered but path points elsewhere
    bool dangling = false;        // registered but file does not exist
    QString path;                 // registered path (raw)
};

// Backend: everything that touches the system (registry / regsvr32 / files / explorer).
class Engine
{
public:
    static Engine &instance();

    // --- paths ---
    static QString dataRoot();            // %LOCALAPPDATA%\Yumeiren
    static QString imageDllDir();         // dll/ExplorerBgTool
    static QString effectDllDir();        // dll/ExplorerBlurMica
    static QString imageDllPath();
    static QString effectDllPath();
    static QString imageIniPath();
    static QString effectIniPath();
    static QString bgDir();               // processed images
    static QString processedImagePath();
    static QString wallpaperPath();

    void ensureDataDirs();
    // Extract bundled DLLs from resources. A stale explorer.exe may hold the
    // old DLL locked; in that case the existing file is kept.
    bool extractDlls(QString *error);

    // --- image background (ExplorerBgTool.dll / explorerTool) ---
    bool writeImageConfig(const QString &imagePath, int posType, int imgAlpha,
                          bool folderExt, QString *error);
    bool registerImageDll(QString *error);
    ComponentStatus imageStatus() const;

    // --- blur/mica effects (ExplorerBlurMica.dll) ---
    struct EffectConfig
    {
        int effect = 1;
        bool clearAddress = true;
        bool clearBarBg = true;
        bool clearWinUIBg = true;
        bool showLine = false;
        int lightR = 255, lightG = 255, lightB = 255, lightA = 200;
        int darkR = 0, darkG = 0, darkB = 0, darkA = 120;
    };
    bool writeEffectConfig(const EffectConfig &cfg, QString *error);
    bool registerEffectDll(QString *error);
    ComponentStatus effectStatus() const;

    // --- uninstall ---
    bool unregisterImageDll(QString *error);
    bool unregisterEffectDll(QString *error);

    // --- system ---
    static bool isElevated();
    static bool restartExplorer(QString *error);
    static QString windowsProductName();
    static QString statusText(const ComponentStatus &st);

private:
    Engine() = default;

    static QString folderExtKey(const char *clsid);
    static ComponentStatus queryStatus(const char *clsid, const QString &ourDll);
    bool runRegsvr32(const QString &dll, bool unregister, QString *error);
    bool registerDllInternal(const char *clsid, const QString &dll, QString *error);
    bool unregisterDllInternal(const char *clsid, const QString &dll, QString *error);
    static bool writeIniFile(const QString &path, const QString &content, QString *error);
};

#endif // ENGINE_H

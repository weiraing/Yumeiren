#ifndef ENGINE_H
#define ENGINE_H

#include <QStringList>

/**
 * @brief DLL 注册状态查询结果。
 *
 * registered 表示 CLSID 是否存在于注册表；ours/foreign/dangling/stale 进一步细分
 * 注册表指向的 DLL 是否有效。
 */
struct ComponentStatus
{
    bool registered = false;      // CLSID present in registry
    bool ours = false;            // registered path == our dll path
    bool foreign = false;         // registered but path points elsewhere
    bool dangling = false;        // registered but file does not exist
    bool stale = false;           // registered path still points at the old
                                  // %LOCALAPPDATA%\Yumeiren\dll location
    QString path;                 // registered path (raw)
};

// Backend: everything that touches the system (registry / regsvr32 / files / explorer).
class Engine
{
public:
    static Engine &instance();

    // Hook DLL 及其 config.ini 随缓存迁进程序目录，不再用 %LOCALAPPDATA%。注册表里存的是
    // 绝对路径，路径一变就要重新注册(下一次「应用」会自动完成，需管理员权限)。
    static QString dllRoot();             // <程序目录>/dll
    static QString imageDllDir();         // dll/ExplorerBgTool
    static QString effectDllDir();        // dll/ExplorerBlurMica
    // 迁移前的旧位置，只用于识别残留注册，不再写入。
    static QString legacyDllRoot();       // %LOCALAPPDATA%\Yumeiren/dll
    static QString imageDllPath();
    static QString effectDllPath();
    static QString imageIniPath();
    static QString effectIniPath();
    static QString bgDir();               // 处理后背景图缓存 = CachePaths::renderedBg()
    static QString processedImagePath();
    // 「随机」模式图片池 = CachePaths::imagePool()。与 bgDir 分开，避免池里的图
    // 被「单图」模式误当候选。
    static QString imagePoolDir();

    void ensureDataDirs();
    // Extract bundled DLLs from resources into dllRoot(). A stale explorer.exe
    // may hold an existing DLL locked; in that case the file on disk is kept.
    // 调用前必须先 ensureDataDirs，本函数不自行建目录以免递归。
    bool extractDlls(QString *error);

    // imageDir 是 DLL 扫描图片的目录(只认 *.png / *.jpg，不递归)。
    // random=false：目录里取固定一张(单图)；random=true：每打开一个资源管理器
    // 窗口都从目录里随机换一张。
    bool writeImageConfig(const QString &imageDir, int posType, int imgAlpha,
                          bool folderExt, bool random, QString *error);
    bool registerImageDll(QString *error);
    ComponentStatus imageStatus() const;

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

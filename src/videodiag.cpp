#include "videodiag.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QSettings>
#include <QTimer>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace videodiag {

namespace {

QMutex g_mutex;
QFile g_file;
bool g_diag = false;
constexpr qint64 kMaxLogBytes = 1024 * 1024; // 1MB 滚动

QString levelTag(Level lv)
{
    switch (lv) {
    case Level::Error:   return QStringLiteral("E");
    case Level::Warning: return QStringLiteral("W");
    case Level::Info:    return QStringLiteral("I");
    case Level::Debug:   return QStringLiteral("D");
    }
    return QStringLiteral("?");
}

QString logPath()
{
    // 与 appinfo::dataRoot() 同根，但避免反向依赖：直接按同一规则拼接
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/AppData/Local");
    return base + QStringLiteral("/Yumeiren/logs/videowallpaper.log");
}

void rotateIfNeeded()
{
    if (g_file.size() < kMaxLogBytes)
        return;
    const QString path = g_file.fileName();
    g_file.close();
    QFile::remove(path + QStringLiteral(".old"));
    QFile::rename(path, path + QStringLiteral(".old"));
    g_file.setFileName(path);
    g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}

// 内部写入(调用方必须已持有 g_mutex)：init 与 log 共用，避免非递归锁重入死锁
void writeLine(Level lv, const QString &msg)
{
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    g_file.write(QStringLiteral("%1 [%2] %3\n")
                     .arg(stamp, levelTag(lv), msg).toUtf8());
    g_file.flush();
    rotateIfNeeded();
}

int currentThreadCount()
{
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return -1;
    int count = 0;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid)
                ++count;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
}

} // namespace

void init()
{
    QMutexLocker lock(&g_mutex);
    // 幂等：main() 与单例构造都可能触发，只初始化一次
    static bool inited = false;
    if (inited)
        return;
    inited = true;

    // 诊断开关：环境变量优先，其次 HKCU video/diag
    g_diag = qEnvironmentVariableIntValue("YUMEIREN_DIAG") != 0;
    if (!g_diag)
        g_diag = QSettings(QStringLiteral("Yumeiren"), QStringLiteral("Yumeiren"))
                     .value(QStringLiteral("video/diag"), false).toBool();

    const QString path = logPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    g_file.setFileName(path);
    if (!g_file.isOpen())
        g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    writeLine(Level::Info, QStringLiteral("==== 视频壁纸模块启动 (diag=%1) ====")
                               .arg(g_diag ? QStringLiteral("on") : QStringLiteral("off")));
}

bool diagEnabled()
{
    QMutexLocker lock(&g_mutex);
    return g_diag;
}

void log(Level lv, const QString &msg)
{
    QMutexLocker lock(&g_mutex);
    // 默认 Info+Warning+Error；Debug 仅诊断模式(任务书 10.4)
    if (lv == Level::Debug && !g_diag)
        return;
    if (!g_file.isOpen())
        return;
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    g_file.write(QStringLiteral("%1 [%2] %3\n")
                     .arg(stamp, levelTag(lv), msg).toUtf8());
    g_file.flush();
    rotateIfNeeded();
}

void startDiagSampling()
{
    if (!diagEnabled())
        return;
    // 采样间隔由 YUMEIREN_DIAG_SAMPLE_MS 指定，默认关闭(0)；下限 1s，
    // 高频采样会干扰播放(任务书 10.3：日志不能明显影响播放)。
    int intervalMs = qEnvironmentVariableIntValue("YUMEIREN_DIAG_SAMPLE_MS");
    if (intervalMs <= 0)
        return;
    intervalMs = qMax(1000, intervalMs);

    auto *timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, timer, [] {
        static QElapsedTimer clock;
        if (!clock.isValid())
            clock.start();
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        pmc.cb = sizeof(pmc);
        QString mem;
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                                 sizeof(pmc))) {
            mem = QStringLiteral("ws=%1MB private=%2MB")
                      .arg(quint64(pmc.WorkingSetSize) / 1024 / 1024)
                      .arg(quint64(pmc.PrivateUsage) / 1024 / 1024);
        }
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        log(Level::Debug, QStringLiteral("sample t=%1s %2 threads=%3 handles=%4")
                              .arg(qint64(clock.elapsed() / 1000))
                              .arg(mem)
                              .arg(currentThreadCount())
                              .arg(handles));
    });
    timer->start(intervalMs);
}

} // namespace videodiag

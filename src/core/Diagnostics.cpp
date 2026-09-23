#include "core/Diagnostics.h"

#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/CachePaths.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QObject>
#include <QTimer>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace applog {

namespace {

QMutex g_mutex;
QFile g_file;
bool g_diag = false;
constexpr qint64 kMaxLogBytes = 1024 * 1024; // 1MB 滚动

// 分段测量一律用 QElapsedTimer 单调时钟，不用系统时间差；原点取进程创建时刻。
QElapsedTimer *g_bootClock = nullptr;
qint64 g_processStartOffsetMs = 0;
qint64 g_lastStageMs = 0;

QString levelTag(Level lv)
{
    switch (lv) {
    case Level::Error:   return QStringLiteral("ERROR");
    case Level::Warning: return QStringLiteral("WARNING");
    case Level::Info:    return QStringLiteral("INFO");
    case Level::Debug:   return QStringLiteral("DEBUG");
    }
    return QStringLiteral("?");
}

// 进程已存活毫秒数(此刻单调时钟尚未开始)。系统时钟出现过非单调跳变，故结果
// 不可信时直接丢弃(返回 -1)。
qint64 processAgeMs()
{
    FILETIME creation = {}, exitT = {}, kernel = {}, user = {}, now = {};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitT, &kernel, &user))
        return -1;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = creation.dwLowDateTime;
    a.HighPart = creation.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    const qint64 ms = qint64((b.QuadPart - a.QuadPart) / 10000);
    return (ms >= 0 && ms < 600000) ? ms : -1;
}

qint64 bootElapsedMs()
{
    return g_bootClock ? g_processStartOffsetMs + g_bootClock->elapsed() : 0;
}

QString logPath()
{
    // 诊断日志属于可重新生成的运行时数据，随缓存落在 <程序目录>/.cache/logs。
    return QDir(CachePaths::logs()).filePath(QStringLiteral("videowallpaper.log"));
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

// 调用方必须已持 g_mutex：init 与 log 共用，避免非递归锁重入死锁
void writeLine(Level lv, const QString &module, const QString &msg)
{
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    g_file.write(QStringLiteral("[%1][%2][tid=%3][%4] %5\n")
                     .arg(stamp, levelTag(lv))
                     .arg(quint32(GetCurrentThreadId()))
                     .arg(module.isEmpty() ? QStringLiteral("VideoWallpaper") : module,
                          msg)
                     .toUtf8());
    g_file.flush();
    rotateIfNeeded();
}

// 调用方必须已持锁；delta 为距上一阶段的毫秒数
void stageAt(Level lv, const QString &name, qint64 nowMs)
{
    const qint64 delta = nowMs - g_lastStageMs;
    g_lastStageMs = nowMs;
    writeLine(lv, QStringLiteral("Startup"),
              QStringLiteral("阶段 %1 用时 +%2ms 累计 %3ms")
                  .arg(name).arg(delta).arg(nowMs));
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
    // 幂等：main() 与单例构造都可能触发
    static bool inited = false;
    if (inited)
        return;
    inited = true;

    // 诊断开关：环境变量优先，其次 HKCU video/diag
    g_diag = qEnvironmentVariableIntValue("YUMEIREN_DIAG") != 0;
    if (!g_diag)
        g_diag = AppConfig::instance()
                     .value(ConfigKeys::Video::Diag, false).toBool();

    QString path = logPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    g_file.setFileName(path);
    if (!g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        // 主日志被其他实例占用时回退到按 PID 独立的文件：二次启动诊断不能丢日志。
        path = QStringLiteral("%1.%2.log").arg(path).arg(GetCurrentProcessId());
        g_file.setFileName(path);
        g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }
    if (!g_bootClock) {
        g_bootClock = new QElapsedTimer();
        g_bootClock->start();
        const qint64 age = processAgeMs();
        if (age >= 0)
            g_processStartOffsetMs = age;
    }
    writeLine(Level::Info, QStringLiteral("App"),
              QStringLiteral("==== 进程启动 pid=%1 diag=%2 ver=%3 ====")
                  .arg(GetCurrentProcessId())
                  .arg(g_diag ? QStringLiteral("on") : QStringLiteral("off"))
                  .arg(QCoreApplication::applicationVersion()));
    stageAt(Level::Info, QStringLiteral("进程启动→诊断初始化"), bootElapsedMs());
}

bool diagEnabled()
{
    QMutexLocker lock(&g_mutex);
    return g_diag;
}

void log(Level lv, const QString &msg, const QString &module)
{
    QMutexLocker lock(&g_mutex);
    // 默认 Info+Warning+Error；Debug 仅诊断模式
    if (lv == Level::Debug && !g_diag)
        return;
    if (!g_file.isOpen())
        return;
    writeLine(lv, module, msg);
}

void stage(const QString &name)
{
    QMutexLocker lock(&g_mutex);
    if (!g_file.isOpen() || !g_bootClock)
        return;
    stageAt(Level::Info, name, bootElapsedMs());
}

qint64 elapsedMs()
{
    QMutexLocker lock(&g_mutex);
    return g_bootClock ? bootElapsedMs() : -1;
}

void logObjectEvent(const char *action, const QObject *obj, const QString &detail)
{
    if (!obj)
        return;
    // 对象地址用于把创建/销毁两端的同一对象对上；只记地址与类名，不含路径或
    // 用户数据。DEBUG 级：默认不写盘。
    log(Level::Debug,
        QStringLiteral("%1 %2=0x%3 %4")
            .arg(QString::fromLatin1(action),
                 QString::fromLatin1(obj->metaObject()->className()))
            .arg(quintptr(obj), 0, 16)
            .arg(detail),
        QStringLiteral("Lifecycle"));
}

void startDiagSampling()
{
    if (!diagEnabled())
        return;
    // 间隔由 YUMEIREN_DIAG_SAMPLE_MS 指定，默认关闭(0)；下限 1s，高频采样会干扰播放。
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
        log(Level::Debug,
            QStringLiteral("sample t=%1s %2 threads=%3 handles=%4")
                .arg(qint64(clock.elapsed() / 1000))
                .arg(mem)
                .arg(currentThreadCount())
                .arg(handles),
            QStringLiteral("Resource"));
    });
    timer->start(intervalMs);
}

} // namespace applog

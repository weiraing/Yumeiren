#include "app/AppInfo.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/CachePaths.h"
#include "core/Diagnostics.h"
#include "kanban/ModelThumbJob.h"
#include "platform/windows/desktopmount.h"
#include "ui/MainWindow.h"
#include "wallpaper/VideoWallpaper.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QStyleFactory>
#include <QThread>

int main(int argc, char *argv[])
{
    // 必须早于 QApplication 构造：上屏会初始化已编译着色器的磁盘缓存
    // (QOpenGLProgramBinaryCache)，其 load() 持锁读盘；读盘一旦被卡(权限/网络盘/
    // 杀软)，主线程就在 rhiFlush 里死等这把锁，GUI 线程整个死锁。关掉只是让 Qt
    // 每次重编那几个上屏着色器(毫秒级)。
    qputenv("QT_DISABLE_SHADER_DISK_CACHE", QByteArrayLiteral("1"));

    QApplication app(argc, argv);
    QApplication::setOrganizationName(appinfo::id());
    QApplication::setApplicationName(appinfo::id());
    QApplication::setApplicationVersion(appinfo::version());
    // exe 内嵌的 .ico 归资源管理器/快捷方式，Qt 拿不到；且必须在建任何窗口之前。
    QApplication::setWindowIcon(appinfo::appIcon());

    // 必须先于 applog::init()：诊断日志本身就写在 .cache/logs 下；创建失败只
    // 报错，不回退 AppData。
    QString cacheError;
    const bool cacheReady = CachePaths::ensureDirectories(&cacheError);

    // 单实例守卫：两个实例会各建解码管线并互抢 WorkerW 挂载点。进程被强杀/崩溃后
    // 共享内存段会残留(引用计数无人递减)，导致之后永远"已经在运行"——attach+
    // detach 清掉残段后重试一次即可自愈。
    applog::init(); // 守卫阶段即可记录诊断(幂等)
    applog::stage(QStringLiteral("Qt 应用对象创建"));
    if (!cacheReady)
        applog::log(applog::Level::Error, cacheError, QStringLiteral("Cache"));

    // 隐藏的「生成模型预览图」模式：由本程序**再起一个自己的进程**渲染看板娘页的
    // 静态预览图。不能在当前进程开线程——Cubism 的着色器缓存是进程级单例，存的 GL
    // program id 只在创建它的上下文有效。详见 src/kanban/ModelThumbJob.h。
    //
    // 位置：在单实例守卫**之前**(该模式不建窗口/不挂壁纸，也不该被"已经在运行"挡住)，
    // 且在 AppConfig::load() **之前**(生成进程不该读写用户配置)。
    if (QCoreApplication::arguments().contains(QStringLiteral("--render-model-thumbs"))) {
        QString jobError;
        const int jobExit = kanban::runModelThumbJob(QCoreApplication::arguments(), &jobError);
        if (!jobError.isEmpty())
            applog::log(applog::Level::Warning, jobError, QStringLiteral("ModelThumb"));
        return jobExit;
    }

    AppConfig::instance().load(); // 主窗口创建前加载(含目录创建/校验/修复)
    applog::stage(QStringLiteral("配置加载(创建/校验/修复)"));

    // 资源友好模式(默认开)：限到 4 个逻辑核，优先分属 4 个不同物理核。必须在
    // QApplication 构造后立即设置，使后续线程继承掩码；video/affinityLimit=false 关。
    if (AppConfig::instance()
            .value(ConfigKeys::Video::AffinityLimit, true).toBool()) {
        if (winhelper::applyProcessAffinityLimit(4)) {
            applog::log(applog::Level::Info,
                QStringLiteral("资源友好模式: 进程已限制到 4 个逻辑核(尽量分属不同物理核)"),
                QStringLiteral("App"));
        }
    } else {
        applog::log(applog::Level::Info,
            QStringLiteral("资源友好模式: 已关闭(全核运行)"),
            QStringLiteral("App"));
    }
    if (!winhelper::acquireSingleInstanceLock()) {
        // 已有实例在运行：把它的主窗口调到最前，不弹窗打断。
        QString why;
        // 唤起偶发失败(窗口枚举/前台锁的时序竞争)——重试兜底
        bool activated = false;
        for (int attempt = 0; attempt < 3 && !activated; ++attempt) {
            if (attempt > 0) {
                QThread::msleep(300);
                why.clear();
            }
            activated =
                winhelper::activateExistingInstanceWindow(appinfo::windowTitle(), &why);
        }
        // 守卫结果写按 PID 独立文件：主日志可能被已运行实例锁定
        {
            QFile guardLog(QDir(CachePaths::logs())
                               .filePath(QStringLiteral("guard_%1.log")
                                             .arg(QCoreApplication::applicationPid())));
            if (guardLog.open(QIODevice::WriteOnly | QIODevice::Text))
                guardLog.write(QStringLiteral("%1|%2\n")
                                   .arg(QDateTime::currentDateTime()
                                            .toString(QStringLiteral("HH:mm:ss.zzz")),
                                        activated ? QStringLiteral("已唤起: ") + why
                                                  : QStringLiteral("唤起失败: ") + why)
                                   .toUtf8());
        }
        if (!activated) {
            applog::log(applog::Level::Warning,
                QStringLiteral("唤起已运行实例失败: %1").arg(why),
                QStringLiteral("App"));
            QMessageBox::information(nullptr, QStringLiteral("虞美人"),
                                     QStringLiteral("虞美人已经在运行。"));
        } else {
            applog::log(applog::Level::Info,
                QStringLiteral("已唤起已运行实例窗口: %1").arg(why),
                QStringLiteral("App"));
        }
        return 0;
    }

    applog::stage(QStringLiteral("单实例守卫"));

    // 尽早初始化(幂等)：默认 Info+，诊断模式经 YUMEIREN_DIAG=1 开启
    applog::init();

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFile qss(QStringLiteral(":/style.qss"));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
    applog::stage(QStringLiteral("样式与内嵌资源加载"));

    MainWindow w;
    applog::stage(QStringLiteral("主窗口构造完成(含 UI 初始化)"));
    w.show();
    applog::stage(QStringLiteral("主窗口首次显示"));

    // 内存实验探针：按 YUMEIREN_PROBE_STAGE 建立受控媒体栈状态(正常运行不设置)。
    if (const QString probeStage = qEnvironmentVariable("YUMEIREN_PROBE_STAGE");
        !probeStage.isEmpty())
        VideoWallpaper::instance().runProbeStage(probeStage);

    const int exitCode = app.exec();
    // 必须在 QApplication 与 MainWindow 析构之前主动卸载视频管线：函数内静态单例的
    // 析构由 CRT atexit 链驱动，跑在 main() 返回之后，那时 ~QApplication 已完成，
    // 任何 QWidget 调用(经 unmountWindow → winId())都会踩进 qApp==nullptr →
    // c0000005。详见 docs/crash_analysis.md。
    // 退出段不用 stage()：其差值是距上一次分段的时间，会把事件循环时长一起算进来。
    const qint64 shutdownBegin = applog::elapsedMs();
    VideoWallpaper::shutdown();
    applog::log(applog::Level::Info,
        QStringLiteral("事件循环结束 exit=%1 uptime=%2ms 退出收口 cost=%3ms")
            .arg(exitCode)
            .arg(applog::elapsedMs())
            .arg(applog::elapsedMs() - shutdownBegin),
        QStringLiteral("App"));
    return exitCode;
}

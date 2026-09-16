#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QThread>
#include <QMessageBox>
#include <QStyleFactory>

#include "app/AppInfo.h"
#include "core/CachePaths.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "ui/MainWindow.h"
#include "platform/windows/desktopmount.h"
#include "core/Diagnostics.h"
#include "wallpaper/VideoWallpaper.h"

int main(int argc, char *argv[])
{
    // 必须早于 QApplication 构造：Qt 的上屏路径(QRhiGles2)会初始化一个
    // 「已编译着色器二进制磁盘缓存」(QOpenGLProgramBinaryCache)，而它的 load()
    // 是**持锁做文件读写**的。只要那次读盘被卡住(权限受限、网络盘、杀软或沙箱的
    // 文件拦截)，这把锁就永远不还，主线程随后在 QPlatformBackingStore::rhiFlush
    // 里等它 —— 整个 GUI 线程死锁。
    //
    // 实测症状极具迷惑性：看板娘窗口的位置/尺寸都对、GL 上下文建起来了、纹理也
    // 传上去了、首帧 paintGL 正常返回，然后事件循环再无任何响应，且一行错误日志
    // 都没有。关掉磁盘缓存只是让 Qt 每次重新编译它自己那几个上屏着色器(毫秒级)，
    // 换掉「桌面应用不该依赖磁盘缓存可写」这个隐患。
    qputenv("QT_DISABLE_SHADER_DISK_CACHE", QByteArrayLiteral("1"));

    QApplication app(argc, argv);
    QApplication::setOrganizationName(appinfo::id());
    QApplication::setApplicationName(appinfo::id());
    QApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    // 缓存目录迁移：软件自身产生的缓存(缩略图/渲染背景图/诊断日志)统一写入
    // <程序目录>/.cache，位置只取决于 exe 所在目录，与当前工作目录无关。
    // 必须先于 videodiag::init()：诊断日志本身就写在 .cache/logs 下。
    // 创建失败只报错，不回退 AppData，也不改任何其他数据的位置。
    QString cacheError;
    const bool cacheReady = CachePaths::ensureDirectories(&cacheError);

    // 单实例守卫：两个实例会各建一套解码管线并互抢 WorkerW 挂载点。
    // Windows 上进程被强杀/崩溃后共享内存段会残留（引用计数无人递减），
    // 导致之后永远"已经在运行"——attach+detach 清掉残段后重试一次即可自愈；
    // 真有另一实例在跑时重试依旧失败，提示不变。
    videodiag::init(); // 守卫阶段即可记录诊断(幂等)
    videodiag::stage(QStringLiteral("Qt 应用对象创建"));
    if (!cacheReady)
        videodiag::log(videodiag::Level::Error, cacheError, QStringLiteral("Cache"));

    AppConfig::instance().load(); // 统一配置: 主窗口创建前加载(目录创建/迁移/校验)
    videodiag::stage(QStringLiteral("配置加载(含注册表迁移与校验)"));

    // 资源友好模式(默认开)：限制进程到 4 个逻辑核，且优先分属 4 个不同物理核。
    // 解码线程数跟随 QThread::idealThreadCount(受亲和性掩码影响)，实测
    // (32核机,1080p30)内存 -27%、显存 -36%、CPU 不变。在 QApplication 构造后
    // 立即设置，使全部后续线程继承掩码；video/affinityLimit=false 关闭。
    if (AppConfig::instance()
            .value(ConfigKeys::Video::AffinityLimit, true).toBool()) {
        if (fbswin::applyProcessAffinityLimit(4)) {
            videodiag::log(videodiag::Level::Info,
                QStringLiteral("资源友好模式: 进程已限制到 4 个逻辑核(尽量分属不同物理核)"),
                QStringLiteral("App"));
        }
    } else {
        videodiag::log(videodiag::Level::Info,
            QStringLiteral("资源友好模式: 已关闭(全核运行)"),
            QStringLiteral("App"));
    }
    if (!fbswin::acquireSingleInstanceLock()) {
        // 已有实例在运行：直接把它的主窗口调到最前，不弹窗打断；
        // 找不到(窗口尚未建好等罕见情形)才兜底提示。
        QString why;
        // 唤起偶发失败(窗口枚举/前台锁的时序竞争，实测约一次性)——重试兜底
        bool activated = false;
        for (int attempt = 0; attempt < 3 && !activated; ++attempt) {
            if (attempt > 0) {
                QThread::msleep(300);
                why.clear();
            }
            activated =
                fbswin::activateExistingInstanceWindow(appinfo::windowTitle(), &why);
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
            videodiag::log(videodiag::Level::Warning,
                QStringLiteral("唤起已运行实例失败: %1").arg(why),
                QStringLiteral("App"));
            QMessageBox::information(nullptr, QStringLiteral("虞美人"),
                                     QStringLiteral("虞美人已经在运行。"));
        } else {
            videodiag::log(videodiag::Level::Info,
                QStringLiteral("已唤起已运行实例窗口: %1").arg(why),
                QStringLiteral("App"));
        }
        return 0;
    }

    // one-shot import of the settings left behind by the FolderBgStudio builds
    appinfo::migrateLegacy();
    videodiag::stage(QStringLiteral("单实例守卫与旧配置导入"));

    // 阶段7 诊断日志尽早初始化(幂等)：默认 Info+，诊断模式经 YUMEIREN_DIAG=1 开启
    videodiag::init();

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFile qss(QStringLiteral(":/style.qss"));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
    videodiag::stage(QStringLiteral("样式与内嵌资源加载"));

    MainWindow w;
    videodiag::stage(QStringLiteral("主窗口构造完成(含 UI 初始化)"));
    w.show();
    videodiag::stage(QStringLiteral("主窗口首次显示"));

    // 自动化内存实验探针(第二阶段)：loadSettings 已在 MainWindow 构造中把播放列表
    // 写入 VideoWallpaper，此处再按 YUMEIREN_PROBE_STAGE 建立受控媒体栈状态。
    // 正常运行不设置该环境变量，本分支不执行。
    if (const QString probeStage = qEnvironmentVariable("YUMEIREN_PROBE_STAGE");
        !probeStage.isEmpty())
        VideoWallpaper::instance().runProbeStage(probeStage);

    const int exitCode = app.exec();
    // 退出收口(崩溃修复)：必须在 QApplication 析构之前、且在 MainWindow 析构之前
    // 主动卸载视频管线。函数内静态单例的析构由 CRT atexit 链驱动，跑在 main()
    // 返回之后，那时 ~QApplication 已经完成，任何 QWidget 调用(经 unmountWindow
    // → winId())都会踩进 Qt6Widgets 里 qApp==nullptr 的路径 → c0000005。
    // 详见 docs/crash_analysis.md。
    // 退出段不用 stage()：它的差值是"距上一次分段"的时间，会把事件循环运行
    // 时长一起算进来，读起来像 14 秒的"卸载"。这里单独量收口本身的耗时。
    const qint64 shutdownBegin = videodiag::elapsedMs();
    VideoWallpaper::shutdown();
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("事件循环结束 exit=%1 uptime=%2ms 退出收口 cost=%3ms")
            .arg(exitCode)
            .arg(videodiag::elapsedMs())
            .arg(videodiag::elapsedMs() - shutdownBegin),
        QStringLiteral("App"));
    return exitCode;
}

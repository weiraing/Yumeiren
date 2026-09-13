#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QThread>
#include <QMessageBox>
#include <QStyleFactory>

#include "appinfo.h"
#include "mainwindow.h"
#include "platform/windows/desktopmount.h"
#include "videodiag.h"
#include "videowallpaper.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(appinfo::id());
    QApplication::setApplicationName(appinfo::id());
    QApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    // 单实例守卫：两个实例会各建一套解码管线并互抢 WorkerW 挂载点。
    // Windows 上进程被强杀/崩溃后共享内存段会残留（引用计数无人递减），
    // 导致之后永远"已经在运行"——attach+detach 清掉残段后重试一次即可自愈；
    // 真有另一实例在跑时重试依旧失败，提示不变。
    videodiag::init(); // 守卫阶段即可记录诊断(幂等)

    // 资源友好模式(默认开)：限制进程到前 4 个逻辑核。解码线程数跟随
    // QThread::idealThreadCount(受亲和性掩码影响)，实测(32核机,1080p30)
    // 内存 -27%、显存 -36%、CPU 不变。在 QApplication 构造后立即设置，
    // 使全部后续线程继承掩码；设置 video/affinityLimit=false 关闭。
    if (appinfo::settings()
            .value(QStringLiteral("video/affinityLimit"), true).toBool()) {
        if (fbswin::applyProcessAffinityLimit(4))
            videodiag::log(videodiag::Level::Info,
                QStringLiteral("资源友好模式: 进程已限制到 4 个逻辑核"));
    } else {
        videodiag::log(videodiag::Level::Info,
            QStringLiteral("资源友好模式: 已关闭(全核运行)"));
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
            QFile guardLog(appinfo::dataRoot() + QStringLiteral("/logs/guard_%1.log")
                               .arg(QCoreApplication::applicationPid()));
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
                QStringLiteral("唤起已运行实例失败: %1").arg(why));
            QMessageBox::information(nullptr, QStringLiteral("虞美人"),
                                     QStringLiteral("虞美人已经在运行。"));
        } else {
            videodiag::log(videodiag::Level::Info,
                QStringLiteral("已唤起已运行实例窗口: %1").arg(why));
        }
        return 0;
    }

    // one-shot import of the settings left behind by the FolderBgStudio builds
    appinfo::migrateLegacy();

    // 阶段7 诊断日志尽早初始化(幂等)：默认 Info+，诊断模式经 YUMEIREN_DIAG=1 开启
    videodiag::init();

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFile qss(QStringLiteral(":/style.qss"));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    MainWindow w;
    w.show();

    // 自动化内存实验探针(第二阶段)：loadSettings 已在 MainWindow 构造中把播放列表
    // 写入 VideoWallpaper，此处再按 YUMEIREN_PROBE_STAGE 建立受控媒体栈状态。
    // 正常运行不设置该环境变量，本分支不执行。
    if (const QString probeStage = qEnvironmentVariable("YUMEIREN_PROBE_STAGE");
        !probeStage.isEmpty())
        VideoWallpaper::instance().runProbeStage(probeStage);

    return app.exec();
}

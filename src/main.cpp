#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QSharedMemory>
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
    QSharedMemory instanceGuard(QStringLiteral("Yumeiren.single-instance"));
    if (!instanceGuard.create(1)) {
        if (instanceGuard.attach())
            instanceGuard.detach();
    }
    if (!instanceGuard.create(1)) {
        // 已有实例在运行：直接把它的主窗口调到最前，不弹窗打断；
        // 找不到(窗口尚未建好等罕见情形)才兜底提示。
        if (!fbswin::activateExistingInstanceWindow(appinfo::displayName())) {
            QMessageBox::information(nullptr, QStringLiteral("虞美人"),
                                     QStringLiteral("虞美人已经在运行。"));
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

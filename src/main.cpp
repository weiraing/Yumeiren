#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QSharedMemory>
#include <QStyleFactory>

#include "appinfo.h"
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(appinfo::id());
    QApplication::setApplicationName(appinfo::id());
    QApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    // 单实例守卫：两个实例会各建一套解码管线并互抢 WorkerW 挂载点
    QSharedMemory instanceGuard(QStringLiteral("Yumeiren.single-instance"));
    if (!instanceGuard.create(1)) {
        QMessageBox::information(nullptr, QStringLiteral("虞美人"),
                                 QStringLiteral("虞美人已经在运行。"));
        return 0;
    }

    // one-shot import of the settings left behind by the FolderBgStudio builds
    appinfo::migrateLegacy();

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFile qss(QStringLiteral(":/style.qss"));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    MainWindow w;
    w.show();
    return app.exec();
}

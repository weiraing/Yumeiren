#include <QApplication>
#include <QFile>
#include <QStyleFactory>

#include "appinfo.h"
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(appinfo::id());
    QApplication::setApplicationName(appinfo::id());
    QApplication::setApplicationVersion(QStringLiteral("1.0.0"));

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

#include "kanban/ModelThumbCache.h"

#include "core/CachePaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

namespace kanban {

namespace {

// id 必须是「一个目录叶子名」：不含分隔符、不是 . / ..、不是空。
// 这条校验是安全边界而不是洁癖 —— id 来自磁盘上的目录名，理论上可以是任何东西，
// 而我们会拿它拼出一个将被写入的路径。
bool isSafeModelId(const QString &id)
{
    if (id.isEmpty() || id == QLatin1String(".") || id == QLatin1String(".."))
        return false;
    if (id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\')))
        return false;
    // Windows 上冒号开头的名字(如 "C:")会被解释成驱动器，一并挡掉。
    if (id.contains(QLatin1Char(':')))
        return false;
    return true;
}

} // namespace

QString ModelThumbCache::directory()
{
    const QString dir = CachePaths::modelThumbs();
    if (dir.isEmpty())
        return QString();
    if (!QDir().mkpath(dir))
        return QString();
    return dir;
}

QString ModelThumbCache::pathFor(const QString &modelId)
{
    if (!isSafeModelId(modelId))
        return QString();
    return QDir(CachePaths::modelThumbs()).filePath(modelId + QStringLiteral(".png"));
}

bool ModelThumbCache::has(const QString &modelId)
{
    const QString path = pathFor(modelId);
    if (path.isEmpty())
        return false;
    const QFileInfo info(path);
    return info.exists() && info.isFile() && info.size() > 0;
}

QString ModelThumbCache::store(const QString &modelId, const QImage &image)
{
    const QString target = pathFor(modelId);
    if (target.isEmpty())
        return QString();
    if (directory().isEmpty())
        return QString();
    if (image.isNull())
        return QString();

    // 临时名带 PID：两个生成进程同时被拉起(理论上不该，但不能靠这个前提)
    // 也不会互相踩同一个中间文件。
    const QString temp =
        target + QStringLiteral(".%1.tmp").arg(QCoreApplication::applicationPid());
    if (!image.save(temp, "PNG")) {
        QFile::remove(temp);
        return QString();
    }
    // QFile::rename 在 Windows 上不覆盖已存在的目标，所以先删。
    // 删失败(比如被杀软占着)就当这次失败 —— 界面继续用旧图，好过留下坏图。
    if (QFile::exists(target) && !QFile::remove(target)) {
        QFile::remove(temp);
        return QString();
    }
    if (!QFile::rename(temp, target)) {
        QFile::remove(temp);
        return QString();
    }
    return target;
}

void ModelThumbCache::sweepTempFiles()
{
    const QString dir = CachePaths::modelThumbs();
    if (dir.isEmpty())
        return;
    QDir cache(dir);
    const QStringList leftovers =
        cache.entryList(QStringList() << QStringLiteral("*.tmp"), QDir::Files);
    for (const QString &name : leftovers)
        cache.remove(name);
}

} // namespace kanban

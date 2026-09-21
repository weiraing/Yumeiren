#include "kanban/ModelThumbCache.h"

#include "core/CachePaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

namespace kanban {

namespace {

// key 必须是「一个文件名」：不含分隔符、不是 . / ..、不是空。这是安全边界
// 而非洁癖 —— key 会原样拼进一个将被写入的路径，带分隔符就能逃出缓存目录。
// 正常的 thumbKey(相对路径换 '#' 得来)天然不含分隔符，这关只是兜底。
bool isSafeThumbKey(const QString &key)
{
    if (key.isEmpty() || key == QLatin1String(".") || key == QLatin1String(".."))
        return false;
    if (key.contains(QLatin1Char('/')) || key.contains(QLatin1Char('\\')))
        return false;
    // Windows 上冒号开头的名字(如 "C:")会被解释成驱动器，一并挡掉。
    if (key.contains(QLatin1Char(':')))
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

QString ModelThumbCache::pathFor(const QString &thumbKey)
{
    if (!isSafeThumbKey(thumbKey))
        return QString();
    return QDir(CachePaths::modelThumbs()).filePath(thumbKey + QStringLiteral(".png"));
}

bool ModelThumbCache::has(const QString &thumbKey)
{
    const QString path = pathFor(thumbKey);
    if (path.isEmpty())
        return false;
    const QFileInfo info(path);
    return info.exists() && info.isFile() && info.size() > 0;
}

QString ModelThumbCache::store(const QString &thumbKey, const QImage &image)
{
    const QString target = pathFor(thumbKey);
    if (target.isEmpty())
        return QString();
    if (directory().isEmpty())
        return QString();
    if (image.isNull())
        return QString();

    // 临时名带 PID：两个生成进程同时被拉起也不会互相踩同一个中间文件。
    const QString temp =
        target + QStringLiteral(".%1.tmp").arg(QCoreApplication::applicationPid());
    if (!image.save(temp, "PNG")) {
        QFile::remove(temp);
        return QString();
    }
    // QFile::rename 在 Windows 上不覆盖已存在的目标，所以先删；删失败(如被杀软占着)
    // 就当这次失败 —— 界面继续用旧图，好过留下坏图。
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

bool ModelThumbCache::remove(const QString &thumbKey)
{
    const QString path = pathFor(thumbKey);
    if (path.isEmpty())
        return false;
    // 只删这一张 PNG，目录归 directory() 管。「本来就没有缓存」与「删掉了」对调用方
    // 是同一个结果，所以删不到不算失败，返回值只用来决定要不要写日志。
    return QFile::remove(path);
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

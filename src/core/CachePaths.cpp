#include "core/CachePaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace {

// 只有 ".cache" 这一个目录名允许出现在这里：业务代码一律通过 CachePaths 取路径。
constexpr char kCacheDirName[] = ".cache";

const QStringList &subDirNames()
{
    static const QStringList dirs = {
        QStringLiteral("gallery-thumbs"),
        QStringLiteral("model-thumbs"),
        QStringLiteral("rendered-bg"),
        QStringLiteral("image-pool"),
        QStringLiteral("logs"),
        QStringLiteral("web-profile"),
        QStringLiteral("web-snapshot"),
    };
    return dirs;
}

QString probeFileName()
{
    return QStringLiteral("writetest-%1.tmp").arg(QCoreApplication::applicationPid());
}

// 绝对化 + 统一分隔符。
QString lexicalAbsolute(const QString &path)
{
    return QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
}

// 解析已存在的最深祖先(展开符号链接/junction)，尾部原样接回并消掉 ".."：
// 对还没创建的缓存文件也能做可靠的越界判断。
QString resolvedPath(const QString &path)
{
    QString current = lexicalAbsolute(path);
    QStringList tail;
    while (!QFileInfo::exists(current)) {
        const QFileInfo fi(current);
        const QString parent = fi.dir().absolutePath();
        if (parent == fi.absoluteFilePath()) // 已到卷根，停
            break;
        tail.prepend(fi.fileName());
        current = parent;
    }
    QString head = QFileInfo(current).canonicalFilePath();
    if (head.isEmpty())
        head = lexicalAbsolute(current);
    for (const QString &part : tail)
        head += QLatin1Char('/') + part;
    return QDir::cleanPath(head);
}

bool isInside(const QString &parent, const QString &path)
{
    const QString p = QDir::cleanPath(parent);
    const QString f = QDir::cleanPath(path);
    if (p.compare(f, Qt::CaseInsensitive) == 0)
        return true;
    return f.startsWith(p + QLatin1Char('/'), Qt::CaseInsensitive);
}
} // namespace

QString CachePaths::root()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QLatin1String(kCacheDirName));
}

QString CachePaths::galleryThumbs()
{
    return QDir(root()).filePath(QStringLiteral("gallery-thumbs"));
}

QString CachePaths::modelThumbs()
{
    return QDir(root()).filePath(QStringLiteral("model-thumbs"));
}

QString CachePaths::renderedBg()
{
    return QDir(root()).filePath(QStringLiteral("rendered-bg"));
}

QString CachePaths::imagePool()
{
    return QDir(root()).filePath(QStringLiteral("image-pool"));
}

QString CachePaths::webProfile()
{
    // WebView2 浏览器配置/缓存(登录态、磁盘缓存)。归清缓存管：它是可再生缓存，
    // 代价只是网页要重新登录。磁盘缓存大小由启动参数另行限幅(见 WebWallpaper)。
    return QDir(root()).filePath(QStringLiteral("web-profile"));
}

QString CachePaths::webSnapshot()
{
    return QDir(root()).filePath(QStringLiteral("web-snapshot"));
}

QString CachePaths::logs()
{
    return QDir(root()).filePath(QStringLiteral("logs"));
}

bool CachePaths::contains(const QString &path)
{
    return isInside(resolvedPath(root()), resolvedPath(path));
}

bool CachePaths::isInsideProgramDir(const QString &path)
{
    return isInside(resolvedPath(QCoreApplication::applicationDirPath()), resolvedPath(path));
}

bool CachePaths::ensureDirectories(QString *errorMessage)
{
    QDir rootDir(root());
    if (!rootDir.mkpath(QStringLiteral("."))) {
        if (errorMessage)
            *errorMessage = QStringLiteral("无法创建缓存目录：%1").arg(root());
        return false;
    }
    // .cache 被 junction/符号链接指到程序目录之外时写入同样会跑出去：判失败。
    if (!isInsideProgramDir(rootDir.canonicalPath())) {
        if (errorMessage)
            *errorMessage = QStringLiteral("缓存目录 %1 指向程序目录之外，已拒绝写入。").arg(root());
        return false;
    }

    const QStringList subs = subDirNames();
    for (const QString &sub : subs) {
        const QString dir = QDir(root()).filePath(sub);
        if (!rootDir.mkpath(sub)) {
            if (errorMessage)
                *errorMessage = QStringLiteral("无法创建缓存子目录：%1").arg(dir);
            return false;
        }
        if (!contains(dir)) {
            if (errorMessage)
                *errorMessage = QStringLiteral("缓存子目录 %1 超出了 %2，已拒绝写入。")
                                    .arg(dir, root());
            return false;
        }
    }
    return true;
}

bool CachePaths::isWritable(QString *errorMessage)
{
    if (!ensureDirectories(errorMessage))
        return false;

    const QString probe = QDir(root()).filePath(probeFileName());
    QFile file(probe);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage)
            *errorMessage =
                QStringLiteral("缓存目录不可写：%1 (%2)。请把软件放到可写目录后重试；"
                               "软件不会改用 AppData 等其他位置存放缓存。")
                    .arg(root(), file.errorString());
        return false;
    }
    file.write("ok");
    file.close();
    // 探针用完即删；删不掉只留下几字节临时文件，不代表不可写。
    file.remove();
    return true;
}

#include "kanban/ImageDecode.h"

#include <QMutex>
#include <QMutexLocker>

namespace kanban {

namespace {

// 愿意为单张图付的最大解压内存。给到 1 GiB：16384x8192 的 RGBA8888 是 512 MiB，
// 装得下；再往上基本就是文件头里的宽高不可信了，那时不抬上限、让 Qt 按默认策略
// 拒绝更安全(否则一个坏头就能让进程去申请几十 GB)。
constexpr qint64 kMaxDecodeBytes = qint64(1024) * 1024 * 1024;

// 保护全局分配上限的抬升与还原，详见 ImageDecode.h。
QMutex &limitMutex()
{
    static QMutex mutex;
    return mutex;
}

} // namespace

QImage readImageDownscaled(QImageReader &reader, int maxDim)
{
    // reader.size() 只读文件头、不解码 —— 拿到的是素材真实尺寸，也正是 Qt 那条
    // 上限检查用的尺寸。
    const QSize source = reader.size();

    QSize target;
    if (maxDim > 0 && source.isValid() && !source.isEmpty()) {
        const int longest = qMax(source.width(), source.height());
        if (longest > maxDim) {
            target = QSize(qMax(1, source.width() * maxDim / longest),
                           qMax(1, source.height() * maxDim / longest));
        }
    }

    // 按最宽的 RGBA8888 估所需内存(PNG 也可能 1/2/3 字节每像素，估大不估小)。
    qint64 bytes = 0;
    if (source.isValid() && !source.isEmpty()) {
        bytes = qint64(source.width()) * qint64(source.height()) * 4;
    }

    QMutexLocker locker(&limitMutex());

    const int saved = QImageReader::allocationLimit();
    const int needed = bytes > 0 ? int(bytes / (1024 * 1024)) + 1 : 0;
    const bool raised = needed > saved && bytes <= kMaxDecodeBytes;
    if (raised) {
        QImageReader::setAllocationLimit(needed);
    }

    QImage image = reader.read();

    if (raised && QImageReader::allocationLimit() == needed) {
        // 只在当前值仍是我们设的那个时才还原 —— 否则说明别处改过，不该覆盖它。
        QImageReader::setAllocationLimit(saved);
    }

    if (!image.isNull() && !target.isEmpty()) {
        // 显式缩放而非 setScaledSize：Qt 6 不允许指定自动缩放的重采样模式，
        // 默认那条路径在大比例缩减下会采出锯齿。
        image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

} // namespace kanban

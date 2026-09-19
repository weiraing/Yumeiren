#ifndef IMAGEPROCESS_H
#define IMAGEPROCESS_H

#include <QImage>
#include <QSize>

namespace ImageProcess
{
// brightness/contrast: 1.0 = unchanged; blurRadius: 0 = off (gaussian-like).
QImage adjust(const QImage &src, double brightness, double contrast, int blurRadius);

// 绕竖直中心轴的伪 3D 旋转：水平按 cos(角度) 压缩，±180° 即左右镜像，±90° 侧立
// 不可见。画布尺寸不变，两侧以透明填充。
QImage rotateAroundY(const QImage &src, int degrees);

// Compose the preview: the processed image over a mock explorer backdrop with
// the global alpha applied (emulates imgAlpha of the hook DLL).
// realWindowSize 是参照的真实资源管理器窗口(物理像素)，故预览中图片的相对大小
// 与真实窗口一致。posType 为界面位置选项 0..6(0 填充 1 居中原尺寸 2 拉伸 3..6 四角)。
QImage mockExplorerPreview(const QImage &processed, const QSize &nativeSize, int posType,
                           int imgAlpha, const QSize &canvasSize, qreal canvasDpr,
                           const QSize &realWindowSize, bool darkMode);
}

#endif // IMAGEPROCESS_H

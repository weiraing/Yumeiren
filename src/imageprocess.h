#ifndef IMAGEPROCESS_H
#define IMAGEPROCESS_H

#include <QImage>
#include <QSize>

namespace ImageProcess
{
// brightness/contrast: 1.0 = unchanged; blurRadius: 0 = off (gaussian-like).
QImage adjust(const QImage &src, double brightness, double contrast, int blurRadius);

// 绕竖直中心轴的伪 3D 旋转投影：水平按 cos(角度) 压缩(角度为负向逆时针)，
// ±180° 即左右镜像，±90° 侧立不可见。画布尺寸不变，两侧以透明填充。
QImage rotateAroundY(const QImage &src, int degrees);

// Compose the preview: the processed image over a mock explorer backdrop with
// the global alpha applied (emulates imgAlpha of the hook DLL).
// 比例仿真：canvasSize/canvasDpr 描述预览画布；realWindowSize 是参照的真实
// 资源管理器窗口(物理像素)。posType 为界面位置选项 0..6(0 填充 1 居中原尺寸
// 2 拉伸 3..6 四角)，预览中图片的相对大小与真实窗口中一致。
QImage mockExplorerPreview(const QImage &processed, const QSize &nativeSize, int posType,
                           int imgAlpha, const QSize &canvasSize, qreal canvasDpr,
                           const QSize &realWindowSize, bool darkMode);
}

#endif // IMAGEPROCESS_H

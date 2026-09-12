#include "imageprocess.h"

#include <QPainter>
#include <QtMath>
#include <vector>

namespace {

void applyLevels(QImage &img, double brightness, double contrast)
{
    if (qFuzzyCompare(brightness, 1.0) && qFuzzyCompare(contrast, 1.0))
        return;
    // Standard LUT: brightness multiplies, contrast pivots around mid gray.
    double c = contrast;
    quint8 lut[256];
    for (int i = 0; i < 256; ++i) {
        double v = i * brightness;
        v = (v - 128.0) * c + 128.0;
        lut[i] = quint8(qBound(0.0, v, 255.0));
    }
    if (img.format() != QImage::Format_ARGB32)
        img = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            QRgb p = line[x];
            line[x] = qRgba(lut[qRed(p)], lut[qGreen(p)], lut[qBlue(p)], qAlpha(p));
        }
    }
}

// Separable box blur, 3 passes ~= gaussian. Operates on RGB only, so the
// transparency of figures/stickers is preserved.
void boxBlur(QImage &img, int radius)
{
    if (radius < 1 || img.isNull())
        return;
    if (img.format() != QImage::Format_ARGB32)
        img = img.convertToFormat(QImage::Format_ARGB32);

    const int w = img.width();
    const int h = img.height();
    const int passes = 3;

    std::vector<quint8> src(size_t(w) * h * 4);
    std::vector<quint8> tmp(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            QRgb p = line[x];
            size_t i = (size_t(y) * w + x) * 4;
            src[i] = quint8(qRed(p));
            src[i + 1] = quint8(qGreen(p));
            src[i + 2] = quint8(qBlue(p));
            src[i + 3] = quint8(qAlpha(p));
        }
    }

    for (int pass = 0; pass < passes; ++pass) {
        // horizontal
        for (int y = 0; y < h; ++y) {
            size_t row = size_t(y) * w;
            for (int c = 0; c < 4; ++c) {
                int sum = 0;
                for (int x = -radius; x <= radius; ++x)
                    sum += src[(row + qBound(0, x, w - 1)) * 4 + c];
                const int div = 2 * radius + 1;
                for (int x = 0; x < w; ++x) {
                    tmp[(row + x) * 4 + c] = quint8(sum / div);
                    int out = src[(row + qBound(0, x - radius, w - 1)) * 4 + c];
                    int in  = src[(row + qBound(0, x + radius + 1, w - 1)) * 4 + c];
                    sum += in - out;
                }
            }
        }
        // vertical
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 4; ++c) {
                int sum = 0;
                for (int y = -radius; y <= radius; ++y)
                    sum += tmp[(size_t(qBound(0, y, h - 1)) * w + x) * 4 + c];
                const int div = 2 * radius + 1;
                for (int y = 0; y < h; ++y) {
                    src[(size_t(y) * w + x) * 4 + c] = quint8(sum / div);
                    int out = tmp[(size_t(qBound(0, y - radius, h - 1)) * w + x) * 4 + c];
                    int in  = tmp[(size_t(qBound(0, y + radius + 1, h - 1)) * w + x) * 4 + c];
                    sum += in - out;
                }
            }
        }
    }

    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            size_t i = (size_t(y) * w + x) * 4;
            line[x] = qRgba(src[i], src[i + 1], src[i + 2], src[i + 3]);
        }
    }
}

} // namespace

QImage ImageProcess::adjust(const QImage &src, double brightness, double contrast, int blurRadius)
{
    if (src.isNull())
        return QImage();
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    applyLevels(img, brightness, contrast);
    if (blurRadius > 0)
        boxBlur(img, blurRadius);
    return img;
}

QImage ImageProcess::rotateAroundY(const QImage &src, int degrees)
{
    if (src.isNull() || degrees % 360 == 0)
        return src;
    const double sx = qCos(qDegreesToRadians(double(degrees % 360)));
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    if (qAbs(sx) < 0.005)
        return out; // 侧立不可见
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QTransform t;
    t.translate(src.width() / 2.0, 0);
    t.scale(sx, 1.0); // 负值即左右镜像，正负由角度方向决定
    t.translate(-src.width() / 2.0, 0);
    p.setTransform(t);
    p.drawImage(0, 0, src);
    p.end();
    return out;
}

QImage ImageProcess::mockExplorerPreview(const QImage &processed, const QSize &nativeSize,
                                         int posType, int imgAlpha, const QSize &canvasSize,
                                         qreal canvasDpr, const QSize &realWindowSize,
                                         bool darkMode)
{
    const int W = qMax(240, canvasSize.width());
    const int H = qMax(180, canvasSize.height());
    const qreal dpr = canvasDpr > 0 ? canvasDpr : 1.0;
    QImage canvas(int(W * dpr), int(H * dpr), QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(dpr);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const QColor toolbarBg = darkMode ? QColor(38, 38, 40) : QColor(251, 251, 252);
    const QColor barLine   = darkMode ? QColor(52, 52, 56) : QColor(229, 230, 234);
    const QColor textCol   = darkMode ? QColor(214, 215, 220) : QColor(88, 90, 98);
    const QColor subText   = darkMode ? QColor(140, 142, 150) : QColor(140, 142, 150);
    const QColor pillBg    = darkMode ? QColor(56, 56, 60) : QColor(255, 255, 255);
    const QColor folder    = darkMode ? QColor(222, 178, 82) : QColor(249, 199, 96);
    const QColor sideTint  = darkMode ? QColor(30, 30, 32, 190) : QColor(248, 248, 250, 190);

    canvas.fill(toolbarBg);

    // Toolbar: nav dots + address pill
    const int toolH = 36;
    p.setBrush(pillBg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(12, 9, 18, 18, 6, 6);
    p.drawRoundedRect(34, 9, 18, 18, 6, 6);
    p.setPen(subText);
    p.setFont(QFont(QStringLiteral("Segoe UI Symbol"), 8));
    p.drawText(QRect(12, 9, 18, 18), Qt::AlignCenter, QChar(0x2190));
    p.drawText(QRect(34, 9, 18, 18), Qt::AlignCenter, QChar(0x2192));

    p.setBrush(pillBg);
    p.drawRoundedRect(60, 8, W - 60 - 30, 20, 10, 10);
    p.setPen(textCol);
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 8));
    p.drawText(QRect(74, 8, W - 90, 20), Qt::AlignVCenter, QStringLiteral("文档"));

    p.setPen(Qt::NoPen);
    p.setBrush(pillBg);
    p.drawRoundedRect(W - 26, 9, 16, 18, 5, 5);
    p.setPen(barLine);
    p.drawLine(0, toolH, W, toolH);

    // Command bar
    const int cmdH = 26;
    p.setPen(textCol);
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 7.5));
    p.drawText(QRect(14, toolH, 80, cmdH), Qt::AlignVCenter, QStringLiteral("＋ 新建"));
    p.drawText(QRect(W - 80, toolH, 66, cmdH), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("详细信息"));
    p.setPen(barLine);
    p.drawLine(0, toolH + cmdH, W, toolH + cmdH);

    // Content background image, laid out exactly like the hook DLL would lay
    // it out in a real window: scale factor k maps the mock content width onto
    // the reference window width (physical pixels), so the image's relative
    // size in the preview equals its relative size in the real explorer.
    const int contentTop = toolH + cmdH;
    const int contentH = H - contentTop;
    const QRectF contentRect(0, contentTop, W, contentH);
    QImage img = processed;
    if (imgAlpha < 255) {
        img = img.convertToFormat(QImage::Format_ARGB32);
        QPainter pm(&img);
        pm.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        pm.fillRect(img.rect(), QColor(0, 0, 0, imgAlpha));
        pm.end();
    }
    if (!img.isNull()) {
        const double k = realWindowSize.width() > 0
            ? double(W) / realWindowSize.width() : 0.2;
        const QSizeF native(nativeSize.width() > 0 ? nativeSize.width() : 1920,
                            nativeSize.height() > 0 ? nativeSize.height() : 1080);
        QRectF r;
        if (posType == 2) {                       // 拉伸填满
            r = contentRect;
        } else if (posType == 0) {                // 填充窗口(zoom & fill)
            const QSizeF fit = native.scaled(contentRect.size(),
                                             Qt::KeepAspectRatioByExpanding);
            r = QRectF((W - fit.width()) / 2.0, contentTop + (contentH - fit.height()) / 2.0,
                       fit.width(), fit.height());
        } else {                                  // 原尺寸：1 居中 / 3..6 四角
            const QSizeF scaled = native * k;
            qreal x = (W - scaled.width()) / 2.0;
            qreal y = contentTop + (contentH - scaled.height()) / 2.0;
            if (posType == 3 || posType == 5)     // 左上 / 左下
                x = 0;
            if (posType == 4 || posType == 6)     // 右上 / 右下
                x = W - scaled.width();
            if (posType == 3 || posType == 4)     // 上排
                y = contentTop;
            if (posType == 5 || posType == 6)     // 下排
                y = H - scaled.height();
            r = QRectF(x, y, scaled.width(), scaled.height());
        }
        p.save();
        p.setClipRect(contentRect);
        p.drawImage(r, img);
        p.restore();
    } else {
        p.fillRect(0, contentTop, W, H - contentTop,
                   darkMode ? QColor(26, 26, 28) : QColor(243, 244, 246));
    }

    // Sidebar (with the combined effect it is a translucent acrylic tint)
    const int sideW = 82;
    p.fillRect(0, contentTop, sideW, H - contentTop, sideTint);
    p.setPen(barLine);
    p.drawLine(sideW, contentTop, sideW, H);
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 7.5));
    const QStringList items = {QStringLiteral("主文件夹"), QStringLiteral("图库"),
                               QStringLiteral("桌面"), QStringLiteral("下载"), QStringLiteral("文档")};
    for (int i = 0; i < items.size(); ++i) {
        const int y = contentTop + 12 + i * 30;
        if (y + 16 > H)
            break;
        p.setPen(Qt::NoPen);
        p.setBrush(folder);
        p.drawRoundedRect(14, y + 2, 15, 12, 2, 2);
        p.setPen(subText);
        p.drawText(QRect(36, y, sideW - 42, 16), Qt::AlignVCenter, items[i]);
    }

    // Folder items grid (columns adapt to the canvas width)
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 7.5));
    const int rows = contentH >= 150 ? 2 : 1;
    const int cols = 3;
    const qreal cellW = qreal(W - sideW - 12) / cols;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const qreal x = sideW + 6 + c * cellW + (cellW - 42) / 2.0;
            const int y = contentTop + 22 + r * 88;
            QColor g1 = folder, g2 = folder.darker(112);
            p.setPen(Qt::NoPen);
            p.setBrush(g1);
            p.drawRoundedRect(QRectF(x, y, 42, 30), 3, 3);
            p.setBrush(g2);
            p.drawRect(QRectF(x, y - 5, 20, 7));
            p.setPen(subText);
            p.drawText(QRectF(sideW + 6 + c * cellW, y + 34, cellW, 14),
                       Qt::AlignCenter, QStringLiteral("文件夹"));
        }
    }
    p.end();
    return canvas;
}

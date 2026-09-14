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

    // Scale factor: all UI chrome scales proportionally so the mock explorer
    // content stays fully visible at any canvas size (ref width = 800 px).
    const qreal s = qMax(0.4, qMin(2.0, W / 800.0));

    // Toolbar: nav dots + address pill
    const int toolH = qRound(36 * s);
    const int navY  = qRound(9 * s);
    const int navS  = qRound(18 * s);
    p.setBrush(pillBg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(qRound(12 * s), navY, navS, navS, qRound(6 * s), qRound(6 * s));
    p.drawRoundedRect(qRound(34 * s), navY, navS, navS, qRound(6 * s), qRound(6 * s));
    p.setPen(subText);
    p.setFont(QFont(QStringLiteral("Segoe UI Symbol"), qMax(5, qRound(8 * s))));
    p.drawText(QRect(qRound(12 * s), navY, navS, navS), Qt::AlignCenter, QChar(0x2190));
    p.drawText(QRect(qRound(34 * s), navY, navS, navS), Qt::AlignCenter, QChar(0x2192));

    const int pillX = qRound(60 * s);
    const int pillH = qRound(20 * s);
    p.setBrush(pillBg);
    p.drawRoundedRect(pillX, qRound(8 * s), W - pillX - qRound(30 * s), pillH,
                      qRound(10 * s), qRound(10 * s));
    p.setPen(textCol);
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), qMax(5, qRound(8 * s))));
    p.drawText(QRect(pillX + qRound(14 * s), qRound(8 * s), W - pillX - qRound(30 * s), pillH),
               Qt::AlignVCenter, QStringLiteral("文档"));

    p.setPen(Qt::NoPen);
    p.setBrush(pillBg);
    p.drawRoundedRect(W - qRound(26 * s), qRound(9 * s), qRound(16 * s), qRound(18 * s),
                      qRound(5 * s), qRound(5 * s));
    p.setPen(barLine);
    p.drawLine(0, toolH, W, toolH);

    // Command bar
    const int cmdH = qRound(26 * s);
    p.setPen(textCol);
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), qMax(5, qRound(7.5 * s))));
    p.drawText(QRect(qRound(14 * s), toolH, qRound(80 * s), cmdH),
               Qt::AlignVCenter, QStringLiteral("＋ 新建"));
    p.drawText(QRect(W - qRound(80 * s), toolH, qRound(66 * s), cmdH),
               Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("详细信息"));
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
    const int sideW = qRound(82 * s);
    p.fillRect(0, contentTop, sideW, H - contentTop, sideTint);
    p.setPen(barLine);
    p.drawLine(sideW, contentTop, sideW, H);
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), qMax(5, qRound(7.5 * s))));
    const QStringList items = {QStringLiteral("主文件夹"), QStringLiteral("图库"),
                               QStringLiteral("桌面"), QStringLiteral("下载"), QStringLiteral("文档")};
    const int sidePadY = qRound(12 * s);
    const int sideItemH = qRound(30 * s);
    const int sideIconW = qRound(15 * s);
    const int sideIconH = qRound(12 * s);
    const int sideTxtX = qRound(36 * s);
    for (int i = 0; i < items.size(); ++i) {
        const int y = contentTop + sidePadY + i * sideItemH;
        if (y + qRound(16 * s) > H)
            break;
        p.setPen(Qt::NoPen);
        p.setBrush(folder);
        p.drawRoundedRect(qRound(14 * s), y + qRound(2 * s), sideIconW, sideIconH, qRound(2 * s), qRound(2 * s));
        p.setPen(subText);
        p.drawText(QRect(sideTxtX, y, sideW - sideTxtX - qRound(6 * s), qRound(16 * s)),
                   Qt::AlignVCenter, items[i]);
    }

    // Folder items grid (columns adapt to the canvas width)
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), qMax(5, qRound(7.5 * s))));
    const int rows = contentH >= qRound(150 * s) ? 2 : 1;
    const int cols = 3;
    const qreal cellW = qreal(W - sideW - qRound(12 * s)) / cols;
    const int folderW = qRound(42 * s);
    const int folderH = qRound(30 * s);
    const int tabW = qRound(20 * s);
    const int tabH = qRound(7 * s);
    const int rowSpacing = qRound(88 * s);
    const int gridPadY = qRound(22 * s);
    const int gridPadX = qRound(6 * s);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const qreal x = sideW + gridPadX + c * cellW + (cellW - folderW) / 2.0;
            const int y = contentTop + gridPadY + r * rowSpacing;
            QColor g1 = folder, g2 = folder.darker(112);
            p.setPen(Qt::NoPen);
            p.setBrush(g1);
            p.drawRoundedRect(QRectF(x, y, folderW, folderH), qRound(3 * s), qRound(3 * s));
            p.setBrush(g2);
            p.drawRect(QRectF(x, y - qRound(5 * s), tabW, tabH));
            p.setPen(subText);
            p.drawText(QRectF(sideW + gridPadX + c * cellW, y + qRound(34 * s), cellW, qRound(14 * s)),
                       Qt::AlignCenter, QStringLiteral("文件夹"));
        }
    }
    p.end();
    return canvas;
}

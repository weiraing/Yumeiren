#include "core/ImageProcess.h"

#include <QPainter>
#include <QFontMetrics>
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
    // 下限取小一点，避免预览框缩小时被强行撑变形(宽高比由调用方锁定为桌面比例)。
    const int W = qMax(160, canvasSize.width());
    const int H = qMax(90, canvasSize.height());
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
    const int statusH = qRound(22 * s);        // 底部状态栏，让模拟窗口有个收尾
    const int contentBottom = H - statusH;     // 内容区下边界，状态栏画在它下方
    const int contentH = contentBottom - contentTop;
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
                y = contentBottom - scaled.height();
            r = QRectF(x, y, scaled.width(), scaled.height());
        }
        p.save();
        p.setClipRect(contentRect);
        p.drawImage(r, img);
        p.restore();
    } else {
        p.fillRect(0, contentTop, W, contentH,
                   darkMode ? QColor(26, 26, 28) : QColor(243, 244, 246));
    }

    // Sidebar (with the combined effect it is a translucent acrylic tint)
    const QStringList items = {QStringLiteral("主文件夹"), QStringLiteral("图库"),
                               QStringLiteral("桌面"), QStringLiteral("下载"), QStringLiteral("文档"),
                               QStringLiteral("此电脑"), QStringLiteral("网络")};
    const int sideIconX = qRound(14 * s);
    const int sideIconW = qRound(15 * s);
    const int sideIconH = qRound(12 * s);
    const int sideTxtX  = sideIconX + sideIconW + qRound(8 * s);
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), qMax(5, qRound(7.5 * s))));
    // 字号有 5pt 下限，小画布下文字并不是按 s 等比缩小的，所以侧栏的宽度、
    // 行高都改用真实字体度量，「主文件夹」才不会被分割线切掉。
    const QFontMetrics sideFm(p.font());
    int sideLabelW = 0;
    for (const QString &it : items)
        sideLabelW = qMax(sideLabelW, sideFm.horizontalAdvance(it));
    const int sideW = qMin(sideTxtX + sideLabelW + qRound(10 * s), qRound(W * 0.32));
    const int sideRowH  = qMax(qRound(16 * s), sideFm.height());
    const int sideItemH = sideRowH + qRound(4 * s);
    const int sidePadY  = qRound(12 * s);
    p.fillRect(0, contentTop, sideW, contentBottom - contentTop, sideTint);
    p.setPen(barLine);
    p.drawLine(sideW, contentTop, sideW, contentBottom);
    for (int i = 0; i < items.size(); ++i) {
        const int y = contentTop + sidePadY + i * sideItemH;
        if (y + sideRowH > contentBottom)
            break;
        p.setPen(Qt::NoPen);
        p.setBrush(folder);
        p.drawRoundedRect(sideIconX, y + (sideRowH - sideIconH) / 2, sideIconW, sideIconH,
                          qRound(2 * s), qRound(2 * s));
        p.setPen(subText);
        p.drawText(QRectF(sideTxtX, y, sideW - sideTxtX - qRound(6 * s), sideRowH),
                   Qt::AlignVCenter | Qt::ElideRight, items[i]);
    }

    // Folder items grid: 先定网格，再定图标，最后按图标高度反推文件名字号。
    // 顺序不能反 —— 一旦用文件名宽度去撑网格，就会出现「文字比文件夹还大」。
    const int gridPadY = qRound(14 * s);
    const int gridPadX = qRound(6 * s);
    const int rowTop = contentTop + gridPadY;
    const int availW = W - sideW - gridPadX * 2;
    const int availH = contentBottom - rowTop;
    const QString folderName = QStringLiteral("文件夹");
    const int cellW = qMax(18, qRound(92 * s));                 // 名义网格宽度，列距不再横向摊满
    const int cols = qBound(2, availW / cellW, 10);
    const qreal colW = qreal(availW) / cols;
    const int folderW = qMax(9, qRound(colW * 0.58));
    const int folderH = qMax(6, qRound(folderW / 1.38));
    const int tabH    = qMax(2, qRound(folderH * 0.24));
    const int tabW    = qMax(3, qRound(folderW * 0.46));
    // 文件名字号只取图标高度的约三成，用像素字号锁定，不跟着字号下限一起膨胀。
    QFont nameFont = p.font();
    nameFont.setPixelSize(qBound(6, qRound(folderH * 0.28), 11));
    p.setFont(nameFont);
    const QFontMetrics nameFm(nameFont);
    const int nameH   = qMax(6, nameFm.height());
    const int cellH   = tabH + folderH + qRound(3 * s) + nameH; // 图标 + 文件名
    const int rowPitch = cellH + qMax(qRound(6 * s), qRound(cellH * 0.16)); // 行距贴着内容
    const int rows = qBound(1, (availH + rowPitch - cellH) / rowPitch, 2);
    for (int r = 0; r < rows; ++r) {
        const int y = rowTop + r * rowPitch;
        if (y >= contentBottom)
            break;
        for (int c = 0; c < cols; ++c) {
            const qreal x = sideW + gridPadX + c * colW + (colW - folderW) / 2.0;
            p.setPen(Qt::NoPen);
            p.setBrush(folder.darker(112));
            p.drawRect(QRectF(x, y, tabW, tabH));
            p.setBrush(folder);
            p.drawRoundedRect(QRectF(x, y + tabH, folderW, folderH),
                              qMax(1, qRound(3 * s)), qMax(1, qRound(3 * s)));
            p.setPen(subText);
            p.drawText(QRectF(sideW + gridPadX + c * colW, y + tabH + folderH + qRound(3 * s),
                              colW, nameH),
                       Qt::AlignHCenter | Qt::AlignTop | Qt::ElideRight, folderName);
        }
    }

    // 底部状态栏：项目数 + 视图切换按钮，补住窗口最下方那条空白。
    p.setPen(QPen(barLine, 1));
    p.setBrush(Qt::NoBrush);
    p.drawLine(0, contentBottom, W, contentBottom);
    p.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), qMax(5, qRound(7 * s))));
    p.setPen(subText);
    p.drawText(QRect(sideW + qRound(10 * s), contentBottom, qRound(150 * s), statusH),
               Qt::AlignVCenter, QStringLiteral("%1 个项目").arg(rows * cols));
    const int btnW = qRound(18 * s);
    const int btnH = qMax(6, statusH - qRound(8 * s));
    const int btnX = W - qRound(30 * s);
    const int btnY = contentBottom + (statusH - btnH) / 2;
    p.setPen(QPen(barLine, 1));
    p.setBrush(pillBg);
    p.drawRoundedRect(btnX, btnY, btnW, btnH, qRound(2 * s), qRound(2 * s));
    p.setPen(QPen(subText, 1));
    for (int i = 1; i <= 2; ++i) {
        const int ly = btnY + btnH * i / 3;
        p.drawLine(btnX + qRound(4 * s), ly, btnX + btnW - qRound(4 * s), ly);
    }
    p.end();
    return canvas;
}

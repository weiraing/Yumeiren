// 文件夹美化模块(图片背景+效果样式+使用说明，即首屏三个页签)——图片背景页的
// 构建与交互逻辑（含图库浏览、参数调整、随机模式）。
#include "ui/MainWindow.h"

#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/CachePaths.h"
#include "explorerbg/Engine.h"
#include "ui/TooltipStyle.h"

#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFileDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 只解到需要的尺寸：大图(如 4K)先按文件头在解码内缩放，避免先完整展开几十 MB
// 再缩到几百像素宽。各格式插件的 scaledSize 路径自带平滑，最后再用 Smooth 把
// 残余尺寸校准到目标，画质与"全尺寸解码+一次 Smooth"等价。头信息无效或图不大时
// 退化为整图解码，与旧路径一致。
QImage decodeDownscaled(const QString &path, const QSize &target)
{
    QImageReader reader(path);
    const QSize source = reader.size();
    if (!target.isEmpty() && source.isValid() && !source.isEmpty()
        && (source.width() > target.width() || source.height() > target.height())) {
        reader.setScaledSize(source.scaled(target, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (!image.isNull() && !target.isEmpty()
        && (image.width() > target.width() || image.height() > target.height())) {
        image = image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

class GalleryDelegate : public QStyledItemDelegate
{
public:
    explicit GalleryDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QIcon icon = opt.icon;
        const QString name = opt.text;
        opt.icon = QIcon();
        opt.text.clear();
        opt.features &= ~(QStyleOptionViewItem::HasDecoration | QStyleOptionViewItem::HasDisplay);
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        painter->save();
        painter->setFont(opt.font);
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const QRect card = option.rect.adjusted(4, 4, -4, -4);
        const int footerHeight = qMax(28, opt.fontMetrics.height() + 8);
        const int dividerY = card.bottom() - footerHeight;
        const QRect imageRect(card.left() + 4, card.top() + 4,
                              card.width() - 8, dividerY - card.top() - 8);
        icon.paint(painter, imageRect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        painter->setPen(QColor(128, 128, 128, 65));
        painter->drawLine(card.left(), dividerY, card.right(), dividerY);

        const QRect textRect(card.left() + 4, dividerY + 1,
                             card.width() - 8, footerHeight - 1);
        const QString text = opt.fontMetrics.elidedText(name, Qt::ElideRight, textRect.width());
        style->drawItemText(painter, textRect, Qt::AlignCenter, opt.palette,
                            opt.state & QStyle::State_Enabled, text, QPalette::Text);
        painter->restore();
    }

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->font.setPointSizeF(qMax(7.0, option->font.pointSizeF() - 1.0));
        option->fontMetrics = QFontMetrics(option->font);
    }
};

} // namespace

// 透明度(0=不透明，越大越透) → 绘制与 DLL 配置(imgAlpha)用的 alpha(0..255)。
// 与 AppConfig 里旧键迁移的反算式互为逆运算；上限 88%(见 ConfigKeys::Image)对应
// 旧版 alpha 下限 30，这里对越界值兜底钳到 0..100。
int transparencyToAlpha(int transparencyPercent)
{
    return qRound((100 - qBound(0, transparencyPercent, 100)) * 255 / 100.0);
}


QString MainWindow::elidedTwoLineText(const QString &text, int width) const
{
    if (text.isEmpty())
        return text;
    const QFontMetrics fm(m_imageSourceLabel->fontMetrics());
    const int lineH = fm.lineSpacing();
    const int twoH = lineH * 2;
    if (fm.boundingRect(0, 0, width, twoH, Qt::TextWordWrap, text).height() <= twoH)
        return text; // 两行内放得下
    // 二分查找第一行能容纳的整词前缀
    int lo = 0, hi = text.size();
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (fm.boundingRect(0, 0, width, lineH, Qt::TextWordWrap, text.left(mid)).height()
                <= lineH)
            lo = mid;
        else
            hi = mid - 1;
    }
    int cut = lo;
    while (cut > 0 && text.at(cut - 1).isSpace())
        --cut;
    const QString first = text.left(cut);
    const QString rest = text.mid(cut).trimmed();
    if (first.isEmpty()) // 单词超宽的极端情况：整体单行省略
        return fm.elidedText(text, Qt::ElideRight, width);
    return first + QLatin1Char('\n') + fm.elidedText(rest, Qt::ElideRight, width);
}
// 文件夹美化的第三个页签「使用说明」：纯静态说明卡，内容只描述图片背景/效果样式
// 两项能力，故归入本模块(此前误放在动态壁纸页的源文件里)。
QWidget *MainWindow::buildHelpPage()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget(scroll);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);

    auto *card = new QFrame(page);
    card->setObjectName(QStringLiteral("PageCard"));
    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(18, 14, 18, 18);
    cardLay->setSpacing(10);

    auto *t = new QLabel(QStringLiteral("使用说明"), card);
    t->setObjectName(QStringLiteral("GroupTitle"));
    cardLay->addWidget(t);

    const QString text = QStringLiteral(
        "<p style='color:#d5d8de'>「虞美人」整合了三个开源项目的能力，为 Windows 10 / 11 的文件资源管理器设置背景：</p>"
        "<p style='color:#b9bcc4'>• <b>图片背景</b> —— 基于 Maplespe 的 explorerTool(ExplorerBgTool.dll)，"
        "默认浏览软件目录下的 data/image 文件夹，也可点击“选择文件夹”更换目录，"
        "支持亮度 / 对比度 / 模糊 / 透明度 / 显示位置调整，"
        "可选扩展到文件打开、保存对话框。</p>"
        "<p style='color:#b9bcc4'>• <b>效果样式</b> —— 基于 Maplespe 的 ExplorerBlurMica(官方 2.0.1)，"
        "为窗口添加 Blur / Acrylic / Mica / MicaAlt 系统级背景效果，亮暗色模式自适应。</p>"
        "<p style='color:#b9bcc4'>• 两项能力各自独立：只开图片、只开特效、或两个都开都可以。"
        "两边的“应用”只写自己那份配置、只注册自己那个 DLL，不会关掉另一项；"
        "两个都开时图片覆盖文件列表区，模糊/亚克力作为整窗底色。</p>"
        "<p style='color:#d5d8de'><b>生效方式</b>：程序需以管理员身份运行（自动弹出 UAC 确认）。"
        "点击“应用”后会写入配置、注册 DLL 并重启资源管理器；打开任意文件夹即可看到效果。</p>"
        "<p style='color:#d5d8de'><b>常见问题</b>：</p>"
        "<p style='color:#b9bcc4'>• 若 Windows 大版本更新后背景消失，重新点击“应用”即可。</p>"
        "<p style='color:#b9bcc4'>• 若资源管理器窗口无法打开，按住 <b>ESC</b> 键点击资源管理器可跳过背景加载，"
        "然后在本工具点击“恢复”。</p>"
        "<p style='color:#b9bcc4'>• “Home / 图库”页不显示背景，请进入任意文件夹查看。</p>"
        "<p style='color:#82858d'>致谢：SuryaMajumdar/ExplorerBgTool (MIT) · Maplespe/explorerTool · "
        "Maplespe/ExplorerBlurMica (LGPL-3.0)。本工具仅调用其官方 DLL 并生成配置。</p>");

    auto *body = new QLabel(text, card);
    body->setWordWrap(true);
    body->setTextFormat(Qt::RichText);
    body->setAlignment(Qt::AlignTop);
    cardLay->addWidget(body);
    lay->addWidget(card);
    lay->addStretch(1);
    scroll->setWidget(page);
    return scroll;
}

QWidget *MainWindow::buildImagePage()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget(scroll);
    auto *lay = new QHBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(14);

    auto *leftCard = new QFrame(page);
    leftCard->setObjectName(QStringLiteral("PageCard"));
    leftCard->setMinimumWidth(350); // 保证图片浏览三列网格的空间
    auto *leftLay = new QVBoxLayout(leftCard);
    leftLay->setContentsMargins(14, 12, 14, 14);
    leftLay->setSpacing(8);

    auto *galTitle = new QLabel(QStringLiteral("图片浏览"), leftCard);
    galTitle->setObjectName(QStringLiteral("GroupTitle"));
    leftLay->addWidget(galTitle);
    m_galleryList = new QListWidget(leftCard);
    auto *gallery = m_galleryList;
    gallery->setObjectName(QStringLiteral("GalleryList"));
    gallery->setProperty("imageCards", true);
    gallery->setViewMode(QListView::IconMode);
    gallery->setResizeMode(QListView::Adjust);
    gallery->setMovement(QListView::Static);
    gallery->setIconSize(QSize(140, 108));
    gallery->setGridSize(QSize(160, 160));
    gallery->setSpacing(0); // 间距由 gridSize 决定，避免叠加导致换列
    gallery->setWordWrap(false); // 标题单行显示，由委托负责省略
    gallery->setItemDelegate(new GalleryDelegate(gallery));
    gallery->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    gallery->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 列宽由视口宽度反算(updateGalleryGrid)，若让网格的 sizeHint 再反过来参与横向分配，
    // 会形成「宽度→列宽→sizeHint→宽度」的自激环：拖动右边框时界面无限重排直至未响应。
    gallery->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    connect(gallery, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            selectPreset(row);
    });
    gallery->viewport()->installEventFilter(this);
    leftLay->addWidget(gallery, 1);

    auto *folderRow = new QHBoxLayout();
    auto *folderBtn = new QPushButton(QStringLiteral("📁 选择文件夹"), leftCard);
    folderBtn->setObjectName(QStringLiteral("PrimaryButton"));
    folderBtn->setToolTip(tooltipstyle::format(QStringLiteral("读取文件夹内所有符合格式的图片并展示到图库")));
    connect(folderBtn, &QPushButton::clicked, this, &MainWindow::pickPresetFolder);
    auto *refreshBtn = new QPushButton(QStringLiteral("↻ 刷新"), leftCard);
    refreshBtn->setObjectName(QStringLiteral("LibraryScanButton"));
    refreshBtn->setToolTip(tooltipstyle::format(QStringLiteral("重新加载当前文件夹的图片")));
    connect(refreshBtn, &QPushButton::clicked, this, [this] {
        rebuildGallery();
        setLog(QStringLiteral("图库已刷新：共 %1 张图片。").arg(m_presets.size()), false);
    });
    folderRow->addWidget(folderBtn, 2);
    folderRow->addWidget(refreshBtn, 1);
    leftLay->addLayout(folderRow);

    m_imageSourceLabel = new QLabel(leftCard);
    m_imageSourceLabel->setObjectName(QStringLiteral("HintLabel"));
    m_imageSourceLabel->setWordWrap(true);
    // 长文件名不参与分栏宽度计算；文本按固定两行省略展示(见 setImageSourceText)
    m_imageSourceLabel->setMinimumWidth(1);
    m_imageSourceLabel->installEventFilter(this);
    leftLay->addWidget(m_imageSourceLabel);

    lay->addWidget(leftCard, 6);

    auto *rightCard = new QFrame(page);
    rightCard->setObjectName(QStringLiteral("PageCard"));
    auto *rightLay = new QVBoxLayout(rightCard);
    rightLay->setContentsMargins(14, 12, 14, 14);
    rightLay->setSpacing(8);

    auto *prevTitle = new QLabel(QStringLiteral("预览(模拟资源管理器窗口)"), rightCard);
    prevTitle->setObjectName(QStringLiteral("GroupTitle"));
    rightLay->addWidget(prevTitle);

    auto *prevFrame = new QFrame(rightCard);
    prevFrame->setObjectName(QStringLiteral("PreviewFrame"));
    auto *prevLay = new QVBoxLayout(prevFrame);
    prevLay->setContentsMargins(1, 1, 1, 1);
    m_previewLabel = new QLabel(prevFrame);
    m_previewLabel->setAlignment(Qt::AlignCenter); // 画布铺满预览框，居中仅作兜底
    m_previewLabel->setMinimumSize(1, 1);
    m_previewLabel->installEventFilter(this); // 尺寸变化时按新比例重绘
    prevLay->addWidget(m_previewLabel, 1);
    // 预览框宽高比锁死为桌面比例，保证「所见即所得」，不再随右侧卡片拉伸变形。
    m_previewFrame = prevFrame;
    m_previewFrame->installEventFilter(this);
    rightLay->addWidget(prevFrame, 0);
    // 桌面分辨率/主屏换了(外接屏、改缩放)比例要跟着重算。
    if (QScreen *screen = QGuiApplication::primaryScreen())
        connect(screen, &QScreen::geometryChanged, this, &MainWindow::updatePreviewAspect);
    connect(qApp, &QApplication::screenAdded, this, &MainWindow::updatePreviewAspect);
    connect(qApp, &QApplication::screenRemoved, this, &MainWindow::updatePreviewAspect);

    auto *adjTitleRow = new QHBoxLayout();
    adjTitleRow->setContentsMargins(0, 0, 0, 0);
    auto *adjTitle = new QLabel(QStringLiteral("调整参数"), rightCard);
    adjTitle->setObjectName(QStringLiteral("GroupTitle"));
    adjTitleRow->addWidget(adjTitle);
    adjTitleRow->addStretch(1);
    auto *resetParamsBtn = new QPushButton(QStringLiteral("↺ 重置"), rightCard);
    resetParamsBtn->setObjectName(QStringLiteral("ParamResetButton"));
    resetParamsBtn->setCursor(Qt::PointingHandCursor);
    resetParamsBtn->setToolTip(tooltipstyle::format(QStringLiteral(
            "恢复默认参数：尺寸100%、显示位置右下、亮度/对比度100%、模糊0、透明度0%，\n"
            "两个选项不勾选")));
    connect(resetParamsBtn, &QPushButton::clicked, this, &MainWindow::resetImageParams);
    adjTitleRow->addWidget(resetParamsBtn);
    rightLay->addLayout(adjTitleRow);

    auto *grid2 = new QGridLayout();
    grid2->setHorizontalSpacing(10);
    grid2->setVerticalSpacing(8);
    grid2->setColumnStretch(1, 1);

    auto addRow = [&](int row, const QString &label, QSlider *slider, QLabel *valLabel) {
        grid2->addWidget(new QLabel(label, rightCard), row, 0);
        grid2->addWidget(slider, row, 1);
        grid2->addWidget(valLabel, row, 2);
    };
    m_scale = makeSlider(10, 150, 100, &m_scaleVal, QStringLiteral("%"));
    m_scale->setToolTip(tooltipstyle::format(QStringLiteral(
            "调整图片相对文件浏览器窗口的比例，100% 表示按原图大小显示，用于“居中(原尺寸)”与四角模式；"
            "“填充窗口/拉伸填满”模式下图片始终铺满窗口，该调节无效果")));
    m_brightness = makeSlider(20, 200, 100, &m_brightnessVal, QStringLiteral("%"));
    m_contrast = makeSlider(50, 150, 100, &m_contrastVal, QStringLiteral("%"));
    m_blur = makeSlider(0, 20, 0, &m_blurVal, QStringLiteral("px"));
    m_transparency = makeSlider(0, ConfigKeys::Image::MaxTransparency, 0, &m_transparencyVal,
                                QStringLiteral("%"));
    m_transparency->setToolTip(tooltipstyle::format(QStringLiteral(
            "背景图透明度：0% 完全不透明，越往右越透明(最透 88%)，预览实时生效；\n"
            "旧版本此处为方向相反的「不透明度」滑杆，原设置已按相同效果自动换算")));

    addRow(0, QStringLiteral("尺寸"), m_scale, m_scaleVal);
    addRow(1, QStringLiteral("亮度"), m_brightness, m_brightnessVal);
    addRow(2, QStringLiteral("对比度"), m_contrast, m_contrastVal);
    addRow(3, QStringLiteral("模糊"), m_blur, m_blurVal);
    addRow(4, QStringLiteral("透明度"), m_transparency, m_transparencyVal);
    m_rotate = makeSlider(-180, 180, 0, &m_rotateVal, QStringLiteral("°"));
    m_rotate->setToolTip(tooltipstyle::format(QStringLiteral(
            "绕图片竖直中心轴旋转的投影：向左逆时针、向右顺时针，±180° 即左右镜像")));
    addRow(5, QStringLiteral("转动角度"), m_rotate, m_rotateVal);
    rightLay->addLayout(grid2);

    // 背景组件仅支持四角/居中/拉伸/填充
    auto *posLabel = new QLabel(QStringLiteral("显示位置"), rightCard);
    posLabel->setObjectName(QStringLiteral("FieldLabel"));
    rightLay->addWidget(posLabel);

    auto makePosBtn = [this](const QString &text, int mode) {
        auto *b = new QPushButton(text, this);
        b->setObjectName(QStringLiteral("PosButton"));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setMinimumHeight(30);
        connect(b, &QPushButton::clicked, this, [this, mode] { setPosMode(mode); });
        m_posButtons[mode] = b;
        return b;
    };
    auto makeDisabledPosBtn = [this](const QString &text) {
        auto *b = new QPushButton(text, this);
        b->setObjectName(QStringLiteral("PosButton"));
        b->setEnabled(false);
        b->setToolTip(tooltipstyle::format(QStringLiteral(
                "背景组件暂不支持边中点锚点，仅支持四角、居中、填充窗口与拉伸填满")));
        return b;
    };

    auto *posGrid = new QGridLayout();
    posGrid->setSpacing(6);
    posGrid->addWidget(makePosBtn(QStringLiteral("↖ 左上"), 3), 0, 0);
    posGrid->addWidget(makeDisabledPosBtn(QStringLiteral("↑ 上")), 0, 1);
    posGrid->addWidget(makePosBtn(QStringLiteral("↗ 右上"), 4), 0, 2);
    posGrid->addWidget(makeDisabledPosBtn(QStringLiteral("← 左")), 1, 0);
    posGrid->addWidget(makePosBtn(QStringLiteral("◎ 居中"), 1), 1, 1);
    posGrid->addWidget(makeDisabledPosBtn(QStringLiteral("→ 右")), 1, 2);
    posGrid->addWidget(makePosBtn(QStringLiteral("↙ 左下"), 5), 2, 0);
    posGrid->addWidget(makeDisabledPosBtn(QStringLiteral("↓ 下")), 2, 1);
    posGrid->addWidget(makePosBtn(QStringLiteral("↘ 右下"), 6), 2, 2);
    rightLay->addLayout(posGrid);

    auto *modeRow = new QHBoxLayout();
    modeRow->setSpacing(6);
    auto *fillBtn = makePosBtn(QStringLiteral("⊞ 填充窗口"), 0);
    fillBtn->setToolTip(tooltipstyle::format(QStringLiteral("等比缩放铺满整个窗口，超出部分裁剪")));
    auto *stretchBtn = makePosBtn(QStringLiteral("⤢ 拉伸填满"), 2);
    stretchBtn->setToolTip(tooltipstyle::format(QStringLiteral("忽略宽高比，拉伸至整个窗口")));
    modeRow->addWidget(fillBtn, 1);
    modeRow->addWidget(stretchBtn, 1);
    rightLay->addLayout(modeRow);

    m_folderExt = new QCheckBox(QStringLiteral("同时应用到文件打开/保存对话框"), rightCard);
    rightLay->addWidget(m_folderExt);

    auto *imgModeRow = new QHBoxLayout();
    imgModeRow->setSpacing(6);
    imgModeRow->addWidget(new QLabel(QStringLiteral("模式"), rightCard));
    imgModeRow->addStretch(1);
    m_imgModeSingle = new QRadioButton(QStringLiteral("单图"), rightCard);
    m_imgModeSingle->setToolTip(tooltipstyle::format(QStringLiteral(
            "点击应用后，所有资源管理器窗口固定使用当前选中的这一张背景图")));
    m_imgModeRandom = new QRadioButton(QStringLiteral("随机"), rightCard);
    m_imgModeRandom->setToolTip(tooltipstyle::format(QStringLiteral(
            "点击应用后，每打开一个新窗口、进入或退出文件夹，"
            "都从图片浏览列表里的图片中随机换一张作为背景")));
    m_imgModeSingle->setChecked(true);
    imgModeRow->addWidget(m_imgModeSingle);
    imgModeRow->addWidget(m_imgModeRandom);
    rightLay->addLayout(imgModeRow);

    auto saveImageMode = [this] {
        if (m_imgModeRandom && m_imgModeRandom->isChecked())
            AppConfig::instance().setValue(ConfigKeys::Image::Mode, 1);
        else
            AppConfig::instance().setValue(ConfigKeys::Image::Mode, 0);
    };
    connect(m_imgModeSingle, &QRadioButton::toggled, this, saveImageMode);

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    m_applyImageBtn = new QPushButton(QStringLiteral("✓ 应用图片背景"), rightCard);
    m_applyImageBtn->setObjectName(QStringLiteral("PrimaryButton"));
    auto *resetBtn = new QPushButton(QStringLiteral("↩ 恢复"), rightCard);
    resetBtn->setObjectName(QStringLiteral("DangerButton"));
    connect(m_applyImageBtn, &QPushButton::clicked, this, &MainWindow::applyImage);
    connect(resetBtn, &QPushButton::clicked, this, &MainWindow::uninstallImage);
    btnRow->addWidget(m_applyImageBtn, 1);
    btnRow->addWidget(resetBtn, 1);
    rightLay->addLayout(btnRow);
    // 预览框高度改成按桌面比例锁定后不再吃纵向拉伸，剩余空间统一留到卡片底部。
    rightLay->addStretch(1);

    lay->addWidget(rightCard, 5);

    // 滑条一个拖动会触发几十次 valueChanged，每次都跑完整条预览流水线
    // (LUT+3 遍 box blur+整幅预览重画)会卡 GUI 线程；接防抖入口，拖动中每 60ms
    // 才重画一次，与窗口 resize 的合并策略一致。
    for (QSlider *s : {m_rotate, m_scale, m_brightness, m_contrast, m_blur, m_transparency})
        connect(s, &QSlider::valueChanged, this, &MainWindow::scheduleImagePreview);

    scroll->setWidget(page);
    return scroll;
}
QString MainWindow::galleryThumbPath(const QString &image) const
{
    QFileInfo fi(image);
    const QString key = fi.absoluteFilePath() + QStringLiteral("|") +
                        QString::number(fi.lastModified().toMSecsSinceEpoch());
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());

    CachePaths::ensureDirectories();
    return QDir(CachePaths::galleryThumbs())
        .filePath(hash + QStringLiteral("_v2.png"));
}

void MainWindow::rebuildGallery()
{
    m_presets.clear();
    if (!m_presetDir.isEmpty() && QDir(m_presetDir).exists()) {
        const QStringList nameFilters = {QStringLiteral("*.png"), QStringLiteral("*.jpg"),
                                         QStringLiteral("*.jpeg"), QStringLiteral("*.bmp"),
                                         QStringLiteral("*.webp")};
        QDirIterator it(m_presetDir, nameFilters, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString rel = QDir(m_presetDir).relativeFilePath(path);
            QString name = rel;
            name.replace(QLatin1Char('/'), QStringLiteral(" · "));
            const int dot = name.lastIndexOf(QLatin1Char('.'));
            if (dot > 0)
                name = name.left(dot);
            m_presets.append({name, path, false});
        }
    }

    if (!m_galleryList)
        return;

    m_galleryList->blockSignals(true);
    m_galleryList->clear();
    for (int i = 0; i < m_presets.size(); ++i) {
        const QString &res = m_presets[i].res;
        auto *item = new QListWidgetItem(m_presets[i].name, m_galleryList);
        item->setData(Qt::UserRole, res);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);

        const QString thumb = galleryThumbPath(res);
        if (QFileInfo::exists(thumb)) {
            item->setIcon(QIcon(thumb));
        } else {
            // 后台线程只做解码+缩放+落盘，不触碰 UI；回到 GUI 线程的回调必须先过
            // 存活闸门(见 m_thumbTasksLive / ~MainWindow)。
            QThreadPool::globalInstance()->start([this, res, thumb] {
                // 解码内缩放：不再对 4K 素材先完整展开再缩，省峰值内存与解码时间
                QImage img = decodeDownscaled(res, QSize(300, 220));
                if (img.isNull())
                    return;
                // 完整显示(不裁剪)并保留透明通道：PNG 透明区域透出卡片底色
                img = img.scaled(300, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                img.save(thumb, "PNG");
                QMutexLocker guard(&m_thumbTasksMutex);
                if (!m_thumbTasksLive)
                    return; // 主窗口已销毁：缩略图已落盘，下次进入图库自然命中缓存
                QMetaObject::invokeMethod(this, [this, res, thumb] {
                    if (!m_galleryList)
                        return;
                    for (int r = 0; r < m_presets.size() && r < m_galleryList->count(); ++r) {
                        if (m_presets[r].res == res)
                            m_galleryList->item(r)->setIcon(QIcon(thumb));
                    }
                }, Qt::QueuedConnection);
            });
        }
    }
    m_galleryList->blockSignals(false);

    if (!m_presets.isEmpty()) {
        const int sel = qBound(0, m_selectedPreset, m_presets.size() - 1);
        m_galleryList->setCurrentRow(sel);
        setImageSourceText(QStringLiteral("当前选择：图片 · %1")
                               .arg(m_presets[sel].name));
    } else {
        m_selectedPreset = -1;
        setImageSourceText(QStringLiteral("暂无图片，可将图片放入浏览目录或点击“选择文件夹”"));
    }
    updateGalleryGrid(); // 新条目应用正方形网格块
    updateImagePreview();
}

void MainWindow::pickPresetFolder()
{
    // 默认从当前浏览目录开始选择，选中后持久化到配置
    QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择图片文件夹(其中图片将展示到图库)"), m_presetDir);
    if (dir.isEmpty())
        return;
    m_presetDir = dir;
    AppConfig::instance().setValue(ConfigKeys::Image::GalleryDir, dir);
    m_selectedPreset = 0; // 从该文件夹的第一张图开始展示
    rebuildGallery();
    setLog(QStringLiteral("图库已更新：共 %1 张图片(仅文件夹内)。").arg(m_presets.size()), false);
}

void MainWindow::selectPreset(int index)
{
    if (index < 0 || index >= m_presets.size())
        return;
    m_selectedPreset = index;
    m_customImage.clear();
    if (m_galleryList && m_galleryList->currentRow() != index)
        m_galleryList->setCurrentRow(index);
    setImageSourceText(QStringLiteral("当前选择：图片 · %1").arg(m_presets[index].name));
    updateImagePreview();
}

// 预览框宽高比锁死为主屏(桌面)比例，高度按宽度反算，使预览与真实桌面同形。
//
// ⚠️ 这里只登记「想要多高」，真正的 setFixedHeight 交给 applyPreviewAspect() 在事件循环里做。
// 原因(2026-09-23 实测)：本函数是被 m_previewFrame 自己的 Resize 事件调起来的，在里面同步
// 改高度会**立刻再发一次 Resize**；而新高度又改变父滚动区的滚动条状态、把宽度改回去 ——
// 拖窗口(尤其拖角、宽高同时变)时形成同步自激环，几轮下来进程直接 0xC0000005 退出，
// 现象就是用户报的「预览那栏上下抖一下，然后软件没了」。
// 推回事件循环后，这一轮布局彻底走完再改，拖动中的多次请求也自动合并成一次。
void MainWindow::updatePreviewAspect()
{
    if (!m_previewFrame)
        return;
    const int frameW = m_previewFrame->width();
    if (frameW < 2)          // 还没布局过(隐藏/宽度为 0)，等第一次 Resize 再算
        return;
    qreal aspect = 16.0 / 9.0;
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect sg = screen->geometry(); // 逻辑像素，DPR 在比值里自动抵消
        if (sg.height() > 0)
            aspect = qreal(sg.width()) / sg.height();
    }
    m_previewAspectPending = qMax(90, qRound(frameW / aspect));
    if (!m_previewAspectTimer) {
        m_previewAspectTimer = new QTimer(this);
        m_previewAspectTimer->setSingleShot(true);
        connect(m_previewAspectTimer, &QTimer::timeout, this, &MainWindow::applyPreviewAspect);
    }
    m_previewAspectTimer->start(0);
}

void MainWindow::applyPreviewAspect()
{
    if (!m_previewFrame || m_previewAspectPending <= 0)
        return;
    const int want = m_previewAspectPending;
    m_previewAspectPending = 0;
    // 迟滞 3px：滚动条一进一出只让预览框宽度变约 3.6px(左右列按 6:5 分宽度)、折算高度
    // 约 2.3px，落在死区里就不会来回翻转。死区一旦小于这个量，环就重新活过来。
    if (qAbs(want - m_previewFrame->height()) >= 3)
        m_previewFrame->setFixedHeight(want);
}

void MainWindow::updateImagePreview()
{
    if (!m_previewLabel || !m_brightness)
        return;
    QString path;
    if (m_selectedPreset >= 0 && m_selectedPreset < m_presets.size())
        path = m_presets[m_selectedPreset].res;
    else if (!m_customImage.isEmpty())
        path = m_customImage;

    // 源图按"路径+修改时间"缓存：调参和拖动窗口会反复重画，磁盘解码只做一次。
    // 解码直接按 900 预览宽收口：4K 图不再先展开几十 MB 再缩；真实原生尺寸从
    // 文件头取(不解码)，头信息无效时才退回解码结果。
    const qint64 mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    const QString srcKey = path + QLatin1Char('#') + QString::number(mtime);
    if (srcKey != m_previewSrcKey) {
        QImageReader reader(path);
        const QSize nativeSize = reader.size();
        if (nativeSize.isValid() && nativeSize.width() > 900)
            reader.setScaledSize(QSize(900, qMax(1, nativeSize.height() * 900 / nativeSize.width())));
        QImage decoded = reader.read();
        if (!decoded.isNull() && decoded.width() > 900)
            decoded = decoded.scaledToWidth(900, Qt::SmoothTransformation);
        m_previewSrcNative = nativeSize.isValid() && !nativeSize.isEmpty() ? nativeSize : decoded.size();
        m_previewSrcCache = decoded;
        m_previewSrcKey = srcKey;
    }
    const QImage src = m_previewSrcCache;
    const QSize native = m_previewSrcNative;
    QImage processed = ImageProcess::adjust(src, m_brightness->value() / 100.0,
                                            m_contrast->value() / 100.0, m_blur->value());
    processed = ImageProcess::rotateAroundY(processed, m_rotate->value());

    // 尺寸调节只对原尺寸(居中/四角)模式有效，填充/拉伸下宽高比不变。
    const double pct = m_scale ? m_scale->value() / 100.0 : 1.0;
    const QSize effNative(qRound(native.width() * pct), qRound(native.height() * pct));

    // 真实参照：主屏物理像素，用来把图片按真实比例画进预览。
    QSize realWin(1920, 1080);
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect sg = screen->geometry();
        const qreal sdpr = screen->devicePixelRatio();
        realWin = QSize(int(sg.width() * sdpr), int(sg.height() * sdpr));
    }

    // 画布铺满整个预览框 —— 框本身已被 updatePreviewAspect() 锁成桌面宽高比。
    const QSize labelSize = m_previewLabel->size();
    const QSize mock(qMax(160, labelSize.width()), qMax(90, labelSize.height()));
    m_previewRenderSize = labelSize;
    const qreal dpr = m_previewLabel->devicePixelRatioF();

    const int pos = qBound(0, m_posMode, 6);
    QImage preview = ImageProcess::mockExplorerPreview(
        processed, effNative, pos, transparencyToAlpha(m_transparency->value()),
        mock, dpr, realWin, m_darkTheme);
    m_previewLabel->setPixmap(QPixmap::fromImage(preview));
}

void MainWindow::setPosMode(int mode)
{
    m_posMode = qBound(0, mode, 6);
    for (int i = 0; i < 7; ++i)
        if (m_posButtons[i])
            m_posButtons[i]->setChecked(i == m_posMode);
    updateImagePreview();
}

void MainWindow::updateGalleryGrid()
{
    if (!m_galleryList)
        return;
    // 固定三列：网格块为正方形，上方图片(完整显示、保留透明)、下方标题
    const int w = m_galleryList->viewport()->width();
    const int colW = qMax(90, (w - 24) / 3);
    const QSize grid(colW, colW);
    if (m_galleryList->gridSize() != grid)
        m_galleryList->setGridSize(grid);
    const QSize icon(qMax(40, colW - 22), qMax(40, colW - 40));
    if (m_galleryList->iconSize() != icon)
        m_galleryList->setIconSize(icon);
    // 条目占满网格块(正方形)，卡片间隙由 QSS margin 提供
    for (int i = 0; i < m_galleryList->count(); ++i) {
        if (m_galleryList->item(i)->sizeHint() != grid)
            m_galleryList->item(i)->setSizeHint(grid);
    }
}

void MainWindow::resetImageParams()
{
    m_rotate->setValue(0);
    m_scale->setValue(100);
    m_brightness->setValue(100);
    m_contrast->setValue(100);
    m_blur->setValue(0);
    m_transparency->setValue(0);
    setPosMode(6); // 默认右下
    m_folderExt->setChecked(false);
    saveImageSettings();
    setLog(QStringLiteral("参数已恢复默认值，点击“应用图片背景”生效。"), false);
}
void MainWindow::applyImage()
{
    const bool randomMode = m_imgModeRandom && m_imgModeRandom->isChecked();
    QString path;
    if (!randomMode) {
        if (m_selectedPreset >= 0 && m_selectedPreset < m_presets.size())
            path = m_presets[m_selectedPreset].res;
        else if (!m_customImage.isEmpty())
            path = m_customImage;
        if (path.isEmpty()) {
            setLog(QStringLiteral("请先在图片浏览中选择图片，或使用“选择图片…”"), true);
            return;
        }
    }

    m_applyImageBtn->setEnabled(false);
    setLog(randomMode ? QStringLiteral("正在生成随机图片池…")
                      : QStringLiteral("正在处理图片…"), false);
    QCoreApplication::processEvents();

    QString err;
    Engine::instance().ensureDataDirs();

    // DLL 只扫描 folder 目录里的 *.png / *.jpg：单图指向成品图目录，随机指向图片池目录。
    QString imageDir;
    if (randomMode) {
        int count = 0;
        if (!buildRandomImagePool(&count, &err)) {
            setLog(err, true);
            m_applyImageBtn->setEnabled(true);
            return;
        }
        imageDir = Engine::imagePoolDir();
        setLog(QStringLiteral("随机图片池已生成 %1 张，正在写入配置…").arg(count), false);
    } else {
        QImage src(path);
        if (src.isNull()) {
            setLog(QStringLiteral("图片读取失败"), true);
            m_applyImageBtn->setEnabled(true);
            return;
        }
        const QImage processed = applyImageParams(src);
        if (!processed.save(Engine::processedImagePath(), "PNG")) {
            setLog(QStringLiteral("处理后的图片保存失败"), true);
            m_applyImageBtn->setEnabled(true);
            return;
        }
        imageDir = QFileInfo(Engine::processedImagePath()).absolutePath();
        setLog(QStringLiteral("正在写入配置…"), false);
    }
    QCoreApplication::processEvents();

    // posType: 0..3 corners, 4 center, 5 stretch, 6 zoom&fill
    static const int posMap[] = {6, 4, 5, 0, 1, 2, 3};
    int posType = posMap[qBound(0, m_posMode, 6)];

    if (!Engine::instance().writeImageConfig(imageDir, posType,
                                             transparencyToAlpha(m_transparency->value()),
                                             m_folderExt->isChecked(),
                                             randomMode, &err)) {
        setLog(err, true);
        m_applyImageBtn->setEnabled(true);
        return;
    }
    setLog(QStringLiteral("正在注册 DLL(需要管理员权限)…"), false);
    QCoreApplication::processEvents();

    // 只动图片 Hook：特效 Hook 的注册状态与配置原样保留。
    saveImageSettings();
    if (!Engine::instance().registerImageDll(&err)) {
        setLog(err, true);
        m_applyImageBtn->setEnabled(true);
        return;
    }
    if (m_folderExt->isChecked()) {
        // folderExt is read at DLL load; re-register so it takes effect now.
        Engine::instance().unregisterImageDll(nullptr);
        Engine::instance().registerImageDll(&err);
    }

    setLog(QStringLiteral("正在重启资源管理器…"), false);
    QCoreApplication::processEvents();
    Engine::restartExplorer(nullptr);
    refreshStatus();
    setLog(randomMode
               ? QStringLiteral("随机图片背景已应用！每打开或切换一个文件夹窗口都会换一张。")
               : QStringLiteral("图片背景已应用！打开任意文件夹即可查看效果。"),
           false);
    m_applyImageBtn->setEnabled(true);
}

QImage MainWindow::applyImageParams(const QImage &src) const
{
    QImage processed = ImageProcess::adjust(src, m_brightness->value() / 100.0,
                                            m_contrast->value() / 100.0, m_blur->value());
    processed = ImageProcess::rotateAroundY(processed, m_rotate->value());
    // DLL 没有缩放参数，只能缩放实际写入的图片(仅原尺寸模式；填充/拉伸始终铺满窗口)。
    const double pct = m_scale->value() / 100.0;
    const int uiPos = qBound(0, m_posMode, 6);
    if (qAbs(pct - 1.0) > 1e-3 && uiPos != 0 && uiPos != 2) {
        const QSize target(qRound(processed.width() * pct), qRound(processed.height() * pct));
        processed = processed.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return processed;
}

bool MainWindow::buildRandomImagePool(int *count, QString *error)
{
    if (count)
        *count = 0;
    if (m_presets.isEmpty()) {
        if (error)
            *error = QStringLiteral("图片浏览列表为空，随机模式没有可用图片；"
                                    "请先用「选择文件夹」导入图片。");
        return false;
    }

    const QString dir = Engine::imagePoolDir();
    QDir pool(dir);
    if (!pool.exists() && !QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("无法创建随机图片池目录：%1").arg(dir);
        return false;
    }
    // 旧池先清空：图库可能换过目录，残留的图会继续被随机抽到。
    for (const QFileInfo &old : pool.entryInfoList({QStringLiteral("*.png"),
                                                    QStringLiteral("*.jpg")}, QDir::Files))
        QFile::remove(old.absoluteFilePath());

    int made = 0;
    for (const PresetImage &preset : m_presets) {
        QImage src(preset.res);
        if (src.isNull())
            continue; // 坏图/格式不支持：跳过，不让整批失败
        const QImage processed = applyImageParams(src);
        const QString out = dir + QStringLiteral("/bg_%1.png")
                                        .arg(made + 1, 3, 10, QLatin1Char('0'));
        if (!processed.save(out, "PNG")) {
            if (error)
                *error = QStringLiteral("随机图片池写入失败：%1").arg(out);
            return false;
        }
        ++made;
        setLog(QStringLiteral("正在生成随机图片池 (%1/%2)…")
                   .arg(made).arg(m_presets.size()), false);
        QCoreApplication::processEvents();
    }

    if (made == 0) {
        if (error)
            *error = QStringLiteral("图片浏览列表里的图片都读取失败，无法生成随机池。");
        return false;
    }
    if (count)
        *count = made;
    return true;
}

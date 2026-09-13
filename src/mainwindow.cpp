#include "mainwindow.h"

#include "appinfo.h"
#include "videodiag.h"
#include "videowallpaper.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QDirIterator>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QAbstractItemView>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QDir>
#include <QImage>
#include <QListWidget>
#include <QMouseEvent>
#include <QPointer>
#include <QWindow>
#include <QPushButton>
#include <QPainterPath>
#include <QRegion>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTextStream>
#include <QThreadPool>
#include <functional>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include <QScreen>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>

namespace {
// 旧版 MinGW SDK 头文件可能缺失这些定义
constexpr UINT kDwmwaCornerPreference = 33;
constexpr UINT kDwmwcpRound = 2;
// 显示器电源开关(开/关屏)通知 GUID
const GUID kMonitorPowerOnGuid = {0x02731015, 0x4510, 0x4526,
                                  {0x9E, 0x9F, 0x91, 0xAA, 0x8C, 0xAB, 0x0F, 0x8A}};
}
#endif

namespace {

// 图库缩略图标题：一行小字，超宽以省略号截断
class GalleryDelegate : public QStyledItemDelegate
{
public:
    explicit GalleryDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->font.setPointSizeF(qMax(7.0, option->font.pointSizeF() - 1.0));
        option->fontMetrics = QFontMetrics(option->font);
        const int textW = option->rect.width() - 12;
        if (textW > 0)
            option->text = option->fontMetrics.elidedText(option->text, Qt::ElideRight, textW);
    }
};

} // namespace

// Defined next to the combo popup helpers; only ever called from the diagnostic hook
// at the end of the constructor, and declared here because the constructor comes first.
void runComboSelfTest(QWidget *window);

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(appinfo::windowTitle()); // 原生标题=Yumeiren(窗口行); 品牌名在自绘标题栏
    resize(990, 780);
    // 最小尺寸保证图片浏览/调整参数等模块不被挤压隐藏
    setMinimumSize(880, 680);
    // 标题栏自绘方案：保留 WS_THICKFRAME(圆角/阴影/贴边由 DWM 提供)，
    // 通过 WM_NCCALCSIZE 隐藏系统标题栏，WM_NCHITTEST 实现边缘缩放与标题拖动。

    // 立刻恢复上次的视频壁纸：媒体打开+解码器初始化约需 1.5-2s，必须赶在
    // 界面构建(图片库缩略图等)之前起跑，否则壁纸要多等近一秒才出现。
    // 失败提示延迟到事件循环启动(日志控件就绪)后再补发。
    {
        QSettings early = appinfo::settings();
        const QStringList earlyPlaylist =
            early.value(QStringLiteral("video/playlist")).toStringList();
        const bool earlyWasPlaying =
            early.value(QStringLiteral("video/wasPlaying"), false).toBool();
        if (!earlyPlaylist.isEmpty() && earlyWasPlaying) {
            VideoWallpaper::instance().setPlaylist(earlyPlaylist);
            QString err;
            if (!VideoWallpaper::instance().startPlaying(&err)) {
                const QString msg = err.isEmpty()
                    ? QStringLiteral("视频壁纸自动恢复失败") : err;
                QTimer::singleShot(0, this, [this, msg] { setLog(msg, true); });
            }
        } else if (earlyWasPlaying) {
            // wasPlaying 为真但列表为空：自动恢复被跳过，记录原因避免无声失败
            videodiag::log(videodiag::Level::Warning,
                QStringLiteral("自动恢复跳过: 上次标记播放中但播放列表为空"));
        }
    }

    // Effect presets collected from the three projects' default configs.
    m_effectPresets = {
        {QStringLiteral("亚克力 · 浅色"),
         QStringLiteral("Win10/11 · Acrylic 白色薄纱，适配浅色模式"),
         {1, true, true, true, false, 255, 255, 255, 200, 255, 255, 255, 200}},
        {QStringLiteral("亚克力 · 深色"),
         QStringLiteral("Win10/11 · Acrylic 深色模式最佳"),
         {1, true, true, true, false, 24, 24, 24, 160, 0, 0, 0, 120}},
        {QStringLiteral("Mica · 云母"),
         QStringLiteral("仅 Win11 · 系统材质自适应"),
         {2, true, true, true, false, 255, 255, 255, 130, 0, 0, 0, 100}},
        {QStringLiteral("MicaAlt · 云母强"),
         QStringLiteral("仅 Win11 · 更明显的云母材质"),
         {4, true, true, true, false, 255, 255, 255, 140, 0, 0, 0, 110}},
        {QStringLiteral("纯模糊"),
         QStringLiteral("Win10/11 · 无着色的清晰模糊"),
         {3, true, true, true, false, 255, 255, 255, 200, 255, 255, 255, 200}},
        {QStringLiteral("Win10 经典模糊"),
         QStringLiteral("Win10 · 灰色调经典模糊"),
         {1, true, true, false, false, 222, 222, 222, 200, 0, 0, 0, 120}},
    };

    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(buildTitleBar());

    auto *body = new QWidget(central);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(buildSidebar());

    auto *right = new QWidget(body);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(buildHeader());

    m_topStack = new QStackedWidget(right);

    // Top page 1: 文件夹美化 (three sub pages switched by header tabs).
    auto *folderPage = new QWidget(m_topStack);
    folderPage->setObjectName(QStringLiteral("ContentArea"));
    auto *folderLay = new QVBoxLayout(folderPage);
    folderLay->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget(folderPage);
    m_stack->addWidget(buildImagePage());  // 0
    m_stack->addWidget(buildEffectPage()); // 1
    m_stack->addWidget(buildHelpPage());   // 2
    folderLay->addWidget(m_stack, 1);

    // log line lives inside the 文件夹美化 page only
    auto *logBar = new QFrame(folderPage);
    logBar->setObjectName(QStringLiteral("Header"));
    auto *logLayout = new QHBoxLayout(logBar);
    logLayout->setContentsMargins(18, 6, 18, 6);
    m_logLabel = new QLabel(QStringLiteral("就绪。选择预设或自定义图片，然后点击“应用”。"), logBar);
    m_logLabel->setObjectName(QStringLiteral("LogLabel"));
    m_logLabel->setWordWrap(true);
    logLayout->addWidget(m_logLabel, 1);
    folderLay->addWidget(logBar);

    m_topStack->addWidget(folderPage);

    // Top page 2: 动态壁纸 (design placeholder).
    m_topStack->addWidget(buildWallpaperPage());

    rightLayout->addWidget(m_topStack, 1);

    bodyLayout->addWidget(right, 1);
    rootLayout->addWidget(body, 1);
    setCentralWidget(central);

#ifdef Q_OS_WIN
    // 订阅显示器开/关通知，供视频壁纸状态机在熄屏时自动暂停
    RegisterPowerSettingNotification(reinterpret_cast<HWND>(winId()),
                                     &kMonitorPowerOnGuid, 0);
    // Win11 原生圆角 + 阴影(无边框窗口需显式开启；Win10 上调用失败无害)
    HWND hwndSelf = reinterpret_cast<HWND>(winId());
    UINT pref = kDwmwcpRound;
    DwmSetWindowAttribute(hwndSelf, kDwmwaCornerPreference, &pref, sizeof(pref));
#endif

    m_nav->setCurrentRow(0);
    m_topStack->setCurrentIndex(0);
    selectHeaderTab(0);
    loadSettings();
    applyTheme(m_themeCombo->currentIndex());
    refreshStatus();
    // the legacy import ran before this window existed; report what it carried over
    if (!appinfo::migrationNotes().isEmpty())
        setLog(appinfo::migrationNotes().join(QStringLiteral(" ")), false);
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_themeMode == 0)
            applyTheme(0);
    });

    // 上次的视频壁纸已在构造函数开头恢复(抢先于界面构建)；loadSettings 会把
    // 音量/帧率/多屏等设置应用到已存在的播放管线。

    // TEMPORARY diagnostic build hook: FBS_COMBO_DEBUG=<dir> walks every combo box,
    // opens its popup and writes metrics plus a rendered PNG into that directory.
    if (qEnvironmentVariableIsSet("FBS_COMBO_DEBUG"))
        runComboSelfTest(this);
}

QWidget *MainWindow::buildTitleBar()
{
    m_titleBar = new QFrame(this);
    m_titleBar->setObjectName(QStringLiteral("TitleBar"));
    m_titleBar->setFixedHeight(34);
    auto *lay = new QHBoxLayout(m_titleBar);
    lay->setContentsMargins(12, 0, 6, 0);
    lay->setSpacing(2);

    auto *title = new QLabel(QStringLiteral("🌸 ") + appinfo::displayName(), m_titleBar);
    title->setObjectName(QStringLiteral("TitleText"));
    lay->addWidget(title);
    lay->addStretch(1);

    auto mkBtn = [this](const QString &text, const char *name) {
        auto *b = new QPushButton(text, m_titleBar);
        b->setObjectName(QLatin1String(name));
        b->setFixedSize(42, 26);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    m_titleMin = mkBtn(QStringLiteral("─"), "TitleButton");
    m_titleMax = mkBtn(QStringLiteral("□"), "TitleButton");
    m_titleClose = mkBtn(QStringLiteral("✕"), "TitleCloseButton");
    connect(m_titleMin, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(m_titleMax, &QPushButton::clicked, this, [this] {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
    });
    connect(m_titleClose, &QPushButton::clicked, this, &QWidget::close);
    lay->addWidget(m_titleMin);
    lay->addWidget(m_titleMax);
    lay->addWidget(m_titleClose);
    return m_titleBar;
}

QWidget *MainWindow::buildSidebar()
{
    auto *side = new QFrame(this);
    side->setObjectName(QStringLiteral("Sidebar"));
    side->setFixedWidth(196);
    auto *lay = new QVBoxLayout(side);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *title = new QLabel(appinfo::displayName(), side);
    title->setObjectName(QStringLiteral("AppTitle"));
    auto *subtitle = new QLabel(QStringLiteral("美化 · 动态壁纸"), side);
    subtitle->setObjectName(QStringLiteral("AppSubtitle"));
    lay->addWidget(title);
    lay->addWidget(subtitle);

    m_nav = new QListWidget(side);
    m_nav->setObjectName(QStringLiteral("NavList"));
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->addItem(QStringLiteral("🌸  文件夹美化"));
    m_nav->addItem(QStringLiteral("🎞  动态壁纸"));
    m_nav->setCurrentRow(0);
    connect(m_nav, &QListWidget::currentRowChanged, this, &MainWindow::switchPage);
    lay->addWidget(m_nav, 1);

    // —— 底部：主题 + 环境信息卡片 ——
    auto *sideCard = new QFrame(side);
    sideCard->setObjectName(QStringLiteral("SideCard"));
    auto *cardLay = new QVBoxLayout(sideCard);
    cardLay->setContentsMargins(12, 10, 12, 10);
    cardLay->setSpacing(6);

    auto *themeRow = new QHBoxLayout();
    themeRow->setSpacing(8);
    auto *themeLbl = new QLabel(QStringLiteral("主题"), sideCard);
    themeLbl->setObjectName(QStringLiteral("ThemeLabel"));
    themeLbl->setAlignment(Qt::AlignVCenter);
    themeRow->addWidget(themeLbl);
    m_themeCombo = new QComboBox(sideCard);
    m_themeCombo->addItems({QStringLiteral("跟随系统"), QStringLiteral("亮色"), QStringLiteral("深色")});
    styleCombo(m_themeCombo);
    connect(m_themeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        QSettings st = appinfo::settings();
        st.setValue(QStringLiteral("ui/theme"), idx);
        applyTheme(idx);
    });
    themeRow->addWidget(m_themeCombo, 1);
    cardLay->addLayout(themeRow);

    auto *cardSep = new QFrame(sideCard);
    cardSep->setObjectName(QStringLiteral("SideCardSep"));
    cardSep->setFrameShape(QFrame::HLine);
    cardLay->addWidget(cardSep);

    m_adminLabel = new QLabel(sideCard);
    m_adminLabel->setObjectName(QStringLiteral("EnvAdminLabel"));
    m_osLabel = new QLabel(sideCard);
    m_osLabel->setObjectName(QStringLiteral("EnvOsLabel"));
    cardLay->addWidget(m_adminLabel);
    cardLay->addWidget(m_osLabel);

    auto *cardWrap = new QHBoxLayout();
    cardWrap->setContentsMargins(10, 6, 10, 12);
    cardWrap->addWidget(sideCard);
    lay->addLayout(cardWrap);

    return side;
}

QWidget *MainWindow::buildHeader()
{
    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("Header"));
    header->setFixedHeight(50);
    auto *lay = new QHBoxLayout(header);
    lay->setContentsMargins(14, 6, 18, 6);
    lay->setSpacing(4);

    // Header tabs where the page title used to be.
    const QStringList tabs = {QStringLiteral("图片背景"), QStringLiteral("效果样式"),
                              QStringLiteral("使用说明")};
    for (int i = 0; i < tabs.size(); ++i) {
        auto *b = new QPushButton(tabs[i], header);
        b->setObjectName(QStringLiteral("HeaderTab"));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, i] { selectHeaderTab(i); });
        m_headerTabs.append(b);
        lay->addWidget(b);
    }
    lay->addSpacing(6);

    // Wallpaper tabs: only visible on the 动态壁纸 page.
    const QStringList wallTabs = {QStringLiteral("视频壁纸"), QStringLiteral("动态网页壁纸")};
    for (int i = 0; i < wallTabs.size(); ++i) {
        auto *b = new QPushButton(wallTabs[i], header);
        b->setObjectName(QStringLiteral("HeaderTab"));
        b->setCheckable(true);
        b->setVisible(false);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, i] { selectWallTab(i); });
        m_wallTabs.append(b);
        lay->addWidget(b);
    }
    lay->addStretch(1);

    // status chips live in the header but only on the 文件夹美化 page
    m_statusBox = new QWidget(header);
    auto *box = new QHBoxLayout(m_statusBox);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(4);
    auto *lb1 = new QLabel(QStringLiteral("图片背景"), m_statusBox);
    lb1->setObjectName(QStringLiteral("HeaderSub"));
    m_imageChip = new QLabel(m_statusBox);
    m_imageChip->setObjectName(QStringLiteral("StatusChip"));
    auto *lb2 = new QLabel(QStringLiteral("效果样式"), m_statusBox);
    lb2->setObjectName(QStringLiteral("HeaderSub"));
    m_effectChip = new QLabel(m_statusBox);
    m_effectChip->setObjectName(QStringLiteral("StatusChip"));
    box->addWidget(lb1);
    box->addWidget(m_imageChip);
    box->addSpacing(10);
    box->addWidget(lb2);
    box->addWidget(m_effectChip);
    lay->addWidget(m_statusBox);
    return header;
}

QSlider *MainWindow::makeSlider(int min, int max, int value, QLabel **valueLabel,
                                const QString &suffix)
{
    auto *s = new QSlider(Qt::Horizontal, this);
    s->setRange(min, max);
    s->setValue(value);
    auto *v = new QLabel(QString::number(value) + suffix, this);
    v->setObjectName(QStringLiteral("FieldLabel"));
    v->setMinimumWidth(56);
    v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(s, &QSlider::valueChanged, this, [v, suffix](int val) {
        v->setText(QString::number(val) + suffix);
    });
    *valueLabel = v;
    return s;
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::Resize) {
        if (m_galleryList && obj == m_galleryList->viewport()) {
            updateGalleryGrid(); // 视口宽度变化时重算三列正方形网格
        } else if (m_previewLabel && obj == m_previewLabel
                   && m_previewLabel->size() != m_previewRenderSize) {
            updateImagePreview(); // 预览区变化后按新尺寸重画
        } else if (m_imageSourceLabel && obj == m_imageSourceLabel) {
            setImageSourceText(m_sourceText); // 宽度变化后重新按两行省略
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::setImageSourceText(const QString &text)
{
    m_sourceText = text;
    if (!m_imageSourceLabel)
        return;
    const QFontMetrics fm(m_imageSourceLabel->fontMetrics());
    m_imageSourceLabel->setFixedHeight(fm.lineSpacing() * 2 + 2); // 恒为两行，防止高度跳动
    const int width = m_imageSourceLabel->contentsRect().width();
    m_imageSourceLabel->setText(width > 40
        ? elidedTwoLineText(m_sourceText, width) : m_sourceText);
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

QWidget *MainWindow::buildImagePage()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget(scroll);
    auto *lay = new QHBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(14);

    // ---- left: preset gallery ----
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
    connect(gallery, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            selectPreset(row);
    });
    // 预设图库固定双栏(见 eventFilter)；预览区尺寸变化时按比例重绘
    gallery->viewport()->installEventFilter(this);
    leftLay->addWidget(gallery, 1);

    // custom image buttons
    auto *folderBtn = new QPushButton(QStringLiteral("选择文件夹"), leftCard);
    folderBtn->setToolTip(QStringLiteral("读取文件夹内所有符合格式的图片并展示到图库"));
    connect(folderBtn, &QPushButton::clicked, this, &MainWindow::pickPresetFolder);
    leftLay->addWidget(folderBtn);
    auto *customRow = new QHBoxLayout();
    auto *pickBtn = new QPushButton(QStringLiteral("选择图片…"), leftCard);
    auto *wallBtn = new QPushButton(QStringLiteral("用桌面壁纸"), leftCard);
    pickBtn->setToolTip(QStringLiteral("选择任意 PNG/JPG/BMP/WebP 图片"));
    wallBtn->setToolTip(QStringLiteral("截取当前桌面壁纸作为背景"));
    connect(pickBtn, &QPushButton::clicked, this, &MainWindow::pickImage);
    connect(wallBtn, &QPushButton::clicked, this, &MainWindow::pickWallpaper);
    customRow->addWidget(pickBtn, 1);
    customRow->addWidget(wallBtn, 1);
    leftLay->addLayout(customRow);

    m_imageSourceLabel = new QLabel(leftCard);
    m_imageSourceLabel->setObjectName(QStringLiteral("HintLabel"));
    m_imageSourceLabel->setWordWrap(true);
    // 长文件名不参与分栏宽度计算；文本按固定两行省略展示(见 setImageSourceText)
    m_imageSourceLabel->setMinimumWidth(1);
    m_imageSourceLabel->installEventFilter(this);
    leftLay->addWidget(m_imageSourceLabel);

    lay->addWidget(leftCard, 6);

    // ---- right: preview + adjustments ----
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
    m_previewLabel->setAlignment(Qt::AlignCenter); // 画布按屏幕宽高比居中显示
    m_previewLabel->setMinimumSize(1, 180);
    m_previewLabel->installEventFilter(this); // 尺寸变化时按新比例重绘
    prevLay->addWidget(m_previewLabel, 1);
    rightLay->addWidget(prevFrame, 1);

    auto *adjTitleRow = new QHBoxLayout();
    adjTitleRow->setContentsMargins(0, 0, 0, 0);
    auto *adjTitle = new QLabel(QStringLiteral("调整参数"), rightCard);
    adjTitle->setObjectName(QStringLiteral("GroupTitle"));
    adjTitleRow->addWidget(adjTitle);
    adjTitleRow->addStretch(1);
    auto *resetParamsBtn = new QPushButton(QStringLiteral("↺ 重置"), rightCard);
    resetParamsBtn->setObjectName(QStringLiteral("ParamResetButton"));
    resetParamsBtn->setCursor(Qt::PointingHandCursor);
    resetParamsBtn->setToolTip(QStringLiteral(
        "恢复默认参数：尺寸100%、显示位置右下、亮度/对比度100%、模糊0、不透明度255，\n"
        "两个选项不勾选"));
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
    m_scale->setToolTip(QStringLiteral(
        "调整图片相对文件浏览器窗口的比例，100% 表示按原图大小显示，用于“居中(原尺寸)”与四角模式；"
        "“填充窗口/拉伸填满”模式下图片始终铺满窗口，该调节无效果"));
    m_brightness = makeSlider(20, 200, 100, &m_brightnessVal, QStringLiteral("%"));
    m_contrast = makeSlider(50, 150, 100, &m_contrastVal, QStringLiteral("%"));
    m_blur = makeSlider(0, 20, 0, &m_blurVal, QStringLiteral("px"));
    m_opacity = makeSlider(30, 255, 255, &m_opacityVal);

    addRow(0, QStringLiteral("尺寸"), m_scale, m_scaleVal);
    addRow(1, QStringLiteral("亮度"), m_brightness, m_brightnessVal);
    addRow(2, QStringLiteral("对比度"), m_contrast, m_contrastVal);
    addRow(3, QStringLiteral("模糊"), m_blur, m_blurVal);
    addRow(4, QStringLiteral("不透明度"), m_opacity, m_opacityVal);
    m_rotate = makeSlider(-180, 180, 0, &m_rotateVal, QStringLiteral("°"));
    m_rotate->setToolTip(QStringLiteral(
        "绕图片竖直中心轴旋转的投影：向左逆时针、向右顺时针，±180° 即左右镜像"));
    addRow(5, QStringLiteral("转动角度"), m_rotate, m_rotateVal);
    rightLay->addLayout(grid2);

    // 显示位置：3×3 方位网格 + 填充/拉伸模式(背景组件仅支持四角/居中/拉伸/填充)
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
        b->setToolTip(QStringLiteral(
            "背景组件暂不支持边中点锚点，仅支持四角、居中、填充窗口与拉伸填满"));
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
    fillBtn->setToolTip(QStringLiteral("等比缩放铺满整个窗口，超出部分裁剪"));
    auto *stretchBtn = makePosBtn(QStringLiteral("⤢ 拉伸填满"), 2);
    stretchBtn->setToolTip(QStringLiteral("忽略宽高比，拉伸至整个窗口"));
    modeRow->addWidget(fillBtn, 1);
    modeRow->addWidget(stretchBtn, 1);
    rightLay->addLayout(modeRow);

    m_folderExt = new QCheckBox(QStringLiteral("同时应用到文件打开/保存对话框"), rightCard);
    rightLay->addWidget(m_folderExt);
    m_comboEffect = new QCheckBox(
        QStringLiteral("叠加全窗口模糊/亚克力效果"), rightCard);
    m_comboEffect->setToolTip(
        QStringLiteral("启用 ExplorerBlurMica 作为整窗底色：图片覆盖文件列表区，"
                       "侧边栏等其余区域显示模糊/亚克力效果并融入背景"));
    rightLay->addWidget(m_comboEffect);

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    m_applyImageBtn = new QPushButton(QStringLiteral("应用图片背景"), rightCard);
    m_applyImageBtn->setObjectName(QStringLiteral("PrimaryButton"));
    auto *resetBtn = new QPushButton(QStringLiteral("恢复默认(卸载)"), rightCard);
    resetBtn->setObjectName(QStringLiteral("DangerButton"));
    connect(m_applyImageBtn, &QPushButton::clicked, this, &MainWindow::applyImage);
    connect(resetBtn, &QPushButton::clicked, this, &MainWindow::uninstallAll);
    btnRow->addWidget(m_applyImageBtn, 1);
    btnRow->addWidget(resetBtn, 1);
    rightLay->addLayout(btnRow);

    lay->addWidget(rightCard, 5);

    for (QSlider *s : {m_rotate, m_scale, m_brightness, m_contrast, m_blur, m_opacity})
        connect(s, &QSlider::valueChanged, this, &MainWindow::updateImagePreview);

    scroll->setWidget(page);
    return scroll;
}

QWidget *MainWindow::buildEffectPage()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget(scroll);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(14);

    // ---- preset cards ----
    auto *card = new QFrame(page);
    card->setObjectName(QStringLiteral("PageCard"));
    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(14, 12, 14, 14);
    cardLay->setSpacing(8);

    auto *t1 = new QLabel(QStringLiteral("预设效果样式(来自 ExplorerBlurMica)"), card);
    t1->setObjectName(QStringLiteral("GroupTitle"));
    cardLay->addWidget(t1);
    auto *hint = new QLabel(
        QStringLiteral("为资源管理器窗口添加 Blur / Acrylic / Mica 背景效果，兼容 Win10 与 Win11。"
                       "Mica 仅 Win11 可用；Win11 23H2 以上建议开启“清除 WinUI 工具栏背景”。"),
        card);
    hint->setObjectName(QStringLiteral("HintLabel"));
    hint->setWordWrap(true);
    cardLay->addWidget(hint);

    auto *flow = new QGridLayout();
    flow->setSpacing(10);
    for (int i = 0; i < m_effectPresets.size(); ++i) {
        const auto &p = m_effectPresets[i];
        auto *btn = new QPushButton(card);
        btn->setObjectName(QStringLiteral("PresetCard"));
        btn->setCheckable(true);
        btn->setMinimumSize(260, 62);
        btn->setText(QStringLiteral("%1\n%2").arg(p.name, p.desc));
        btn->setToolTip(p.desc);
        connect(btn, &QPushButton::clicked, this, [this, i] {
            m_selectedEffect = i;
            updateEffectPresetSelection(i);
            const auto &cfg = m_effectPresets[i].cfg;
            m_effectCombo->setCurrentIndex(cfg.effect);
            m_clearAddress->setChecked(cfg.clearAddress);
            m_clearBarBg->setChecked(cfg.clearBarBg);
            m_clearWinUIBg->setChecked(cfg.clearWinUIBg);
            m_showLine->setChecked(cfg.showLine);
            m_lightColor = QColor(cfg.lightR, cfg.lightG, cfg.lightB);
            m_darkColor = QColor(cfg.darkR, cfg.darkG, cfg.darkB);
            m_lightAlpha->setValue(cfg.lightA);
            m_darkAlpha->setValue(cfg.darkA);
            m_lightColorBtn->setStyleSheet(
                QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
                    .arg(m_lightColor.name()));
            m_darkColorBtn->setStyleSheet(
                QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
                    .arg(m_darkColor.name()));
            setLog(QStringLiteral("已选择预设：%1，点击“应用效果”生效。")
                       .arg(m_effectPresets[i].name), false);
        });
        m_effectButtons.append(btn);
        flow->addWidget(btn, i / 2, i % 2);
    }
    flow->setRowStretch(flow->rowCount(), 1);
    cardLay->addLayout(flow);

    // ---- custom settings ----
    auto *t2 = new QLabel(QStringLiteral("自定义参数"), card);
    t2->setObjectName(QStringLiteral("GroupTitle"));
    cardLay->addWidget(t2);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    grid->addWidget(new QLabel(QStringLiteral("效果类型"), card), 0, 0);
    m_effectCombo = new QComboBox(card);
    m_effectCombo->addItems({QStringLiteral("0 - Blur 模糊(≤Win11 22H2)"),
                             QStringLiteral("1 - Acrylic 亚克力"),
                             QStringLiteral("2 - Mica 云母(仅 Win11)"),
                             QStringLiteral("3 - Blur(Clear) 纯模糊"),
                             QStringLiteral("4 - MicaAlt(仅 Win11)")});
    m_effectCombo->setCurrentIndex(1);
    styleCombo(m_effectCombo);
    grid->addWidget(m_effectCombo, 0, 1, 1, 3);

    m_lightColorBtn = new QPushButton(QStringLiteral("亮色混合"), card);
    m_darkColorBtn = new QPushButton(QStringLiteral("暗色混合"), card);
    m_lightColorBtn->setStyleSheet(
        QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
            .arg(m_lightColor.name()));
    m_darkColorBtn->setStyleSheet(
        QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
            .arg(m_darkColor.name()));
    m_lightAlpha = makeSlider(0, 255, 200, &m_lightAlphaVal);
    m_darkAlpha = makeSlider(0, 255, 120, &m_darkAlphaVal);
    grid->addWidget(m_lightColorBtn, 1, 0);
    grid->addWidget(new QLabel(QStringLiteral("亮色透明度"), card), 1, 1);
    grid->addWidget(m_lightAlpha, 1, 2);
    grid->addWidget(m_lightAlphaVal, 1, 3);
    grid->addWidget(m_darkColorBtn, 2, 0);
    grid->addWidget(new QLabel(QStringLiteral("暗色透明度"), card), 2, 1);
    grid->addWidget(m_darkAlpha, 2, 2);
    grid->addWidget(m_darkAlphaVal, 2, 3);

    auto *optRow = new QHBoxLayout();
    m_clearAddress = new QCheckBox(QStringLiteral("清除地址栏背景"), card);
    m_clearBarBg = new QCheckBox(QStringLiteral("清除滚动条背景"), card);
    m_clearWinUIBg = new QCheckBox(QStringLiteral("清除 WinUI 工具栏背景(Win11)"), card);
    m_showLine = new QCheckBox(QStringLiteral("显示树视图分割线"), card);
    m_clearAddress->setChecked(true);
    m_clearBarBg->setChecked(true);
    m_clearWinUIBg->setChecked(true);
    optRow->addWidget(m_clearAddress);
    optRow->addWidget(m_clearBarBg);
    optRow->addWidget(m_clearWinUIBg);
    optRow->addWidget(m_showLine);
    grid->addLayout(optRow, 3, 0, 1, 4);
    cardLay->addLayout(grid);

    m_keepImage = new QCheckBox(
        QStringLiteral("叠加当前图片背景(使用“图片背景”页的图片与参数)"), card);
    cardLay->addWidget(m_keepImage);

    connect(m_lightColorBtn, &QPushButton::clicked, this, [this] {
        QColor c = QColorDialog::getColor(m_lightColor, this, QStringLiteral("亮色模式混合色"));
        if (c.isValid()) {
            m_lightColor = c;
            m_lightColorBtn->setStyleSheet(
                QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
                    .arg(c.name()));
            m_selectedEffect = -1;
            updateEffectPresetSelection(-1);
        }
    });
    connect(m_darkColorBtn, &QPushButton::clicked, this, [this] {
        QColor c = QColorDialog::getColor(m_darkColor, this, QStringLiteral("暗色模式混合色"));
        if (c.isValid()) {
            m_darkColor = c;
            m_darkColorBtn->setStyleSheet(
                QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
                    .arg(c.name()));
            m_selectedEffect = -1;
            updateEffectPresetSelection(-1);
        }
    });

    lay->addWidget(card);

    // ---- apply ----
    auto *btnCard = new QFrame(page);
    btnCard->setObjectName(QStringLiteral("PageCard"));
    auto *btnLay = new QHBoxLayout(btnCard);
    btnLay->setContentsMargins(14, 12, 14, 12);
    btnLay->setSpacing(10);
    m_applyEffectBtn = new QPushButton(QStringLiteral("应用效果样式"), btnCard);
    m_applyEffectBtn->setObjectName(QStringLiteral("PrimaryButton"));
    auto *resetBtn = new QPushButton(QStringLiteral("恢复默认(卸载)"), btnCard);
    resetBtn->setObjectName(QStringLiteral("DangerButton"));
    connect(m_applyEffectBtn, &QPushButton::clicked, this, &MainWindow::applyEffect);
    connect(resetBtn, &QPushButton::clicked, this, &MainWindow::uninstallAll);
    btnLay->addWidget(m_applyEffectBtn, 1);
    btnLay->addWidget(resetBtn, 1);
    lay->addWidget(btnCard);

    lay->addStretch(1);
    scroll->setWidget(page);
    return scroll;
}

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
        "默认浏览软件目录下的 media/image 文件夹，也可点击“选择文件夹”更换目录，"
        "支持亮度 / 对比度 / 模糊 / 不透明度 / 显示位置调整，"
        "可选扩展到文件打开、保存对话框。</p>"
        "<p style='color:#b9bcc4'>• <b>效果样式</b> —— 基于 Maplespe 的 ExplorerBlurMica(官方 2.0.1)，"
        "为窗口添加 Blur / Acrylic / Mica / MicaAlt 系统级背景效果，亮暗色模式自适应。</p>"
        "<p style='color:#b9bcc4'>• 勾选“叠加全窗口效果”时，图片覆盖文件列表区，模糊/亚克力覆盖整个窗口，两者合成完整背景。</p>"
        "<p style='color:#d5d8de'><b>生效方式</b>：程序需以管理员身份运行（自动弹出 UAC 确认）。"
        "点击“应用”后会写入配置、注册 DLL 并重启资源管理器；打开任意文件夹即可看到效果。</p>"
        "<p style='color:#d5d8de'><b>常见问题</b>：</p>"
        "<p style='color:#b9bcc4'>• 若 Windows 大版本更新后背景消失，重新点击“应用”即可。</p>"
        "<p style='color:#b9bcc4'>• 若资源管理器窗口无法打开，按住 <b>ESC</b> 键点击资源管理器可跳过背景加载，"
        "然后在本工具点击“恢复默认(卸载)”。</p>"
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

QWidget *MainWindow::buildVideoWallpaperPage()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget(scroll);
    auto *lay = new QHBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(14);

    // ---- left: start / pause + options ----
    auto *leftCard = new QFrame(page);
    leftCard->setObjectName(QStringLiteral("PageCard"));
    leftCard->setFixedWidth(250);
    auto *leftLay = new QVBoxLayout(leftCard);
    leftLay->setContentsMargins(14, 14, 14, 14);
    leftLay->setSpacing(10);

    auto *t = new QLabel(QStringLiteral("视频壁纸"), leftCard);
    t->setObjectName(QStringLiteral("GroupTitle"));
    leftLay->addWidget(t);
    auto *hint = new QLabel(
        QStringLiteral("画面显示在桌面图标之后，程序运行期间有效。"), leftCard);
    hint->setObjectName(QStringLiteral("HintLabel"));
    hint->setWordWrap(true);
    leftLay->addWidget(hint);

    m_playBtn = new QPushButton(QStringLiteral("启动"), leftCard);
    m_playBtn->setObjectName(QStringLiteral("PrimaryButton"));
    m_playBtn->setMinimumHeight(40);
    connect(m_playBtn, &QPushButton::clicked, this, &MainWindow::startVideo);
    m_pauseBtn = new QPushButton(QStringLiteral("暂停"), leftCard);
    m_pauseBtn->setMinimumHeight(40);
    m_pauseBtn->setEnabled(false);
    connect(m_pauseBtn, &QPushButton::clicked, this, [] {
        VideoWallpaper::instance().pauseResume();
    });
    m_stopBtn = new QPushButton(QStringLiteral("取消"), leftCard);
    m_stopBtn->setMinimumHeight(40);
    m_stopBtn->setEnabled(false);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::stopVideo);
    auto *playRow = new QHBoxLayout();
    playRow->setSpacing(10);
    playRow->addWidget(m_playBtn, 1);
    playRow->addWidget(m_pauseBtn, 1);
    playRow->addWidget(m_stopBtn, 1);
    leftLay->addLayout(playRow);

    auto *volRow = new QHBoxLayout();
    volRow->addWidget(new QLabel(QStringLiteral("音量"), leftCard));
    m_videoVolume = new QSlider(Qt::Horizontal, leftCard);
    m_videoVolume->setRange(0, 100);
    m_videoVolume->setValue(0);
    auto *volVal = new QLabel(QStringLiteral("0"), leftCard);
    volVal->setObjectName(QStringLiteral("FieldLabel"));
    volVal->setMinimumWidth(34);
    volVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(m_videoVolume, &QSlider::valueChanged, this, [volVal](int v) {
        volVal->setText(QString::number(v));
        VideoWallpaper::instance().setVolume(v);
    });
    volRow->addWidget(m_videoVolume, 1);
    volRow->addWidget(volVal);
    leftLay->addLayout(volRow);

    m_autoLoopBox = new QCheckBox(QStringLiteral("列表循环"), leftCard);
    m_autoLoopBox->setChecked(true);
    m_autoLoopBox->setToolTip(QStringLiteral(
        "勾选：列表逐个播放，播完最后一个回到第一个继续；单个视频自动从头循环。\n"
        "不勾选：列表播放一遍后停止。"));
    m_randomBox = new QCheckBox(QStringLiteral("随机播放"), leftCard);
    m_fullscreenPauseBox = new QCheckBox(QStringLiteral("全屏自动暂停"), leftCard);
    m_fullscreenPauseBox->setChecked(true);
    m_fullscreenPauseBox->setToolTip(QStringLiteral(
        "前台应用全屏或完全遮住桌面时暂停视频壁纸(省 GPU/电量)，回到桌面 1 秒内自动恢复"));
    m_batteryBox = new QCheckBox(QStringLiteral("电池模式自动暂停"), leftCard);
    m_batteryBox->setToolTip(QStringLiteral("使用电池供电时自动暂停视频壁纸以省电，接通电源后自动恢复"));
    leftLay->addWidget(m_autoLoopBox);
    leftLay->addWidget(m_randomBox);
    leftLay->addWidget(m_fullscreenPauseBox);
    leftLay->addWidget(m_batteryBox);

    m_reclaimBox = new QCheckBox(QStringLiteral("自动回收内存"), leftCard);
    m_reclaimBox->setChecked(true);
    m_reclaimBox->setToolTip(QStringLiteral("定期把空闲内存还给系统，控制内存占用"));
    leftLay->addWidget(m_reclaimBox);
    m_affinityBox = new QCheckBox(QStringLiteral("资源友好模式"), leftCard);
    m_affinityBox->setChecked(true);
    m_affinityBox->setToolTip(QStringLiteral(
        "把本程序限制到最多 4 个逻辑核：解码线程与内存/显存占用随之下降\n"
        "(实测 1080p 内存 -27%、显存 -36%，CPU 不变)。更改后重启生效。"));
    leftLay->addWidget(m_affinityBox);
    connect(m_affinityBox, &QCheckBox::toggled, this, [this](bool on) {
        QSettings st = appinfo::settings();
        st.setValue(QStringLiteral("video/affinityLimit"), on);
    });
    m_autostartBox = new QCheckBox(QStringLiteral("开机自动启动"), leftCard);
    m_autostartBox->setChecked(VideoWallpaper::instance().autostartEnabled());
    leftLay->addWidget(m_autostartBox);

    auto *modeRow = new QHBoxLayout();
    modeRow->addWidget(new QLabel(QStringLiteral("多屏"), leftCard));
    m_screenModeCombo = new QComboBox(leftCard);
    m_screenModeCombo->addItems({QStringLiteral("主屏显示"), QStringLiteral("全屏拉伸"),
                                 QStringLiteral("多屏镜像")});
    styleCombo(m_screenModeCombo);
    modeRow->addWidget(m_screenModeCombo, 1);
    leftLay->addLayout(modeRow);

    // 帧率上限：默认 30 FPS；仅当视频帧率高于上限时生效(适当放慢呈现节奏)
    auto *fpsRow = new QHBoxLayout();
    fpsRow->addWidget(new QLabel(QStringLiteral("帧率上限"), leftCard));
    m_fpsBox = new QComboBox(leftCard);
    m_fpsBox->addItems({QStringLiteral("跟随视频"), QStringLiteral("15 FPS"),
                        QStringLiteral("24 FPS"), QStringLiteral("30 FPS"),
                        QStringLiteral("60 FPS")});
    m_fpsBox->setCurrentIndex(2); // 默认 24 FPS(省内存/显存/CPU；高于24fps的素材为慢动作)
    styleCombo(m_fpsBox);
    m_fpsBox->setToolTip(QStringLiteral(
        "限制壁纸呈现帧率：视频帧率高于上限时按上限放慢呈现节奏(画面为慢动作效果)；"
        "“跟随视频”保持原生帧率"));
    connect(m_fpsBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        static const int fpsValues[] = {0, 15, 24, 30, 60};
        VideoWallpaper::instance().setTargetFps(fpsValues[qBound(0, index, 4)]);
        QSettings st = appinfo::settings();
        st.setValue(QStringLiteral("video/targetFps"), fpsValues[qBound(0, index, 4)]);
    });
    fpsRow->addWidget(m_fpsBox, 1);
    leftLay->addLayout(fpsRow);

    leftLay->addStretch(1);
    lay->addWidget(leftCard);

    // ---- right: playlist + vertical action strip ----
    auto *rightCard = new QFrame(page);
    rightCard->setObjectName(QStringLiteral("PageCard"));
    auto *rightLay = new QVBoxLayout(rightCard);
    rightLay->setContentsMargins(14, 12, 14, 14);
    rightLay->setSpacing(8);

    auto *listRow = new QHBoxLayout();
    listRow->setSpacing(8);
    m_videoList = new QListWidget(rightCard);
    m_videoList->setObjectName(QStringLiteral("VideoList"));
    m_videoList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_videoList->setUniformItemSizes(false);
    m_videoList->setSpacing(4);
    m_videoList->setToolTip(QStringLiteral(
        "运行中双击条目：立即切换该视频为壁纸；选中条目后点“启动”：从该视频开始播放"));
    // 运行中双击列表条目 → 立即切换该视频为动态壁纸
    connect(m_videoList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
        auto &vp = VideoWallpaper::instance();
        if (!vp.isStarted() || vp.playlist().isEmpty())
            return; // 未启动时双击仅作选中
        const int row = m_videoList->row(item);
        if (row >= 0 && row < vp.playlist().size())
            vp.switchToTrack(row);
    });
    listRow->addWidget(m_videoList, 1);

    auto *strip = new QVBoxLayout();
    strip->setSpacing(8);
    auto addStripBtn = [&](const QString &text, auto slot) {
        auto *b = new QPushButton(text, rightCard);
        b->setMinimumWidth(72);
        b->setMinimumHeight(38);
        connect(b, &QPushButton::clicked, this, slot);
        strip->addWidget(b);
        return b;
    };
    addStripBtn(QStringLiteral("扫描"), [this] { scanVideoDir(); });
    addStripBtn(QStringLiteral("添加"), [this] { addVideos(); });
    addStripBtn(QStringLiteral("删除"), [this] { removeSelectedVideos(); });
    addStripBtn(QStringLiteral("清空"), [this] { clearVideos(); });
    strip->addStretch(1);
    listRow->addLayout(strip);
    rightLay->addLayout(listRow, 1);

    m_videoStatus = new QLabel(QStringLiteral("共 0 个视频 · 停止"), rightCard);
    m_videoStatus->setObjectName(QStringLiteral("HintLabel"));
    rightLay->addWidget(m_videoStatus);

    lay->addWidget(rightCard, 1);

    // option changes apply immediately
    connect(m_autoLoopBox, &QCheckBox::toggled, this, [this](bool on) {
        VideoWallpaper::instance().setAutoLoop(on);
        QSettings st = appinfo::settings();
        st.setValue(QStringLiteral("video/autoLoop"), on);
    });
    connect(m_randomBox, &QCheckBox::toggled, this, [this](bool on) {
        VideoWallpaper::instance().setRandom(on);
        QSettings st = appinfo::settings();
        st.setValue(QStringLiteral("video/random"), on);
    });
    connect(m_fullscreenPauseBox, &QCheckBox::toggled, this, [this](bool on) {
        VideoWallpaper::instance().setPauseOnFullscreen(on);
        QSettings st = appinfo::settings();
        st.setValue(QStringLiteral("video/pauseFullscreen"), on);
    });
    connect(m_batteryBox, &QCheckBox::toggled, this, [this](bool on) {
        VideoWallpaper::instance().setPauseOnBattery(on);
        QSettings st = appinfo::settings();
        st.setValue(QStringLiteral("video/pauseBattery"), on);
    });
    connect(m_screenModeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        VideoWallpaper::instance().setScreenMode(idx);
        QSettings st = appinfo::settings();
        st.setValue(QStringLiteral("video/screenMode"), idx);
    });
    connect(m_reclaimBox, &QCheckBox::toggled, this, [this](bool on) {
        VideoWallpaper::instance().setReclaimMemory(on);
        QSettings st = appinfo::settings();
        st.setValue(QStringLiteral("video/reclaim"), on);
    });
    connect(m_autostartBox, &QCheckBox::toggled, this, [this](bool on) {
        VideoWallpaper::instance().setAutostart(on);
    });
    connect(&VideoWallpaper::instance(), &VideoWallpaper::playbackStateChanged,
            this, &MainWindow::onVideoStateChanged);

    refreshVideoList();
    scroll->setWidget(page);
    return scroll;
}

QWidget *MainWindow::buildWebWallpaperPage()
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

    auto *row1 = new QHBoxLayout();
    auto *t = new QLabel(QStringLiteral("动态网页壁纸"), card);
    t->setObjectName(QStringLiteral("GroupTitle"));
    row1->addWidget(t);
    row1->addStretch(1);
    auto *badge = new QLabel(QStringLiteral("🚧 功能开发中 · 界面预览"), card);
    badge->setObjectName(QStringLiteral("HintLabel"));
    row1->addWidget(badge);
    cardLay->addLayout(row1);

    auto *hint = new QLabel(
        QStringLiteral("计划支持将网页(HTML5)作为桌面动态壁纸，基于浏览器内核离屏渲染，低资源占用。"),
        card);
    hint->setObjectName(QStringLiteral("HintLabel"));
    hint->setWordWrap(true);
    cardLay->addWidget(hint);

    auto *urlRow = new QHBoxLayout();
    auto *urlEdit = new QLineEdit(card);
    urlEdit->setPlaceholderText(QStringLiteral("网页地址 https://… 或本地 HTML 文件路径"));
    urlEdit->setDisabled(true);
    auto *pickBtn = new QPushButton(QStringLiteral("选择 HTML…"), card);
    pickBtn->setDisabled(true);
    urlRow->addWidget(urlEdit, 1);
    urlRow->addWidget(pickBtn);
    cardLay->addLayout(urlRow);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->addWidget(new QLabel(QStringLiteral("刷新策略"), card), 0, 0);
    auto *refreshCombo = new QComboBox(card);
    refreshCombo->addItems({QStringLiteral("实时渲染"), QStringLiteral("每分钟刷新"),
                            QStringLiteral("每小时刷新")});
    refreshCombo->setDisabled(true);
    styleCombo(refreshCombo);
    grid->addWidget(refreshCombo, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("交互模式"), card), 1, 0);
    auto *interCombo = new QComboBox(card);
    interCombo->addItems({QStringLiteral("允许鼠标交互"), QStringLiteral("仅展示(穿透点击)")});
    interCombo->setDisabled(true);
    styleCombo(interCombo);
    grid->addWidget(interCombo, 1, 1);
    cardLay->addLayout(grid);

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    auto *applyBtn = new QPushButton(QStringLiteral("应用网页壁纸"), card);
    applyBtn->setObjectName(QStringLiteral("PrimaryButton"));
    applyBtn->setDisabled(true);
    auto *resetBtn = new QPushButton(QStringLiteral("恢复默认(卸载)"), card);
    resetBtn->setObjectName(QStringLiteral("DangerButton"));
    resetBtn->setDisabled(true);
    btnRow->addWidget(applyBtn, 1);
    btnRow->addWidget(resetBtn, 1);
    cardLay->addLayout(btnRow);

    lay->addWidget(card);
    lay->addStretch(1);
    scroll->setWidget(page);
    return scroll;
}

QWidget *MainWindow::buildWallpaperPage()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("ContentArea"));
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    m_wallStack = new QStackedWidget(page);
    m_wallStack->addWidget(buildVideoWallpaperPage()); // 0
    m_wallStack->addWidget(buildWebWallpaperPage());   // 1
    lay->addWidget(m_wallStack);
    return page;
}

void MainWindow::switchPage(int row)
{
    if (row < 0)
        return;
    m_topStack->setCurrentIndex(row);
    for (auto *t : m_headerTabs)
        t->setVisible(row == 0);
    for (auto *t : m_wallTabs)
        t->setVisible(row == 1);
    m_statusBox->setVisible(row == 0);
    if (row == 0)
        selectHeaderTab(m_stack->currentIndex());
    else
        selectWallTab(m_wallStack->currentIndex());
}

void MainWindow::selectHeaderTab(int index)
{
    if (index < 0 || index >= m_headerTabs.size())
        return;
    for (int i = 0; i < m_headerTabs.size(); ++i)
        m_headerTabs[i]->setChecked(i == index);
    m_stack->setCurrentIndex(index);
    if (index == 0)
        updateImagePreview(); // re-render preview at the new size
}

void MainWindow::selectWallTab(int index)
{
    if (index < 0 || index >= m_wallTabs.size())
        return;
    for (int i = 0; i < m_wallTabs.size(); ++i)
        m_wallTabs[i]->setChecked(i == index);
    m_wallStack->setCurrentIndex(index);
}

QString MainWindow::galleryThumbPath(const QString &image) const
{
    QFileInfo fi(image);
    const QString key = fi.absoluteFilePath() + QStringLiteral("|") +
                        QString::number(fi.lastModified().toMSecsSinceEpoch());
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
    QDir().mkpath(Engine::dataRoot() + QStringLiteral("/thumbs"));
    // v2: PNG 保留透明通道(旧 JPEG 缩略图作废)
    return Engine::dataRoot() + QStringLiteral("/thumbs/") + hash
           + QStringLiteral("_v2.png");
}

void MainWindow::rebuildGallery()
{
    // 图片浏览：内容全部来自当前浏览目录(启动时默认 软件目录/media/image)，
    // 目录为空则图库留空；不再包含任何内置预设图片。
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

    // Lazy population: QListWidget paints only visible items, and each icon
    // comes from the disk thumbnail cache (generated in worker threads).
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
            QThreadPool::globalInstance()->start([this, res, thumb] {
                QImage img(res);
                if (img.isNull())
                    return;
                // 完整显示(不裁剪)并保留透明通道：PNG 透明区域透出卡片底色
                img = img.scaled(300, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                img.save(thumb, "PNG");
                QMetaObject::invokeMethod(this, [this, res, thumb] {
                    if (!m_galleryList)
                        return;
                    // find the row by path; ignore if the list was rebuilt
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
    // 默认从当前浏览目录(软件目录/media/image)开始选择；仅本次会话生效
    QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择图片文件夹(其中图片将展示到图库)"), m_presetDir);
    if (dir.isEmpty())
        return;
    m_presetDir = dir;
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

void MainWindow::pickImage()
{
    QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择背景图片"), QString(),
        QStringLiteral("图片文件 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*)"));
    if (file.isEmpty())
        return;
    QImage img(file);
    if (img.isNull()) {
        setLog(QStringLiteral("无法读取图片：%1").arg(file), true);
        return;
    }
    m_customImage = file;
    m_selectedPreset = -1;
    for (auto *b : m_presetButtons)
        b->setChecked(false);
    setImageSourceText(QStringLiteral("当前选择：自定义图片 · %1")
                           .arg(QFileInfo(file).fileName()));
    updateImagePreview();
}

void MainWindow::pickWallpaper()
{
    QString src = QDir::fromNativeSeparators(
        qEnvironmentVariable("APPDATA")
        + QStringLiteral("/Microsoft/Windows/Themes/TranscodedWallpaper"));
    if (!QFileInfo::exists(src)) {
        setLog(QStringLiteral("未能获取桌面壁纸(可能是纯色桌面)"), true);
        return;
    }
    QImage img(src);
    if (img.isNull()) {
        setLog(QStringLiteral("壁纸读取失败"), true);
        return;
    }
    Engine::instance().ensureDataDirs();
    QString dst = Engine::wallpaperPath();
    img.convertToFormat(QImage::Format_RGB32).save(dst, "JPEG", 95);
    m_customImage = dst;
    m_selectedPreset = -1;
    for (auto *b : m_presetButtons)
        b->setChecked(false);
    setImageSourceText(QStringLiteral("当前选择：桌面壁纸"));
    updateImagePreview();
}

void MainWindow::updateImagePreview()
{
    if (!m_previewLabel || !m_brightness)  // still constructing the page
        return;
    QString path;
    if (m_selectedPreset >= 0 && m_selectedPreset < m_presets.size())
        path = m_presets[m_selectedPreset].res;
    else if (!m_customImage.isEmpty())
        path = m_customImage;

    QImage src(path);
    const QSize native = src.size();
    if (!src.isNull() && src.width() > 900)
        src = src.scaledToWidth(900, Qt::SmoothTransformation); // fast preview path
    QImage processed = ImageProcess::adjust(src, m_brightness->value() / 100.0,
                                            m_contrast->value() / 100.0, m_blur->value());
    processed = ImageProcess::rotateAroundY(processed, m_rotate->value());

    // 尺寸调节：原尺寸(居中/四角)模式下按比例放大/缩小，与真实写入的图片一致；
    // 填充/拉伸模式下宽高比不变，预览结果不受影响，与真实行为一致。
    const double pct = m_scale ? m_scale->value() / 100.0 : 1.0;
    const QSize effNative(qRound(native.width() * pct), qRound(native.height() * pct));

    // 真实参照：主屏(物理像素)。预览画布宽高比与主屏一致，
    // 随预览区宽度等比例缩放，在预览区内居中。
    double screenRatio = 16.0 / 9.0;
    QSize realWin(1920, 1080);
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect sg = screen->geometry();
        screenRatio = double(sg.width()) / double(sg.height());
        const qreal sdpr = screen->devicePixelRatio();
        realWin = QSize(int(sg.width() * sdpr), int(sg.height() * sdpr));
    }

    const QSize labelSize = m_previewLabel->size();
    QSize mock(380, qRound(380 / screenRatio));
    if (labelSize.width() >= 80 && labelSize.height() >= 80) {
        int w = labelSize.width();
        int h = qRound(w / screenRatio);
        if (h > labelSize.height()) { // 预览区高度不足时以高度为准等比缩小
            h = labelSize.height();
            w = qRound(h * screenRatio);
        }
        mock = QSize(qMax(80, w), qMax(60, h));
    }
    m_previewRenderSize = labelSize;
    const qreal dpr = m_previewLabel->devicePixelRatioF();

    const int pos = qBound(0, m_posMode, 6);
    QImage preview = ImageProcess::mockExplorerPreview(
        processed, effNative, pos, m_opacity->value(), mock, dpr, realWin, m_darkTheme);
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
    m_opacity->setValue(255);
    setPosMode(6); // 默认右下
    m_folderExt->setChecked(false);
    m_comboEffect->setChecked(false);
    saveImageSettings();
    setLog(QStringLiteral("参数已恢复默认值，点击“应用图片背景”生效。"), false);
}

void MainWindow::updateEffectPresetSelection(int index)
{
    for (int i = 0; i < m_effectButtons.size(); ++i)
        m_effectButtons[i]->setChecked(i == index);
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" && message) {
        MSG *msg = static_cast<MSG *>(message);
        // 移除系统标题栏与边框(自绘标题栏替代)；保留 WS_THICKFRAME 以获得
        // DWM 阴影、Win11 圆角、Aero Snap 与边缘缩放。
        if (msg->message == WM_NCCALCSIZE && msg->wParam == TRUE) {
            auto *r = reinterpret_cast<RECT *>(msg->lParam);
            if (IsZoomed(msg->hwnd)) {
                // 最大化时客户区收缩一圈边框宽度，避免内容超出屏幕
                const int inset = GetSystemMetrics(SM_CXSIZEFRAME)
                                  + GetSystemMetrics(SM_CXPADDEDBORDER);
                r->left += inset;
                r->top += inset;
                r->right -= inset;
                r->bottom -= inset;
            }
            *result = 0;
            return true;
        }
        if (msg->message == WM_POWERBROADCAST) {
            // 显示器开关/系统休眠 → 通知视频壁纸状态机
            bool monitorOn;
            if (msg->wParam == PBT_POWERSETTINGCHANGE) {
                const auto *setting = reinterpret_cast<const POWERBROADCAST_SETTING *>(msg->lParam);
                if (!IsEqualGUID(setting->PowerSetting, kMonitorPowerOnGuid)
                    || setting->DataLength < sizeof(DWORD))
                    return false;
                monitorOn = *reinterpret_cast<const DWORD *>(setting->Data) != 0;
            } else if (msg->wParam == PBT_APMSUSPEND) {
                monitorOn = false;
            } else if (msg->wParam == PBT_APMRESUMEAUTOMATIC
                       || msg->wParam == PBT_APMRESUMESUSPEND) {
                monitorOn = true;
            } else {
                return false;
            }
            VideoWallpaper::instance().setMonitorOn(monitorOn);
            *result = TRUE;
            return true;
        }
        if (msg->message == WM_NCHITTEST && m_titleBar) {
            // 无边框窗口：让 Windows 处理边缘缩放与标题栏拖动(保留贴边、Aero Snap)
            const qreal dpr = devicePixelRatioF();
            const LONG px = static_cast<short>(LOWORD(msg->lParam));
            const LONG py = static_cast<short>(HIWORD(msg->lParam));
            const QPointF local = mapFromGlobal(QPointF(px / dpr, py / dpr));
            const int m = 6;
            const bool left = local.x() < m;
            const bool right = local.x() > width() - m;
            const bool top = local.y() < m;
            const bool bottom = local.y() > height() - m;
            const bool resizable = !isMaximized() && !isFullScreen();
            LRESULT hit = HTCLIENT;
            if (resizable && top && left)
                hit = HTTOPLEFT;
            else if (resizable && top && right)
                hit = HTTOPRIGHT;
            else if (resizable && bottom && left)
                hit = HTBOTTOMLEFT;
            else if (resizable && bottom && right)
                hit = HTBOTTOMRIGHT;
            else if (resizable && left)
                hit = HTLEFT;
            else if (resizable && right)
                hit = HTRIGHT;
            else if (resizable && top)
                hit = HTTOP;
            else if (resizable && bottom)
                hit = HTBOTTOM;
            else if (local.y() <= m_titleBar->height()) {
                // 标题栏拖动区；窗口控制按钮保持 HTCLIENT 以接收点击
                hit = HTCAPTION;
                const QPoint tb = local.toPoint() - m_titleBar->geometry().topLeft();
                for (QPushButton *b : {m_titleMin, m_titleMax, m_titleClose}) {
                    if (b && b->isVisible() && b->geometry().contains(tb)) {
                        hit = HTCLIENT;
                        break;
                    }
                }
            }
            *result = hit;
            return true;
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange && m_titleMax)
        m_titleMax->setText(isMaximized() ? QStringLiteral("❐") : QStringLiteral("□"));
    QMainWindow::changeEvent(event);
}

void MainWindow::setLog(const QString &text, bool isError)
{
    m_logLabel->setText(text);
    m_logLabel->setProperty("data-err", isError ? 1 : 0);
    m_logLabel->style()->unpolish(m_logLabel);
    m_logLabel->style()->polish(m_logLabel);
}

void MainWindow::setStatusChips()
{
    auto img = Engine::instance().imageStatus();
    auto eff = Engine::instance().effectStatus();
    m_imageChip->setText(Engine::statusText(img));
    m_effectChip->setText(Engine::statusText(eff));
    m_imageChip->setProperty("data-ok", img.ours ? 1 : 0);
    m_imageChip->setProperty("data-warn", img.dangling ? 1 : 0);
    m_effectChip->setProperty("data-ok", eff.ours ? 1 : 0);
    m_effectChip->setProperty("data-warn", eff.dangling ? 1 : 0);
    m_imageChip->style()->unpolish(m_imageChip);
    m_imageChip->style()->polish(m_imageChip);
    m_effectChip->style()->unpolish(m_effectChip);
    m_effectChip->style()->polish(m_effectChip);

    const bool admin = Engine::isElevated();
    m_adminLabel->setText(admin ? QStringLiteral("● 管理员权限")
                                : QStringLiteral("● 非管理员"));
    m_adminLabel->setProperty("data-admin", admin ? 1 : 0);
    m_adminLabel->style()->unpolish(m_adminLabel);
    m_adminLabel->style()->polish(m_adminLabel);
    m_osLabel->setText(Engine::windowsProductName());
}

void MainWindow::refreshStatus()
{
    setStatusChips();
}

void MainWindow::applyTheme(int mode)
{
    m_themeMode = mode;
    bool dark = (mode == 2);
    if (mode == 0)
        dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    const QString path = dark ? QStringLiteral(":/style.qss") : QStringLiteral(":/light.qss");
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        qobject_cast<QApplication *>(QApplication::instance())->setStyleSheet(
            QString::fromUtf8(f.readAll()));
    if (m_darkTheme != dark) {
        m_darkTheme = dark;
        updateImagePreview(); // re-render the mock in the matching scheme
    }
}

namespace {

// Everything here has to stay in step with the popup rules in style.qss/light.qss:
constexpr int kItemPadV = 8;      // ::item padding, top and bottom
constexpr int kItemPadH = 12;     // ::item padding, left and right
constexpr int kItemGapV = 2;      // ::item margin, top and bottom
constexpr int kItemGapH = 3;      // ::item margin, left and right
constexpr int kPopupRadius = 12;  // #ComboContainer border-radius
constexpr int kPopupGutter = 6;   // space between the panel edge and the first row

// A combo popup is a menu-style list, and the delegate Qt gives it sizes a row from the
// font alone: it never sees the padding and margin the stylesheet puts on ::item, so the
// popup lands a row or two short and answers by bringing up a scrollbar. This delegate
// reports the box the stylesheet actually paints, which is what lets Qt size the popup
// around the whole list on its own.
class ComboItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        QFont font = option.font;
        if (const auto *view = qobject_cast<const QWidget *>(parent()))
            font.resolve(view->font());
        const QFontMetrics fm(font);
        hint.setHeight(fm.height() + 2 * (kItemPadV + kItemGapV));
        hint.setWidth(qMax(hint.width(),
                           fm.horizontalAdvance(index.data(Qt::DisplayRole).toString())
                               + 2 * (kItemPadH + kItemGapH)));
        return hint;
    }
};

// Qt shows the popup in a top-level QFrame of its own. That frame is square, and it is
// all that sits behind the item view: leave it unpainted and the backing store comes
// through as a black plate, round only the view and the two borders stack up. So the
// frame takes the rounded panel from the stylesheet and this helper clips its corners.
class ComboPopupShape : public QObject
{
public:
    explicit ComboPopupShape(QComboBox *combo)
        : QObject(combo), m_combo(combo)
    {
        if (QAbstractItemView *view = combo->view()) {
            view->setItemDelegate(new ComboItemDelegate(view));
            view->installEventFilter(this);
        }
        combo->installEventFilter(this);
        styleFrame();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::ChildAdded:
            // The popup frame is built lazily; retry once the child object is complete.
            if (watched == m_combo)
                QMetaObject::invokeMethod(this, [this] { styleFrame(); }, Qt::QueuedConnection);
            break;
        case QEvent::Show:
            styleFrame();
            clipCorners();
            break;
        case QEvent::Resize:
            if (watched == m_frame)
                clipCorners();
            break;
        default:
            break;
        }
        return false;
    }

private:
    void styleFrame()
    {
        QAbstractItemView *view = m_combo ? m_combo->view() : nullptr;
        QWidget *frame = view ? view->parentWidget() : nullptr;
        if (!frame || frame == m_frame)
            return;
        m_frame = frame;
        frame->setObjectName(QStringLiteral("ComboContainer"));
        frame->setAttribute(Qt::WA_StyledBackground, true);
        if (auto *box = qobject_cast<QFrame *>(frame)) {
            box->setFrameShape(QFrame::NoFrame);
            box->setFrameShadow(QFrame::Plain);
        }
        // The panel deliberately keeps no layout margins of its own: Qt sizes the popup
        // from the row hints plus the border and pays no attention to margins set here,
        // so anything added at this level only eats the rows. The space around the list
        // comes from the ::item margin instead, which the hints do account for.
        // The name lands after the frame was first polished, so the style has to be
        // told to look the rules up again or the panel keeps its palette grey.
        frame->style()->unpolish(frame);
        frame->style()->polish(frame);
        frame->update();
        frame->installEventFilter(this);
        clipCorners();
    }

    void clipCorners()
    {
        QWidget *frame = m_frame;
        if (!frame || frame->width() <= 2 * kPopupRadius || frame->height() <= 2 * kPopupRadius)
            return;
        QPainterPath path;
        path.addRoundedRect(frame->rect(), kPopupRadius, kPopupRadius);
        frame->setMask(QRegion(path.toFillPolygon().toPolygon()));
    }

    QComboBox *m_combo = nullptr;
    QPointer<QWidget> m_frame;
};

} // namespace

// TEMPORARY diagnostic hook, not part of a normal run: FBS_COMBO_DEBUG points at a
// directory, and this opens every combo box in the window once per theme, writes down
// the geometry Qt settled on, saves a picture of the popup, and then quits.
void runComboSelfTest(QWidget *window)
{
    const QString dirPath =
        QString::fromLocal8Bit(qEnvironmentVariable("FBS_COMBO_DEBUG").toUtf8());
    QDir().mkpath(dirPath);
    const QList<QComboBox *> boxes = window->findChildren<QComboBox *>();

    auto record = [dirPath](const QString &line) {
        QFile file(dirPath + QStringLiteral("/combo-debug.txt"));
        if (file.open(QIODevice::Append | QIODevice::Text))
            QTextStream(&file) << line << '\n';
    };

    auto applyQss = [record](bool dark) {
        QFile f(dark ? QStringLiteral(":/style.qss") : QStringLiteral(":/light.qss"));
        const QByteArray data =
            f.open(QIODevice::ReadOnly | QIODevice::Text) ? f.readAll() : QByteArray();
        if (auto *app = qobject_cast<QApplication *>(QApplication::instance())) {
            app->setStyleSheet(QString::fromUtf8(data));
            record(QStringLiteral("\n=== theme %1 === open=%2 read=%3 appCss=%4")
                       .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                       .arg(f.error() == QFileDevice::NoError)
                       .arg(data.size())
                       .arg(app->styleSheet().size()));
        }
    };

    auto step = std::make_shared<std::function<void(int, int)>>();
    *step = [boxes, record, applyQss, dirPath, step, window](int theme, int i) {
        if (i >= boxes.size()) {
            if (theme == 0)
                (*step)(1, 0);
            else
                QApplication::quit();
            return;
        }
        if (i == 0)
            applyQss(theme == 1);

        QComboBox *combo = boxes.at(i);
        // Half of these live on a page of the stacked settings view, so bring that page
        // forward first; a hidden combo box refuses to open its popup.
        for (QWidget *w = combo; w && w != combo->window(); w = w->parentWidget())
            if (auto *stack = qobject_cast<QStackedWidget *>(w->parentWidget()))
                stack->setCurrentWidget(w);

        // Probe matrix: the popup frame only exists after the first show, so open and
        // close once to get it built, then hang a different selector form on each of
        // the first five combos and see which one the painter actually obeys.
        combo->showPopup();
        combo->hidePopup();
        QAbstractItemView *probeView = combo->view();
        QWidget *probeFrame = probeView ? probeView->window() : nullptr;
        if (probeView)
            probeView->setStyleSheet(QString());
        if (probeFrame)
            probeFrame->setStyleSheet(QString());
        switch (i) {
        case 0:
            break; // baseline: the app-level #ComboPopup rules as they stand
        case 1: // local rule on the view, bare class selector
            probeView->setStyleSheet(QStringLiteral(
                "QAbstractItemView::item { background:#ffffff; color:#000000;"
                " padding:8px 12px; margin:2px 3px; }"
                "QAbstractItemView::item:selected { background:#00ff00; color:#000000; }"));
            break;
        case 2: // local rule on the view, id selector
            probeView->setStyleSheet(QStringLiteral(
                "#ComboPopup::item { background:#ffffff; color:#000000;"
                " padding:8px 12px; margin:2px 3px; }"
                "#ComboPopup::item:selected { background:#00ff00; color:#000000; }"));
            break;
        case 3: // local rule on the view, wildcard
            probeView->setStyleSheet(QStringLiteral("QWidget { background:#ffd0d0; }"));
            break;
        case 4: // local rule on the popup frame
            if (probeFrame)
                probeFrame->setStyleSheet(QStringLiteral(
                    "QFrame { background:#ffe0a0; border:2px solid #ff0000;"
                    " border-radius:12px; }"));
            break;
        default:
            break;
        }
        combo->showPopup();
        QTimer::singleShot(300, combo, [combo, record, dirPath, theme, i, step, window] {
            QAbstractItemView *view = combo->view();
            QWidget *frame = view ? view->window() : nullptr;
            auto *scroll = qobject_cast<QAbstractScrollArea *>(view);
            if (!view || !frame || !scroll || !scroll->viewport()) {
                record(QStringLiteral("[%1] %2: no popup").arg(i).arg(combo->objectName()));
                (*step)(theme, i + 1);
                return;
            }
            // Which popup does Qt really open: the QListView, or a QMenu behind our back?
            QString tops;
            const QList<QWidget *> wids = QApplication::allWidgets();
            for (QWidget *w : wids) {
                if (!w->isVisible() || !w->isWindow() || w == window)
                    continue;
                tops += QStringLiteral(" [%1 obj=%2 %3x%4]")
                            .arg(QString::fromLatin1(w->metaObject()->className()))
                            .arg(w->objectName())
                            .arg(w->width())
                            .arg(w->height());
            }
            record(QStringLiteral("[%1] hint SH_ComboBox_Popup=%2 viewClass=%3 viewObj=%4 "
                                  "viewVisible=%5 frameClass=%6 frameObj=%7 styleClass=%8 "
                                  "appCssBytes=%9 popupTops=%10")
                       .arg(i)
                       .arg(combo->style()->styleHint(QStyle::SH_ComboBox_Popup, nullptr, combo))
                       .arg(QString::fromLatin1(view->metaObject()->className()))
                       .arg(view->objectName())
                       .arg(view->isVisible())
                       .arg(QString::fromLatin1(frame->metaObject()->className()))
                       .arg(frame->objectName())
                       .arg(QString::fromLatin1(view->style()->metaObject()->className()))
                       .arg(qobject_cast<QApplication *>(QApplication::instance())
                                ? qobject_cast<QApplication *>(QApplication::instance())
                                      ->styleSheet().size()
                                : -1)
                       .arg(tops));
            QScrollBar *bar = scroll->verticalScrollBar();
            int content = 0;
            QString rows;
            for (int r = 0; r < combo->count(); ++r) {
                content += view->sizeHintForRow(r);
                const QRect vr = view->visualRect(combo->model()->index(r, 0));
                rows += QStringLiteral("    row%1 hint=%2 at %3,%4 %5x%6\n")
                            .arg(r)
                            .arg(view->sizeHintForRow(r))
                            .arg(vr.x())
                            .arg(vr.y())
                            .arg(vr.width())
                            .arg(vr.height());
            }
            const bool fits = bar->maximum() == 0 && content <= scroll->viewport()->height();
            record(QStringLiteral("[%1] %2 items=%3 %4 | frame=%5x%6 view=%7x%8 vp=%9x%10 "
                                 "chrome=%11 margins=%12,%13,%14,%15 delegate=%16 barMax=%17")
                       .arg(i)
                       .arg(combo->objectName().isEmpty() ? combo->metaObject()->className()
                                                          : combo->objectName())
                       .arg(combo->count())
                       .arg(fits ? QStringLiteral("FITS") : QStringLiteral("SCROLLS"))
                       .arg(frame->width())
                       .arg(frame->height())
                       .arg(view->width())
                       .arg(view->height())
                       .arg(scroll->viewport()->width())
                       .arg(scroll->viewport()->height())
                       .arg(frame->height() - scroll->viewport()->height())
                       .arg(view->contentsMargins().left())
                       .arg(view->contentsMargins().top())
                       .arg(view->contentsMargins().right())
                       .arg(view->contentsMargins().bottom())
                       .arg(view->itemDelegate() ? view->itemDelegate()->metaObject()->className()
                                                 : QStringLiteral("none"))
                       .arg(bar->maximum())
                   + rows);

            // Grab at the ratio the popup is really shown at, so the picture says what
            // the screen will instead of what a shrunken copy of it looks like.
            const qreal dpr = frame->devicePixelRatioF();
            QImage image(QSize(qRound(frame->width() * dpr), qRound(frame->height() * dpr)),
                         QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Qt::transparent);
            frame->render(&image);
            image.save(dirPath + QStringLiteral("/%1-%2.png")
                                   .arg(theme)
                                   .arg(i, 2, 10, QLatin1Char('0')));
            combo->hidePopup();
            QTimer::singleShot(120, combo, [combo, step, theme, i] {
                combo->setCurrentIndex(0);
                (*step)(theme, i + 1);
            });
        });
    };
    QTimer::singleShot(1200, window, [step] { (*step)(0, 0); });
}

void MainWindow::styleCombo(QComboBox *combo) const
{
    if (combo->property("fbsShape").isValid())
        return;
    combo->setProperty("fbsShape", true);
    // Qt caps the popup at this many rows and scrolls the rest, so keep the ceiling
    // well above any list this app builds; nothing here should ever need scrolling.
    combo->setMaxVisibleItems(32);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    QAbstractItemView *view = combo->view();
    view->setObjectName(QStringLiteral("ComboPopup"));
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setTextElideMode(Qt::ElideRight);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    if (auto *list = qobject_cast<QListView *>(view))
        list->setUniformItemSizes(true);

    new ComboPopupShape(combo);
}

Engine::EffectConfig MainWindow::currentEffectConfig() const
{
    Engine::EffectConfig cfg;
    cfg.effect = qBound(0, m_effectCombo->currentIndex(), 4);
    cfg.clearAddress = m_clearAddress->isChecked();
    cfg.clearBarBg = m_clearBarBg->isChecked();
    cfg.clearWinUIBg = m_clearWinUIBg->isChecked();
    cfg.showLine = m_showLine->isChecked();
    cfg.lightR = m_lightColor.red();
    cfg.lightG = m_lightColor.green();
    cfg.lightB = m_lightColor.blue();
    cfg.lightA = m_lightAlpha->value();
    cfg.darkR = m_darkColor.red();
    cfg.darkG = m_darkColor.green();
    cfg.darkB = m_darkColor.blue();
    cfg.darkA = m_darkAlpha->value();
    return cfg;
}

void MainWindow::loadSettings()
{
    QSettings s = appinfo::settings();
    m_themeCombo->setCurrentIndex(s.value(QStringLiteral("ui/theme"), 0).toInt());
    m_rotate->setValue(s.value(QStringLiteral("image/rotate"), 0).toInt());
    m_scale->setValue(s.value(QStringLiteral("image/scale"), 100).toInt());
    m_brightness->setValue(s.value(QStringLiteral("image/brightness"), 100).toInt());
    m_contrast->setValue(s.value(QStringLiteral("image/contrast"), 100).toInt());
    m_blur->setValue(s.value(QStringLiteral("image/blur"), 0).toInt());
    m_opacity->setValue(s.value(QStringLiteral("image/opacity"), 255).toInt());
    m_posMode = qBound(0, s.value(QStringLiteral("image/posType"), 6).toInt(), 6);
    setPosMode(m_posMode);
    m_folderExt->setChecked(s.value(QStringLiteral("image/folderExt"), false).toBool());
    m_comboEffect->setChecked(s.value(QStringLiteral("image/comboEffect"), true).toBool());
    m_keepImage->setChecked(s.value(QStringLiteral("effect/keepImage"), false).toBool());
    // 图片浏览目录：软件所在目录/media/image，每次启动默认打开(不存在则创建)
    m_presetDir = QCoreApplication::applicationDirPath() + QStringLiteral("/media/image");
    QDir().mkpath(m_presetDir);
    rebuildGallery();
    m_selectedPreset = s.value(QStringLiteral("image/preset"), 0).toInt();
    m_customImage = s.value(QStringLiteral("image/customPath")).toString();
    if (m_selectedPreset >= 0 && m_selectedPreset < m_presets.size()) {
        selectPreset(m_selectedPreset);
    } else if (!m_customImage.isEmpty() && QFileInfo::exists(m_customImage)) {
        for (auto *b : m_presetButtons)
            b->setChecked(false);
        setImageSourceText(QStringLiteral("当前选择：自定义图片 · %1")
                               .arg(QFileInfo(m_customImage).fileName()));
        updateImagePreview();
    } else {
        selectPreset(0);
    }
    // effect custom values
    m_effectCombo->setCurrentIndex(s.value(QStringLiteral("effect/type"), 1).toInt());
    m_lightColor = QColor(s.value(QStringLiteral("effect/lightColor"), QStringLiteral("#ffffff")).toString());
    m_darkColor = QColor(s.value(QStringLiteral("effect/darkColor"), QStringLiteral("#000000")).toString());
    m_lightAlpha->setValue(s.value(QStringLiteral("effect/lightAlpha"), 200).toInt());
    m_darkAlpha->setValue(s.value(QStringLiteral("effect/darkAlpha"), 120).toInt());
    m_clearAddress->setChecked(s.value(QStringLiteral("effect/clearAddress"), true).toBool());
    m_clearBarBg->setChecked(s.value(QStringLiteral("effect/clearBarBg"), true).toBool());
    m_clearWinUIBg->setChecked(s.value(QStringLiteral("effect/clearWinUIBg"), true).toBool());
    m_showLine->setChecked(s.value(QStringLiteral("effect/showLine"), false).toBool());
    m_lightColorBtn->setStyleSheet(
        QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
            .arg(m_lightColor.name()));
    m_darkColorBtn->setStyleSheet(
        QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
            .arg(m_darkColor.name()));
    // video wallpaper settings
    const QStringList playlist = s.value(QStringLiteral("video/playlist")).toStringList();
    VideoWallpaper::instance().setPlaylist(playlist);
    refreshVideoList();
    m_videoVolume->setValue(s.value(QStringLiteral("video/volume"), 0).toInt());
    m_autoLoopBox->setChecked(s.value(QStringLiteral("video/autoLoop"), true).toBool());
    m_randomBox->setChecked(s.value(QStringLiteral("video/random"), false).toBool());
    m_fullscreenPauseBox->setChecked(s.value(QStringLiteral("video/pauseFullscreen"), true).toBool());
    m_batteryBox->setChecked(s.value(QStringLiteral("video/pauseBattery"), false).toBool());
    {
        const int targetFps = s.value(QStringLiteral("video/targetFps"), 24).toInt();
        static const int fpsValues[] = {0, 15, 24, 30, 60};
        for (int i = 0; i < 5; ++i)
            if (fpsValues[i] == targetFps) {
                m_fpsBox->setCurrentIndex(i);
                break;
            }
        VideoWallpaper::instance().setTargetFps(targetFps);
    }
    m_reclaimBox->setChecked(s.value(QStringLiteral("video/reclaim"), true).toBool());
    m_affinityBox->setChecked(s.value(QStringLiteral("video/affinityLimit"), true).toBool());
    m_screenModeCombo->setCurrentIndex(s.value(QStringLiteral("video/screenMode"), 0).toInt());
    VideoWallpaper::instance().setAutoLoop(m_autoLoopBox->isChecked());
    VideoWallpaper::instance().setRandom(m_randomBox->isChecked());
    VideoWallpaper::instance().setPauseOnFullscreen(m_fullscreenPauseBox->isChecked());
    VideoWallpaper::instance().setReclaimMemory(m_reclaimBox->isChecked());
    VideoWallpaper::instance().setScreenMode(m_screenModeCombo->currentIndex());
    VideoWallpaper::instance().setVolume(m_videoVolume->value());
}

void MainWindow::saveImageSettings()
{
    QSettings s = appinfo::settings();
    s.setValue(QStringLiteral("image/rotate"), m_rotate->value());
    s.setValue(QStringLiteral("image/scale"), m_scale->value());
    s.setValue(QStringLiteral("image/brightness"), m_brightness->value());
    s.setValue(QStringLiteral("image/contrast"), m_contrast->value());
    s.setValue(QStringLiteral("image/blur"), m_blur->value());
    s.setValue(QStringLiteral("image/opacity"), m_opacity->value());
    s.setValue(QStringLiteral("image/posType"), m_posMode);
    s.setValue(QStringLiteral("image/folderExt"), m_folderExt->isChecked());
    s.setValue(QStringLiteral("image/comboEffect"), m_comboEffect->isChecked());
    s.setValue(QStringLiteral("image/preset"), m_selectedPreset);
    s.setValue(QStringLiteral("image/customPath"), m_customImage);
}

void MainWindow::saveEffectSettings()
{
    QSettings s = appinfo::settings();
    s.setValue(QStringLiteral("effect/type"), m_effectCombo->currentIndex());
    s.setValue(QStringLiteral("effect/lightColor"), m_lightColor.name());
    s.setValue(QStringLiteral("effect/darkColor"), m_darkColor.name());
    s.setValue(QStringLiteral("effect/lightAlpha"), m_lightAlpha->value());
    s.setValue(QStringLiteral("effect/darkAlpha"), m_darkAlpha->value());
    s.setValue(QStringLiteral("effect/clearAddress"), m_clearAddress->isChecked());
    s.setValue(QStringLiteral("effect/clearBarBg"), m_clearBarBg->isChecked());
    s.setValue(QStringLiteral("effect/clearWinUIBg"), m_clearWinUIBg->isChecked());
    s.setValue(QStringLiteral("effect/showLine"), m_showLine->isChecked());
    s.setValue(QStringLiteral("effect/keepImage"), m_keepImage->isChecked());
}

void MainWindow::applyImage()
{
    QString path;
    if (m_selectedPreset >= 0 && m_selectedPreset < m_presets.size())
        path = m_presets[m_selectedPreset].res;
    else if (!m_customImage.isEmpty())
        path = m_customImage;
    if (path.isEmpty()) {
        setLog(QStringLiteral("请先在图片浏览中选择图片，或使用“选择图片…”"), true);
        return;
    }

    m_applyImageBtn->setEnabled(false);
    setLog(QStringLiteral("正在处理图片…"), false);
    QCoreApplication::processEvents();

    QString err;
    Engine::instance().ensureDataDirs();
    QImage src(path);
    if (src.isNull()) {
        setLog(QStringLiteral("图片读取失败"), true);
        m_applyImageBtn->setEnabled(true);
        return;
    }
    QImage processed = ImageProcess::adjust(src, m_brightness->value() / 100.0,
                                            m_contrast->value() / 100.0, m_blur->value());
    processed = ImageProcess::rotateAroundY(processed, m_rotate->value());
    // 尺寸调节：DLL 没有缩放参数，通过缩放实际写入的图片改变其在
    // 资源管理器中的相对大小(仅原尺寸模式；填充/拉伸始终铺满窗口)。
    const double pct = m_scale->value() / 100.0;
    const int uiPos = qBound(0, m_posMode, 6);
    if (qAbs(pct - 1.0) > 1e-3 && uiPos != 0 && uiPos != 2) {
        const QSize target(qRound(processed.width() * pct), qRound(processed.height() * pct));
        processed = processed.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (!processed.save(Engine::processedImagePath(), "PNG")) {
        setLog(QStringLiteral("处理后的图片保存失败"), true);
        m_applyImageBtn->setEnabled(true);
        return;
    }

    setLog(QStringLiteral("正在写入配置…"), false);
    QCoreApplication::processEvents();

    // posType: 0..3 corners, 4 center, 5 stretch, 6 zoom&fill
    static const int posMap[] = {6, 4, 5, 0, 1, 2, 3};
    int posType = posMap[qBound(0, m_posMode, 6)];

    if (!Engine::instance().writeImageConfig(Engine::processedImagePath(), posType,
                                             m_opacity->value(), m_folderExt->isChecked(), &err)) {
        setLog(err, true);
        m_applyImageBtn->setEnabled(true);
        return;
    }
    setLog(QStringLiteral("正在注册 DLL(需要管理员权限)…"), false);
    QCoreApplication::processEvents();

    saveImageSettings();
    if (!m_comboEffect->isChecked())
        Engine::instance().unregisterEffectDll(nullptr); // exclusive mode
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
    if (m_comboEffect->isChecked()) {
        // Layer the whole-window blur/mica backdrop under the image.
        if (!Engine::instance().writeEffectConfig(currentEffectConfig(), &err)
            || !Engine::instance().registerEffectDll(&err)) {
            setLog(QStringLiteral("图片背景已注册，但叠加效果失败：%1").arg(err), true);
        }
    }

    setLog(QStringLiteral("正在重启资源管理器…"), false);
    QCoreApplication::processEvents();
    Engine::restartExplorer(nullptr);
    refreshStatus();
    setLog(QStringLiteral("图片背景已应用！打开任意文件夹即可查看效果。"), false);
    m_applyImageBtn->setEnabled(true);
}

void MainWindow::applyEffect()
{
    Engine::EffectConfig cfg = currentEffectConfig();

    m_applyEffectBtn->setEnabled(false);
    setLog(QStringLiteral("正在写入配置…"), false);
    QCoreApplication::processEvents();

    QString err;
    if (!Engine::instance().writeEffectConfig(cfg, &err)) {
        setLog(err, true);
        m_applyEffectBtn->setEnabled(true);
        return;
    }
    setLog(QStringLiteral("正在注册 DLL(需要管理员权限)…"), false);
    QCoreApplication::processEvents();

    saveEffectSettings();
    if (!m_keepImage->isChecked())
        Engine::instance().unregisterImageDll(nullptr); // exclusive mode
    if (!Engine::instance().registerEffectDll(&err)) {
        setLog(err, true);
        m_applyEffectBtn->setEnabled(true);
        return;
    }
    if (m_keepImage->isChecked()) {
        // Re-apply the image hook with the last used picture and parameters.
        const QString img = Engine::processedImagePath();
        if (QFileInfo::exists(img)) {
            static const int posMap[] = {6, 4, 5, 0, 1, 2, 3};
            QSettings s = appinfo::settings();
            if (!Engine::instance().writeImageConfig(
                    img, posMap[qBound(0, s.value(QStringLiteral("image/posType"), 0).toInt(), 6)],
                    s.value(QStringLiteral("image/opacity"), 255).toInt(),
                    s.value(QStringLiteral("image/folderExt"), false).toBool(), &err)
                || !Engine::instance().registerImageDll(&err)) {
                setLog(QStringLiteral("效果已注册，但叠加图片失败：%1").arg(err), true);
            }
        }
    }

    setLog(QStringLiteral("正在重启资源管理器…"), false);
    QCoreApplication::processEvents();
    Engine::restartExplorer(nullptr);
    refreshStatus();
    setLog(QStringLiteral("效果样式已应用！打开任意文件夹即可查看效果。"), false);
    m_applyEffectBtn->setEnabled(true);
}

void MainWindow::addVideos()
{
    // 默认打开软件目录下的 media/video(不存在则创建)
    const QString videoDir = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/media/video");
    QDir().mkpath(videoDir);
    QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("添加视频到播放列表"), videoDir,
        QStringLiteral("视频文件 (*.mp4 *.webm *.mkv *.avi *.mov *.wmv);;所有文件 (*)"));
    if (files.isEmpty())
        return;
    QStringList list = VideoWallpaper::instance().playlist();
    for (const QString &f : files)
        if (!list.contains(f))
            list.append(f);
    VideoWallpaper::instance().setPlaylist(list);
    QSettings st = appinfo::settings();
    st.setValue(QStringLiteral("video/playlist"), list);
    refreshVideoList();
}

// 扫描软件目录 media/video(含子目录)下的视频文件，去重后并入播放列表。
// 只增不删：不影响现有条目与正在播放的曲目(setPlaylist 按文件名保持当前曲)。
void MainWindow::scanVideoDir()
{
    const QString videoDir = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/media/video");
    const QStringList nameFilters = {
        QStringLiteral("*.mp4"),  QStringLiteral("*.webm"), QStringLiteral("*.mkv"),
        QStringLiteral("*.avi"),  QStringLiteral("*.mov"),  QStringLiteral("*.wmv")};
    QStringList found;
    QDirIterator it(videoDir, nameFilters, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        found << it.next();

    QStringList list = VideoWallpaper::instance().playlist();
    int added = 0;
    for (const QString &f : found) {
        if (!list.contains(f)) {
            list.append(f);
            ++added;
        }
    }
    if (added == 0) {
        setLog(QStringLiteral("扫描完成：未发现新视频"), false);
        return;
    }
    VideoWallpaper::instance().setPlaylist(list);
    QSettings st = appinfo::settings();
    st.setValue(QStringLiteral("video/playlist"), list);
    refreshVideoList();
    setLog(QStringLiteral("扫描完成：新增 %1 个视频(共 %2 个)")
               .arg(added).arg(list.size()), false);
}

void MainWindow::removeSelectedVideos()
{
    if (!m_videoList)
        return;
    const QList<QListWidgetItem *> selected = m_videoList->selectedItems();
    if (selected.isEmpty())
        return;
    // 就地删除选中行(不重建列表)：删除后把选中迁移到同位置条目(非末项)或新的
    // 末项——滚动位置、选中高亮都保持，支持连点删除。此前 clear+重建 的方案会让
    // 滚动归顶、选中丢失，正是要避免的。
    int firstRow = m_videoList->count();
    QStringList list = VideoWallpaper::instance().playlist();
    for (QListWidgetItem *item : selected) {
        firstRow = qMin(firstRow, m_videoList->row(item));
        list.removeAll(item->data(Qt::UserRole).toString());
    }
    m_videoList->clearSelection();
    m_videoList->setCurrentRow(-1);
    for (QListWidgetItem *item : selected) {
        const int row = m_videoList->row(item);
        delete m_videoList->takeItem(row);
    }
    VideoWallpaper::instance().setPlaylist(list);
    QSettings st = appinfo::settings();
    st.setValue(QStringLiteral("video/playlist"), list);
    // 选中迁移到同位置(删的是末项则为新的末项)；scrollToItem 对已可见行不动滚动
    const int target = qMin(firstRow, m_videoList->count() - 1);
    if (target >= 0) {
        m_videoList->setCurrentRow(target, QItemSelectionModel::SelectCurrent);
        m_videoList->scrollToItem(m_videoList->item(target),
                                  QAbstractItemView::EnsureVisible);
    }
    // 就地删除不重建列表：计数标签需要单独刷新
    if (m_videoStatus)
        m_videoStatus->setText(QStringLiteral("共 %1 个视频 · %2")
                                   .arg(list.size())
                                   .arg(VideoWallpaper::instance().isPlaying()
                                            ? QStringLiteral("播放中") : QStringLiteral("停止")));
    updateVideoButtons();
    updatePlayingHighlight();
}

void MainWindow::clearVideos()
{
    VideoWallpaper::instance().clearPlaylist();
    QSettings st = appinfo::settings();
    st.remove(QStringLiteral("video/playlist"));
    refreshVideoList();
    setLog(QStringLiteral("已清空视频播放列表。"), false);
}

void MainWindow::startVideo()
{
    QString err;
    // 列表中选中了条目时，从选中项开始播放壁纸；未选中则沿用上次进度
    const int selected = m_videoList ? m_videoList->currentRow() : -1;
    if (!VideoWallpaper::instance().startPlaying(&err, selected)) {
        setLog(err.isEmpty() ? QStringLiteral("视频壁纸启动失败") : err, true);
        return;
    }
    QSettings st = appinfo::settings();
    st.setValue(QStringLiteral("video/wasPlaying"), true);
    setLog(QStringLiteral("视频壁纸运行中：画面在桌面图标之后，保持程序运行即可。"), false);
}

void MainWindow::stopVideo()
{
    VideoWallpaper::instance().stopAll();
    QSettings st = appinfo::settings();
    st.setValue(QStringLiteral("video/wasPlaying"), false);
    refreshVideoList();
    setLog(QStringLiteral("视频壁纸已取消。"), false);
}

void MainWindow::refreshVideoList()
{
    if (!m_videoList)
        return;
    m_videoList->clear();
    for (const QString &f : VideoWallpaper::instance().playlist()) {
        auto *item = new QListWidgetItem(m_videoList);
        item->setData(Qt::UserRole, f);
        QFileInfo fi(f);
        auto *box = new QWidget;
        box->setProperty("data-playing", false); // 播放中的条目高亮(QSS 按 property 着色)
        box->setProperty("data-path", f);
        auto *boxLay = new QVBoxLayout(box);
        boxLay->setContentsMargins(8, 5, 8, 5);
        boxLay->setSpacing(1);
        auto *name = new QLabel(fi.fileName(), box);
        QFont nf = name->font();
        nf.setBold(true);
        name->setFont(nf);
        auto *path = new QLabel(QDir::toNativeSeparators(f), box);
        path->setObjectName(QStringLiteral("VideoItemPath"));
        path->setWordWrap(false);
        boxLay->addWidget(name);
        boxLay->addWidget(path);
        item->setSizeHint(QSize(0, 52));
        m_videoList->setItemWidget(item, box);
    }
    if (m_videoStatus)
        m_videoStatus->setText(QStringLiteral("共 %1 个视频 · %2")
                                   .arg(VideoWallpaper::instance().playlist().size())
                                   .arg(VideoWallpaper::instance().isPlaying()
                                            ? QStringLiteral("播放中") : QStringLiteral("停止")));
    updateVideoButtons();
    updatePlayingHighlight();
}

// 正在播放(含暂停/自动挂起，恢复时仍是这一曲)的条目以底色高亮，便于辨别当前曲目
void MainWindow::updatePlayingHighlight()
{
    if (!m_videoList)
        return;
    const auto &vp = VideoWallpaper::instance();
    const QStringList &pl = vp.playlist();
    const QString current = (vp.currentIndex() >= 0 && vp.currentIndex() < pl.size())
                                ? pl.at(vp.currentIndex())
                                : QString();
    const bool highlight = vp.isStarted() && !current.isEmpty();
    for (int i = 0; i < m_videoList->count(); ++i) {
        QWidget *w = m_videoList->itemWidget(m_videoList->item(i));
        if (!w)
            continue;
        const bool playing = highlight && w->property("data-path").toString() == current;
        if (w->property("data-playing") != playing) {
            w->setProperty("data-playing", playing);
            w->style()->unpolish(w);
            w->style()->polish(w);
            w->update();
        }
    }
}

void MainWindow::onVideoStateChanged(const QString &text)
{
    if (m_videoStatus)
        m_videoStatus->setText(text);
    updateVideoButtons();
    updatePlayingHighlight();
}

void MainWindow::updateVideoButtons()
{
    if (!m_playBtn || !m_stopBtn)
        return;
    const bool empty = VideoWallpaper::instance().playlist().isEmpty();
    const bool started = VideoWallpaper::instance().isStarted();

    m_playBtn->setEnabled(!empty && !started); // 运行中由“暂停/继续”与“取消”接管
    m_stopBtn->setEnabled(started);
    if (m_pauseBtn) {
        m_pauseBtn->setEnabled(started);
        m_pauseBtn->setText(VideoWallpaper::instance().isManualPaused()
                                ? QStringLiteral("继续") : QStringLiteral("暂停"));
    }
}

void MainWindow::uninstallAll()
{
    setLog(QStringLiteral("正在卸载背景组件…"), false);
    QCoreApplication::processEvents();
    QString err;
    Engine::instance().unregisterImageDll(&err);
    Engine::instance().unregisterEffectDll(&err);
    Engine::restartExplorer(nullptr);
    refreshStatus();
    setLog(QStringLiteral("已恢复系统默认背景。"), false);
}

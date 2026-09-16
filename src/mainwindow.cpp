#include "mainwindow.h"

#include "appinfo.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/CachePaths.h"
#include "videodiag.h"
#include "videowallpaper.h"
#include "tooltipstyle.h"
#include "app/ApplicationRuntimeState.h"
#include "app/ApplicationShutdown.h"
#include "kanban/KanbanController.h"
#include "kanban/KanbanModelManager.h"
#include "tray/SystemTrayController.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QHideEvent>
#include <QFile>
#include <QFileDialog>
#include <QDirIterator>
#include <QMessageBox>
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
#include <QButtonGroup>
#include <QProcess>
#include <QStandardPaths>
#include <QRadioButton>
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
#include "platform/windows/desktopmount.h"

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
    setMinimumSize(880, 660);
    // 初始窗口几何：优先恢复上次保存的尺寸位置(钳制在可用工作区 94% 内；越界
    // 自动拉回主屏)，无保存记录时用自适应首选 960×840(贴近用户实测合适尺寸，
    // 且高于最小值 880×660 不会裁掉底部按钮行)。
    {
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
        auto &cfg = AppConfig::instance();
        const int cw = cfg.value(ConfigKeys::Window::Width, 0).toInt();
        const int ch = cfg.value(ConfigKeys::Window::Height, 0).toInt();
        if (cw >= 880 && ch >= 660) {
            QSize want(cw, ch);
            want = want.boundedTo(avail.size() * 0.94);
            resize(want);
            m_savedWindowSize = want;
            const int x = cfg.value(ConfigKeys::Window::X, avail.left()).toInt();
            const int y = cfg.value(ConfigKeys::Window::Y, avail.top()).toInt();
            move(qBound(avail.left(), x, qMax(avail.left(), avail.right() - width())),
                 qBound(avail.top(), y, qMax(avail.top(), avail.bottom() - height())));
            if (cfg.value(ConfigKeys::Window::Maximized, false).toBool())
                setWindowState(Qt::WindowMaximized);
        } else {
            m_savedWindowSize = QSize(960, 840).boundedTo(avail.size() * 0.94);
            resize(m_savedWindowSize);
        }
    }
    // 标题栏自绘方案：保留 WS_THICKFRAME(圆角/阴影/贴边由 DWM 提供)，
    // 通过 WM_NCCALCSIZE 隐藏系统标题栏，WM_NCHITTEST 实现边缘缩放与标题拖动。

    // 立刻恢复上次的视频壁纸：媒体打开+解码器初始化约需 1.5-2s，必须赶在
    // 界面构建(图片库缩略图等)之前起跑，否则壁纸要多等近一秒才出现。
    // 失败提示延迟到事件循环启动(日志控件就绪)后再补发。
    {
        AppConfig &early = AppConfig::instance();
        const QStringList earlyPlaylist =
            early.value(ConfigKeys::Video::Playlist).toStringList();
        const bool earlyWasPlaying =
            early.value(ConfigKeys::Video::WasPlaying, false).toBool();
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
        videodiag::stage(earlyWasPlaying && !earlyPlaylist.isEmpty()
                             ? QStringLiteral("壁纸状态恢复已发起(视频管线后台起跑)")
                             : QStringLiteral("壁纸状态恢复跳过(上次未播放)"));
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

    // Top page 3: 看板娘
    m_topStack->addWidget(buildKanbanPage());

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
    videodiag::stage(QStringLiteral("主窗口控件树构建完成(UI 初始化)"));
    loadSettings();
    videodiag::stage(QStringLiteral("设置回填完成(含图库与显示器枚举)"));
    applyTheme(m_themeCombo->currentIndex());
    refreshStatus();
    // the legacy import ran before this window existed; report what it carried over
    if (!appinfo::migrationNotes().isEmpty())
        setLog(appinfo::migrationNotes().join(QStringLiteral(" ")), false);
    // 装在只读目录(如 C:\Program Files)时 .cache 建不起来：明确提示用户，
    // 不静默回退到 AppData，也不改任何非缓存数据的位置。
    QString cacheError;
    if (!CachePaths::isWritable(&cacheError))
        setLog(cacheError, true); // 诊断日志也写在 .cache 内，此时只能走界面日志
    reportDllMigration();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_themeMode == 0)
            applyTheme(0);
    });

    // 上次的视频壁纸已在构造函数开头恢复(抢先于界面构建)；loadSettings 会把
    // 音量/帧率/多屏等设置应用到已存在的播放管线。

    // 看板娘与托盘放在最后装配：控件树、设置回填、视频壁纸恢复都已就位。
    setupKanbanAndTray();

    // TEMPORARY diagnostic build hook: FBS_COMBO_DEBUG=<dir> walks every combo box,
    // opens its popup and writes metrics plus a rendered PNG into that directory.
    if (qEnvironmentVariableIsSet("FBS_COMBO_DEBUG"))
        runComboSelfTest(this);
    videodiag::logObjectEvent("create", this,
                              QStringLiteral("size=%1x%2").arg(width()).arg(height()));
}

// 退出闸门(任务书 6.3)：关闭缩略图后台任务的回调通路。锁内翻标志即可返回，
// 不等待任务跑完(退出时卡住比多一次无效投递更糟)；已经通过检查的任务在投递时
// 对象必然仍然存活，其队列回调由 ~QObject 丢弃。
MainWindow::~MainWindow()
{
    QMutexLocker guard(&m_thumbTasksMutex);
    m_thumbTasksLive = false;
    videodiag::logObjectEvent("destroy", this);
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
    m_nav->addItem(QStringLiteral("🎎  看板娘"));
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
        AppConfig &st = AppConfig::instance();
        st.setValue(ConfigKeys::Ui::Theme, idx);
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
            scheduleImagePreview(); // 拖动过程中合并重绘，松手后再按最终尺寸画一次
        } else if (m_imageSourceLabel && obj == m_imageSourceLabel) {
            setImageSourceText(m_sourceText); // 宽度变化后重新按两行省略
        } else if (m_previewFrame && obj == m_previewFrame) {
            updatePreviewAspect(); // 宽度变了就按桌面比例重算预览框高度
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// 预览重绘很贵(读图+平滑缩放+模拟窗口)，resize 风暴里只在停手后补一次。
void MainWindow::scheduleImagePreview()
{
    if (!m_previewDebounce) {
        m_previewDebounce = new QTimer(this);
        m_previewDebounce->setSingleShot(true);
        connect(m_previewDebounce, &QTimer::timeout, this, &MainWindow::updateImagePreview);
    }
    m_previewDebounce->start(60);
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
    // 列宽由视口宽度反算(updateGalleryGrid)，若让网格的 sizeHint 再反过来参与横向分配，
    // 会形成「宽度→列宽→sizeHint→宽度」的自激环：拖动右边框时界面无限重排直至未响应。
    gallery->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    connect(gallery, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            selectPreset(row);
    });
    // 预设图库固定双栏(见 eventFilter)；预览区尺寸变化时按比例重绘
    gallery->viewport()->installEventFilter(this);
    leftLay->addWidget(gallery, 1);

    // custom image buttons
    auto *folderRow = new QHBoxLayout();
    auto *folderBtn = new QPushButton(QStringLiteral("选择文件夹"), leftCard);
    folderBtn->setObjectName(QStringLiteral("PrimaryButton"));
    folderBtn->setToolTip(tooltipstyle::format(QStringLiteral("读取文件夹内所有符合格式的图片并展示到图库")));
    connect(folderBtn, &QPushButton::clicked, this, &MainWindow::pickPresetFolder);
    auto *refreshBtn = new QPushButton(QStringLiteral("刷新"), leftCard);
    refreshBtn->setObjectName(QStringLiteral("VideoScanButton"));
    refreshBtn->setToolTip(tooltipstyle::format(QStringLiteral("重新加载当前文件夹的图片")));
    connect(refreshBtn, &QPushButton::clicked, this, [this] {
        rebuildGallery();
        setLog(QStringLiteral("图库已刷新：共 %1 张图片。").arg(m_presets.size()), false);
    });
    folderRow->addWidget(folderBtn, 2);
    folderRow->addWidget(refreshBtn, 1);
    leftLay->addLayout(folderRow);
    auto *wallBtn = new QPushButton(QStringLiteral("用桌面壁纸"), leftCard);
    wallBtn->setToolTip(tooltipstyle::format(QStringLiteral("截取当前桌面壁纸作为背景")));
    connect(wallBtn, &QPushButton::clicked, this, &MainWindow::pickWallpaper);
    leftLay->addWidget(wallBtn);

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
            "恢复默认参数：尺寸100%、显示位置右下、亮度/对比度100%、模糊0、不透明度255，\n"
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
    m_opacity = makeSlider(30, 255, 255, &m_opacityVal);

    addRow(0, QStringLiteral("尺寸"), m_scale, m_scaleVal);
    addRow(1, QStringLiteral("亮度"), m_brightness, m_brightnessVal);
    addRow(2, QStringLiteral("对比度"), m_contrast, m_contrastVal);
    addRow(3, QStringLiteral("模糊"), m_blur, m_blurVal);
    addRow(4, QStringLiteral("不透明度"), m_opacity, m_opacityVal);
    m_rotate = makeSlider(-180, 180, 0, &m_rotateVal, QStringLiteral("°"));
    m_rotate->setToolTip(tooltipstyle::format(QStringLiteral(
            "绕图片竖直中心轴旋转的投影：向左逆时针、向右顺时针，±180° 即左右镜像")));
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

    // 模式行：单图 / 随机(互斥单选，默认单图)。两个单选框同属 rightCard，
    // Qt 自动互斥，与视频壁纸页的播放模式行同一写法。
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
    m_applyImageBtn = new QPushButton(QStringLiteral("应用图片背景"), rightCard);
    m_applyImageBtn->setObjectName(QStringLiteral("PrimaryButton"));
    auto *resetBtn = new QPushButton(QStringLiteral("恢复"), rightCard);
    resetBtn->setObjectName(QStringLiteral("DangerButton"));
    connect(m_applyImageBtn, &QPushButton::clicked, this, &MainWindow::applyImage);
    connect(resetBtn, &QPushButton::clicked, this, &MainWindow::uninstallImage);
    btnRow->addWidget(m_applyImageBtn, 1);
    btnRow->addWidget(resetBtn, 1);
    rightLay->addLayout(btnRow);
    // 预览框高度改成按桌面比例锁定后不再吃纵向拉伸，剩余空间统一留到卡片底部。
    rightLay->addStretch(1);

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
        btn->setToolTip(tooltipstyle::format(p.desc));
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
    auto *resetBtn = new QPushButton(QStringLiteral("恢复"), btnCard);
    resetBtn->setObjectName(QStringLiteral("DangerButton"));
    connect(m_applyEffectBtn, &QPushButton::clicked, this, &MainWindow::applyEffect);
    connect(resetBtn, &QPushButton::clicked, this, &MainWindow::uninstallEffect);
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

    // 播放模式：单循环(默认) / 列表循环 / 随机，三选一互斥
    auto *playModeRow = new QHBoxLayout();
    playModeRow->setSpacing(6);
    playModeRow->addWidget(new QLabel(QStringLiteral("模式"), leftCard));
    playModeRow->addStretch(1);
    m_modeSingle = new QRadioButton(QStringLiteral("单循环"), leftCard);
    m_modeSingle->setToolTip(tooltipstyle::format(QStringLiteral(
            "只播列表里选中的这一个视频，播完从头再来，周而复始")));
    m_modeList = new QRadioButton(QStringLiteral("列表循环"), leftCard);
    m_modeList->setToolTip(tooltipstyle::format(QStringLiteral(
            "列表里的视频按顺序一个接一个播，播完最后一个回到第一个，周而复始")));
    m_modeRandom = new QRadioButton(QStringLiteral("随机"), leftCard);
    m_modeRandom->setToolTip(tooltipstyle::format(QStringLiteral(
            "随机从列表挑一个视频作为壁纸，播完再随机挑下一个，周而复始")));
    m_modeSingle->setChecked(true);
    auto *modeGroup = new QButtonGroup(this);
    modeGroup->setExclusive(true);
    modeGroup->addButton(m_modeSingle, VideoWallpaper::SingleLoop);
    modeGroup->addButton(m_modeList, VideoWallpaper::ListLoop);
    modeGroup->addButton(m_modeRandom, VideoWallpaper::Random);
    connect(modeGroup, &QButtonGroup::idClicked, this, [this](int mode) {
        VideoWallpaper::instance().setPlayMode(mode);
        AppConfig &st = AppConfig::instance();
        st.setValue(ConfigKeys::Video::PlayMode, mode);
    });
    playModeRow->addWidget(m_modeSingle);
    playModeRow->addWidget(m_modeList);
    playModeRow->addWidget(m_modeRandom);
    leftLay->addLayout(playModeRow);

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

    m_fullscreenPauseBox = new QCheckBox(QStringLiteral("全屏自动暂停"), leftCard);
    m_fullscreenPauseBox->setChecked(true);
    m_fullscreenPauseBox->setToolTip(tooltipstyle::format(QStringLiteral(
            "前台应用全屏或完全遮住桌面时暂停视频壁纸(省 GPU/电量)，回到桌面 1 秒内自动恢复")));
    m_batteryBox = new QCheckBox(QStringLiteral("电池模式自动暂停"), leftCard);
    m_batteryBox->setToolTip(tooltipstyle::format(QStringLiteral("使用电池供电时自动暂停视频壁纸以省电，接通电源后自动恢复")));
    leftLay->addWidget(m_fullscreenPauseBox);
    leftLay->addWidget(m_batteryBox);

    m_reclaimBox = new QCheckBox(QStringLiteral("自动回收内存"), leftCard);
    m_reclaimBox->setChecked(true);
    m_reclaimBox->setToolTip(tooltipstyle::format(QStringLiteral("定期把空闲内存还给系统，控制内存占用")));
    leftLay->addWidget(m_reclaimBox);
    m_affinityBox = new QCheckBox(QStringLiteral("资源友好模式"), leftCard);
    m_affinityBox->setChecked(true);
    m_affinityBox->setToolTip(tooltipstyle::format(QStringLiteral(
            "把本程序限制到最多 4 个逻辑核(优先落在不同物理核上)：解码线程与"
            "内存/显存占用随之下降\n"
            "(实测 1080p 内存 -27%、显存 -36%，CPU 不变)。更改后重启生效。")));
    leftLay->addWidget(m_affinityBox);
    connect(m_affinityBox, &QCheckBox::toggled, this, [this](bool on) {
        AppConfig &st = AppConfig::instance();
        st.setValue(ConfigKeys::Video::AffinityLimit, on);
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
    m_fpsBox->setToolTip(tooltipstyle::format(QStringLiteral(
            "限制壁纸呈现帧率：视频帧率高于上限时按上限放慢呈现节奏(画面为慢动作效果)；"
            "“跟随视频”保持原生帧率")));
    connect(m_fpsBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        static const int fpsValues[] = {0, 15, 24, 30, 60};
        VideoWallpaper::instance().setTargetFps(fpsValues[qBound(0, index, 4)]);
        AppConfig &st = AppConfig::instance();
        st.setValue(ConfigKeys::Video::TargetFps, fpsValues[qBound(0, index, 4)]);
    });
    fpsRow->addWidget(m_fpsBox, 1);
    leftLay->addLayout(fpsRow);

    // 左列两张卡片：上方视频壁纸参数，下方视频转码，中间留出明显间隙。
    // 卡片高度改为随内容收缩(去掉卡内 addStretch)，空白集中到列尾。
    auto *leftCol = new QWidget(page);
    leftCol->setFixedWidth(250);
    auto *leftColLay = new QVBoxLayout(leftCol);
    leftColLay->setContentsMargins(0, 0, 0, 0);
    leftColLay->setSpacing(18);
    leftColLay->addWidget(leftCard);
    leftColLay->addWidget(buildTranscodeCard(leftCol));
    leftColLay->addStretch(1);
    lay->addWidget(leftCol);

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
    m_videoList->setToolTip(tooltipstyle::format(QStringLiteral(
            "运行中双击条目：立即切换该视频为壁纸；选中条目后点“启动”：从该视频开始播放")));
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
    // “立即转码”只在恰好选中一个视频时可用，与启动/暂停/取消互不影响
    connect(m_videoList, &QListWidget::itemSelectionChanged,
            this, &MainWindow::updateTranscodeButton);
    listRow->addWidget(m_videoList, 1);

    auto *strip = new QVBoxLayout();
    strip->setSpacing(8);
    auto addStripBtn = [&](const QString &text, const char *objectName, auto slot) {
        auto *b = new QPushButton(text, rightCard);
        b->setObjectName(QString::fromUtf8(objectName)); // 语义配色见 style.qss/light.qss
        b->setMinimumWidth(72);
        b->setMinimumHeight(38);
        connect(b, &QPushButton::clicked, this, slot);
        strip->addWidget(b);
        return b;
    };
    addStripBtn(QStringLiteral("扫描"), "VideoScanButton", [this] { scanVideoDir(); });
    strip->addSpacing(46); // 扫描(发现类)与列表管理三键之间空一个按键距离
    addStripBtn(QStringLiteral("添加"), "VideoAddButton", [this] { addVideos(); });
    addStripBtn(QStringLiteral("删除"), "VideoDeleteButton", [this] { removeSelectedVideos(); });
    addStripBtn(QStringLiteral("清空"), "VideoClearButton", [this] { clearVideos(); });
    strip->addStretch(1);
    listRow->addLayout(strip);
    rightLay->addLayout(listRow, 1);

    m_videoStatus = new QLabel(QStringLiteral("共 0 个视频 · 停止"), rightCard);
    m_videoStatus->setObjectName(QStringLiteral("HintLabel"));
    rightLay->addWidget(m_videoStatus);

    lay->addWidget(rightCard, 1);

    // option changes apply immediately
    connect(m_fullscreenPauseBox, &QCheckBox::toggled, this, [this](bool on) {
        VideoWallpaper::instance().setPauseOnFullscreen(on);
        AppConfig &st = AppConfig::instance();
        st.setValue(ConfigKeys::Video::PauseFullscreen, on);
    });
    connect(m_batteryBox, &QCheckBox::toggled, this, [this](bool on) {
        VideoWallpaper::instance().setPauseOnBattery(on);
        AppConfig &st = AppConfig::instance();
        st.setValue(ConfigKeys::Video::PauseBattery, on);
    });
    connect(m_screenModeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        VideoWallpaper::instance().setScreenMode(idx);
        AppConfig &st = AppConfig::instance();
        st.setValue(ConfigKeys::Video::ScreenMode, idx);
    });
    connect(m_reclaimBox, &QCheckBox::toggled, this, [this](bool on) {
        VideoWallpaper::instance().setReclaimMemory(on);
        AppConfig &st = AppConfig::instance();
        st.setValue(ConfigKeys::Video::Reclaim, on);
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

// ---------------------------------------------------------------------------
// 视频转码(ffmpeg)：帧率重采样 + 可选去音频，输出落在源视频同目录。
// 全程用 QProcess 异步跑，UI 不阻塞。
// ---------------------------------------------------------------------------
namespace {

// 定位 ffmpeg.exe：优先程序目录及其常见子目录，最后回退系统 PATH。
QString findFfmpeg()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/ffmpeg.exe"),
        appDir + QStringLiteral("/tools/ffmpeg.exe"),
        appDir + QStringLiteral("/resources/ffmpeg.exe"),
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c))
            return QFileInfo(c).absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

// 编码器运行时探测(结果缓存)：有些发行版(例如 conda 构建)是 --disable-gpl，
// 根本没有 libx264，所以不能写死；按 libx264 → libopenh264 → mpeg4 取首个可用。
QString pickVideoEncoder()
{
    static QString cached;
    if (!cached.isEmpty())
        return cached;
    const QString exe = findFfmpeg();
    if (exe.isEmpty())
        return cached;

    QProcess probe;
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(exe, {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")});
    // 这里在点击线程上等，所以超时给得很短：探测不出来就用 mpeg4，
    // 它是 ffmpeg 自带的核心编码器，任何发行版都有，宁可画质差也不能卡住界面。
    if (!probe.waitForFinished(1500)) {
        probe.kill();
        probe.waitForFinished(200);
        cached = QStringLiteral("mpeg4");
        return cached;
    }

    QStringList available;
    const QString out = QString::fromUtf8(probe.readAllStandardOutput());
    for (const QString &line : out.split(QLatin1Char('\n'))) {
        const QString t = line.trimmed();
        if (t.isEmpty())
            continue;
        // 视频编码器行形如：" V....D libx264    H.264 / AVC / MPEG-4 AVC ..."，
        // 开头那个空格会被 trimmed 掉，所以只能按首字母 V 判断，再取第二个 token。
        const QStringList tokens = t.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (tokens.size() < 2 || !tokens.at(0).startsWith(QLatin1Char('V')))
            continue;
        available << tokens.at(1);
    }
    for (const char *cand : {"libx264", "libopenh264", "mpeg4"}) {
        const QString name = QString::fromLatin1(cand);
        if (available.contains(name)) {
            cached = name;
            break;
        }
    }
    if (cached.isEmpty() && !available.isEmpty())
        cached = available.first();
    return cached;
}

// "HH:MM:SS.xx" → 微秒，解析失败返回 -1。
qint64 parseFfmpegTime(const QString &value)
{
    const QStringList parts = value.trimmed().split(QLatin1Char(':'));
    if (parts.size() != 3)
        return -1;
    bool okH = false, okM = false, okS = false;
    const int h = parts.at(0).toInt(&okH);
    const int m = parts.at(1).toInt(&okM);
    const double s = parts.at(2).toDouble(&okS);
    if (!okH || !okM || !okS)
        return -1;
    return qint64((h * 3600 + m * 60) * 1000000.0 + s * 1000000.0);
}

} // namespace

QWidget *MainWindow::buildTranscodeCard(QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("PageCard"));
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(14, 14, 14, 14);
    lay->setSpacing(10);

    auto *t = new QLabel(QStringLiteral("视频转码"), card);
    t->setObjectName(QStringLiteral("GroupTitle"));
    lay->addWidget(t);

    auto *hint = new QLabel(
        QStringLiteral("转码列表中选中的视频：重采样帧率、可选去掉音频，结果保存在源视频同目录。"),
        card);
    hint->setObjectName(QStringLiteral("HintLabel"));
    hint->setWordWrap(true);
    lay->addWidget(hint);

    // 帧率：15/24/30/60 帧互斥单选，默认 24 帧(250px 卡片里排版很紧，行距压到 2)
    auto *fpsRow = new QHBoxLayout();
    fpsRow->setSpacing(2);
    fpsRow->addWidget(new QLabel(QStringLiteral("帧率"), card));
    m_tcFpsGroup = new QButtonGroup(this);
    m_tcFpsGroup->setExclusive(true);
    for (int fps : {15, 24, 30, 60}) {
        auto *rb = new QRadioButton(QStringLiteral("%1帧").arg(fps), card);
        rb->setToolTip(tooltipstyle::format(QStringLiteral(
                "转码后每秒 %1 帧：帧率越低越省解码资源，桌面壁纸建议 15 或 24 帧").arg(fps)));
        m_tcFpsGroup->addButton(rb, fps);
        if (fps == 24)
            rb->setChecked(true);
        fpsRow->addWidget(rb);
    }
    lay->addLayout(fpsRow);

    // 音频：无(默认，转码时丢掉音轨) / 有(保留并转 AAC)
    auto *audioRow = new QHBoxLayout();
    audioRow->setSpacing(6);
    audioRow->addWidget(new QLabel(QStringLiteral("音频"), card));
    m_tcAudioNo = new QRadioButton(QStringLiteral("无"), card);
    m_tcAudioNo->setToolTip(tooltipstyle::format(QStringLiteral(
            "不保留音频，输出文件不含音轨(桌面壁纸通常用不到声音)")));
    m_tcAudioYes = new QRadioButton(QStringLiteral("有"), card);
    m_tcAudioYes->setToolTip(tooltipstyle::format(QStringLiteral(
            "保留音频并转成 AAC 160k；源视频没有音轨时输出仍然无声")));
    m_tcAudioNo->setChecked(true);
    auto *audioGroup = new QButtonGroup(this);
    audioGroup->setExclusive(true);
    audioGroup->addButton(m_tcAudioNo);
    audioGroup->addButton(m_tcAudioYes);
    audioRow->addWidget(m_tcAudioNo);
    audioRow->addWidget(m_tcAudioYes);
    audioRow->addStretch(1);
    lay->addLayout(audioRow);

    m_transcodeBtn = new QPushButton(QStringLiteral("立即转码"), card);
    m_transcodeBtn->setObjectName(QStringLiteral("PrimaryButton"));
    m_transcodeBtn->setMinimumHeight(36);
    m_transcodeBtn->setEnabled(false); // 恰好选中一个视频才可点
    m_transcodeBtn->setToolTip(tooltipstyle::format(QStringLiteral(
            "先在右侧列表选中一个视频再点\n"
            "输出名：{源文件名}_{帧率}fps_{无声0/有声1}.mp4，保存在源视频所在文件夹\n"
            "转码完成后自动加入视频列表")));
    connect(m_transcodeBtn, &QPushButton::clicked, this, &MainWindow::transcodeSelectedVideo);
    lay->addWidget(m_transcodeBtn);

    return card;
}

// 按钮可用性只取决于「是否恰好选中一个视频」与「是否正在转码」，
// 与启动/暂停/取消三键互不影响。
void MainWindow::updateTranscodeButton()
{
    if (!m_transcodeBtn)
        return;
    const bool busy = m_transcodeProc && m_transcodeProc->state() != QProcess::NotRunning;
    if (busy) {
        m_transcodeBtn->setEnabled(false);
        return;
    }
    m_transcodeBtn->setText(QStringLiteral("立即转码"));
    m_transcodeBtn->setEnabled(m_videoList && m_videoList->selectedItems().size() == 1);
}

void MainWindow::transcodeSelectedVideo()
{
    if (!m_videoList || !m_tcFpsGroup || !m_tcAudioNo || !m_tcAudioYes)
        return;
    if (m_transcodeProc && m_transcodeProc->state() != QProcess::NotRunning)
        return; // 一次只跑一个转码任务

    const auto selected = m_videoList->selectedItems();
    if (selected.size() != 1) {
        setLog(QStringLiteral("请先在右侧视频列表中选中一个视频，再点“立即转码”。"), true);
        return;
    }
    const QString src = selected.first()->data(Qt::UserRole).toString();
    const QFileInfo fi(src);
    if (!fi.exists()) {
        setLog(QStringLiteral("源视频不存在：%1").arg(QDir::toNativeSeparators(src)), true);
        return;
    }
    const QString exe = findFfmpeg();
    if (exe.isEmpty()) {
        setLog(QStringLiteral("未找到 ffmpeg.exe：请把它放到程序目录或加入 PATH 后重试。"), true);
        return;
    }
    const QString encoder = pickVideoEncoder();
    if (encoder.isEmpty()) {
        setLog(QStringLiteral("ffmpeg 没有可用的视频编码器(libx264 / libopenh264 / mpeg4)。"), true);
        return;
    }

    const int fps = m_tcFpsGroup->checkedId();
    const bool keepAudio = m_tcAudioYes->isChecked();
    const QString dst = fi.absolutePath() + QLatin1Char('/')
                        + QStringLiteral("%1_%2fps_%3.mp4")
                              .arg(fi.completeBaseName()).arg(fps).arg(keepAudio ? 1 : 0);

    QStringList args;
    args << QStringLiteral("-hide_banner") << QStringLiteral("-nostdin")
         << QStringLiteral("-loglevel") << QStringLiteral("info")
         << QStringLiteral("-y") << QStringLiteral("-i") << src
         << QStringLiteral("-map") << QStringLiteral("0:v:0")
         << QStringLiteral("-vf") << QStringLiteral("fps=%1").arg(fps)
         << QStringLiteral("-c:v") << encoder;
    if (encoder == QLatin1String("libx264"))
        args << QStringLiteral("-preset") << QStringLiteral("veryfast")
             << QStringLiteral("-crf") << QStringLiteral("23");
    else
        args << QStringLiteral("-b:v") << QStringLiteral("6M"); // openh264/mpeg4 只吃码率
    args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
         << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    if (keepAudio)
        args << QStringLiteral("-map") << QStringLiteral("0:a:0?")
             << QStringLiteral("-c:a") << QStringLiteral("aac")
             << QStringLiteral("-b:a") << QStringLiteral("160k");
    else
        args << QStringLiteral("-an");
    args << dst;

    if (!m_transcodeProc) {
        m_transcodeProc = new QProcess(this);
        m_transcodeProc->setProcessChannelMode(QProcess::MergedChannels);
        connect(m_transcodeProc, &QProcess::readyReadStandardOutput,
                this, &MainWindow::onTranscodeOutput);
        connect(m_transcodeProc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
                    onTranscodeFinished(code, status != QProcess::NormalExit);
                });
        connect(m_transcodeProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return; // 其余错误走 finished 统一收尾，避免重复报错
            setLog(QStringLiteral("ffmpeg 启动失败：%1").arg(m_transcodeProc->errorString()), true);
            m_transcodeDst.clear();
            updateTranscodeButton();
        });
    }

    m_transcodeSrc = src;
    m_transcodeDst = dst;
    // 同名输出可能已经存在(重复转码同一个视频)，失败时只能清理本次写的半成品
    m_transcodeHadOutput = QFileInfo::exists(dst);
    m_transcodeTotalUs = 0;
    m_transcodeBuf.clear();
    m_transcodeErrTail.clear();
    m_transcodeBtn->setText(QStringLiteral("转码中 0%"));
    m_transcodeBtn->setEnabled(false);
    setLog(QStringLiteral("开始转码：%1 → %2（%3帧 · %4 · %5）")
               .arg(fi.fileName(), QFileInfo(dst).fileName(), QString::number(fps),
                    keepAudio ? QStringLiteral("有音频") : QStringLiteral("无音频"), encoder),
           false);
    m_transcodeProc->start(exe, args);
}

// ffmpeg 的进度行以 \r 分隔、普通日志以 \n 分隔，两种都当一行切出来。
void MainWindow::onTranscodeOutput()
{
    if (!m_transcodeProc)
        return;
    m_transcodeBuf += m_transcodeProc->readAllStandardOutput();

    auto handleLine = [this](const QString &raw) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            return;
        if (m_transcodeTotalUs <= 0) {
            const int at = line.indexOf(QStringLiteral("Duration:"));
            if (at >= 0) {
                int end = line.indexOf(QLatin1Char(','), at);
                if (end < 0)
                    end = line.size();
                m_transcodeTotalUs = parseFfmpegTime(line.mid(at + 9, end - (at + 9)));
            }
        }
        const int at = line.indexOf(QStringLiteral("time="));
        if (at >= 0 && m_transcodeTotalUs > 0 && m_transcodeBtn) {
            QString value = line.mid(at + 5);
            const int sp = value.indexOf(QLatin1Char(' '));
            if (sp >= 0)
                value = value.left(sp);
            const qint64 cur = parseFfmpegTime(value);
            if (cur >= 0) {
                const int pct = int(qBound(0.0,
                                           double(cur) * 100.0 / double(m_transcodeTotalUs),
                                           99.0));
                m_transcodeBtn->setText(QStringLiteral("转码中 %1%").arg(pct));
                return;
            }
        }
        // 非进度行留最后几行，失败时回显给用户定位原因
        m_transcodeErrTail += line + QLatin1Char('\n');
        if (m_transcodeErrTail.size() > 1500)
            m_transcodeErrTail = m_transcodeErrTail.right(1500);
    };

    int from = 0;
    for (;;) {
        const int cr = m_transcodeBuf.indexOf('\r', from);
        const int lf = m_transcodeBuf.indexOf('\n', from);
        int idx = -1;
        if (cr >= 0 && lf >= 0)
            idx = qMin(cr, lf);
        else if (cr >= 0)
            idx = cr;
        else if (lf >= 0)
            idx = lf;
        if (idx < 0)
            break;
        handleLine(QString::fromUtf8(m_transcodeBuf.constData() + from, idx - from));
        from = idx + 1;
    }
    m_transcodeBuf = m_transcodeBuf.mid(from);
}

void MainWindow::onTranscodeFinished(int exitCode, bool crashed)
{
    const QFileInfo out(m_transcodeDst);
    const bool ok = !crashed && exitCode == 0 && out.exists() && out.size() > 0;
    if (ok) {
        addVideoToPlaylist(out.absoluteFilePath());
        setLog(QStringLiteral("转码完成：%1").arg(out.fileName()), false);
    } else {
        if (out.exists() && !m_transcodeHadOutput)
            QFile::remove(out.absoluteFilePath()); // 半成品留在目录里只会误导
        QString reason = crashed ? QStringLiteral("ffmpeg 异常退出")
                                 : QStringLiteral("ffmpeg 返回错误码 %1").arg(exitCode);
        const QString tail = m_transcodeErrTail.trimmed();
        if (!tail.isEmpty())
            reason += QStringLiteral("：%1").arg(tail.section(QLatin1Char('\n'), -2).trimmed());
        setLog(QStringLiteral("转码失败（%1）").arg(reason), true);
    }
    m_transcodeSrc.clear();
    m_transcodeDst.clear();
    m_transcodeHadOutput = false;
    m_transcodeTotalUs = 0;
    m_transcodeBuf.clear();
    m_transcodeErrTail.clear();
    updateTranscodeButton();
}

// 把转码结果并入播放列表并持久化，同时选中它，方便连续操作。
void MainWindow::addVideoToPlaylist(const QString &path)
{
    QStringList list = VideoWallpaper::instance().playlist();
    if (!list.contains(path))
        list.append(path);
    VideoWallpaper::instance().setPlaylist(list);
    AppConfig &st = AppConfig::instance();
    st.setValue(ConfigKeys::Video::Playlist, list);
    refreshVideoList();

    if (m_videoList) {
        for (int i = 0; i < m_videoList->count(); ++i) {
            if (m_videoList->item(i)->data(Qt::UserRole).toString() != path)
                continue;
            m_videoList->clearSelection();
            // Qt6 的 QAbstractItemView 没有 SelectCurrent，直接改条目选中态最稳
            m_videoList->item(i)->setSelected(true);
            m_videoList->setCurrentRow(i);
            m_videoList->scrollToItem(m_videoList->item(i), QAbstractItemView::EnsureVisible);
            break;
        }
    }
    updateTranscodeButton();
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
    auto *resetBtn = new QPushButton(QStringLiteral("恢复"), card);
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

// —— 看板娘页 ————————————————————————————————————————————————
//
// 版式沿用动态壁纸页(左控制卡 + 右信息卡)，不另起一套视觉。
// 这一页只「读控制器、发意图」：状态文本、按钮可用性全部由 updateKanbanControls()
// 单点刷新，控件本身绝不判断「现在能不能暂停」。
QWidget *MainWindow::buildKanbanPage()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget(scroll);
    auto *lay = new QHBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(14);

    // ---- 左：运行控制 + 状态 ----
    auto *leftCard = new QFrame(page);
    leftCard->setObjectName(QStringLiteral("PageCard"));
    leftCard->setFixedWidth(250);
    auto *leftLay = new QVBoxLayout(leftCard);
    leftLay->setContentsMargins(14, 14, 14, 14);
    leftLay->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("看板娘"), leftCard);
    title->setObjectName(QStringLiteral("GroupTitle"));
    leftLay->addWidget(title);

    auto *hint = new QLabel(QStringLiteral("桌面上的小人常驻窗口，无边框、不抢焦点，可用鼠标拖动。"),
                            leftCard);
    hint->setObjectName(QStringLiteral("HintLabel"));
    hint->setWordWrap(true);
    leftLay->addWidget(hint);

    m_kanbanStartBtn = new QPushButton(QStringLiteral("启动"), leftCard);
    m_kanbanStartBtn->setObjectName(QStringLiteral("PrimaryButton"));
    m_kanbanStartBtn->setMinimumHeight(40);
    connect(m_kanbanStartBtn, &QPushButton::clicked, this, [this] {
        if (!m_kanban->start())
            setKanbanLog(m_kanban->lastError().isEmpty() ? QStringLiteral("看板娘启动失败")
                                                          : m_kanban->lastError(),
                         true);
        refreshKanbanModels();
        updateKanbanControls();
    });
    m_kanbanPauseBtn = new QPushButton(QStringLiteral("暂停"), leftCard);
    m_kanbanPauseBtn->setMinimumHeight(40);
    m_kanbanPauseBtn->setEnabled(false);
    connect(m_kanbanPauseBtn, &QPushButton::clicked, this, [this] {
        m_kanban->pauseResume();
        updateKanbanControls();
    });
    m_kanbanNextBtn = new QPushButton(QStringLiteral("播放下一个动作"), leftCard);
    m_kanbanNextBtn->setMinimumHeight(40);
    m_kanbanNextBtn->setEnabled(false);
    connect(m_kanbanNextBtn, &QPushButton::clicked, this, [this] {
        m_kanban->playNext();
        updateKanbanControls();
    });
    m_kanbanStopBtn = new QPushButton(QStringLiteral("取消"), leftCard);
    m_kanbanStopBtn->setObjectName(QStringLiteral("DangerButton"));
    m_kanbanStopBtn->setMinimumHeight(40);
    m_kanbanStopBtn->setEnabled(false);
    connect(m_kanbanStopBtn, &QPushButton::clicked, this, [this] {
        m_kanban->stop();
        updateKanbanControls();
    });
    leftLay->addWidget(m_kanbanStartBtn);
    leftLay->addWidget(m_kanbanPauseBtn);
    leftLay->addWidget(m_kanbanNextBtn);
    leftLay->addWidget(m_kanbanStopBtn);

    m_kanbanStatus = new QLabel(QStringLiteral("未启动"), leftCard);
    m_kanbanStatus->setObjectName(QStringLiteral("HintLabel"));
    m_kanbanStatus->setWordWrap(true);
    leftLay->addWidget(m_kanbanStatus);

    m_kanbanLog = new QLabel(QStringLiteral("就绪。"), leftCard);
    m_kanbanLog->setObjectName(QStringLiteral("LogLabel"));
    m_kanbanLog->setWordWrap(true);
    leftLay->addWidget(m_kanbanLog);
    leftLay->addStretch(1);

    lay->addWidget(leftCard);

    // ---- 右：模型 + 参数 ----
    auto *right = new QWidget(page);
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(14);
    rightLay->addWidget(buildKanbanModelCard(right));
    rightLay->addWidget(buildKanbanParamCard(right));
    rightLay->addStretch(1);
    lay->addWidget(right, 1);

    scroll->setWidget(page);
    return scroll;
}

QWidget *MainWindow::buildKanbanModelCard(QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("PageCard"));
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(14, 14, 14, 14);
    lay->setSpacing(10);

    auto *t = new QLabel(QStringLiteral("模型"), card);
    t->setObjectName(QStringLiteral("GroupTitle"));
    lay->addWidget(t);

    auto *row = new QHBoxLayout();
    row->setSpacing(8);
    m_kanbanModelCombo = new QComboBox(card);
    styleCombo(m_kanbanModelCombo);
    m_kanbanModelCombo->setToolTip(tooltipstyle::format(
        QStringLiteral("下拉里只列「文件齐全」的模型\n缺 moc3 / 贴图 / 动作的模型会被跳过并记入日志")));
    connect(m_kanbanModelCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (m_kanbanSyncing || index < 0)
            return;
        const QVector<kanban::ModelInfo> models = m_kanban->validModelList();
        if (index >= models.size())
            return;
        m_kanban->setModelPath(models.at(index).modelJsonPath);
        updateKanbanControls();
    });
    row->addWidget(m_kanbanModelCombo, 1);

    auto *refreshBtn = new QPushButton(QStringLiteral("刷新"), card);
    refreshBtn->setMinimumHeight(32);
    refreshBtn->setToolTip(tooltipstyle::format(
        QStringLiteral("重新扫描模型目录(新增/删除模型后点一下)")));
    connect(refreshBtn, &QPushButton::clicked, this, [this] {
        refreshKanbanModels();
        setKanbanLog(QStringLiteral("已重新扫描模型目录。"), false);
    });
    row->addWidget(refreshBtn);

    auto *openBtn = new QPushButton(QStringLiteral("打开模型目录"), card);
    openBtn->setMinimumHeight(32);
    connect(openBtn, &QPushButton::clicked, this, [] {
        const QString dir = kanban::KanbanModelManager::defaultModelsRoot();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    row->addWidget(openBtn);
    lay->addLayout(row);

    m_kanbanModelInfo = new QLabel(card);
    m_kanbanModelInfo->setObjectName(QStringLiteral("HintLabel"));
    m_kanbanModelInfo->setWordWrap(true);
    lay->addWidget(m_kanbanModelInfo);

    auto *pathHint = new QLabel(
        QStringLiteral("把 Cubism 模型整个文件夹放进 <程序目录>\\data\\kanban\\models，"
                       "每个模型一个子目录，内含 *.model3.json。"),
        card);
    pathHint->setObjectName(QStringLiteral("HintLabel"));
    pathHint->setWordWrap(true);
    lay->addWidget(pathHint);
    return card;
}

QWidget *MainWindow::buildKanbanParamCard(QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("PageCard"));
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(14, 14, 14, 14);
    lay->setSpacing(10);

    auto *t = new QLabel(QStringLiteral("显示与互动"), card);
    t->setObjectName(QStringLiteral("GroupTitle"));
    lay->addWidget(t);

    // 三行滑块统一用「标签 | 滑块 | 数值」的网格，数值列等宽，来回拉动时不抖版。
    auto *grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);

    auto *scaleLbl = new QLabel(QStringLiteral("缩放"), card);
    m_kanbanScale = makeSlider(20, 300, 100, &m_kanbanScaleVal, QStringLiteral("%"));
    m_kanbanScale->setToolTip(tooltipstyle::format(
        QStringLiteral("相对模型基准尺寸的百分比，也可在窗口上用滚轮调")));
    connect(m_kanbanScale, &QSlider::valueChanged, this, [this](int v) {
        if (!m_kanbanSyncing)
            m_kanban->setScalePercent(v);
    });
    grid->addWidget(scaleLbl, 0, 0);
    grid->addWidget(m_kanbanScale, 0, 1);
    grid->addWidget(m_kanbanScaleVal, 0, 2);

    auto *opacityLbl = new QLabel(QStringLiteral("不透明度"), card);
    m_kanbanOpacity = makeSlider(20, 100, 100, &m_kanbanOpacityVal, QStringLiteral("%"));
    m_kanbanOpacity->setToolTip(tooltipstyle::format(
        QStringLiteral("整个窗口不透明度，20% 是最淡一档")));
    connect(m_kanbanOpacity, &QSlider::valueChanged, this, [this](int v) {
        if (!m_kanbanSyncing)
            m_kanban->setOpacityPercent(v);
    });
    grid->addWidget(opacityLbl, 1, 0);
    grid->addWidget(m_kanbanOpacity, 1, 1);
    grid->addWidget(m_kanbanOpacityVal, 1, 2);

    auto *fpsLbl = new QLabel(QStringLiteral("动画帧率"), card);
    m_kanbanFps = makeSlider(10, 60, 30, &m_kanbanFpsVal, QStringLiteral(" fps"));
    m_kanbanFps->setToolTip(tooltipstyle::format(
        QStringLiteral("动画推进的目标帧率。越低越省 CPU/GPU，10~30 通常够用")));
    connect(m_kanbanFps, &QSlider::valueChanged, this, [this](int v) {
        if (!m_kanbanSyncing)
            m_kanban->setTargetFps(v);
    });
    grid->addWidget(fpsLbl, 2, 0);
    grid->addWidget(m_kanbanFps, 2, 1);
    grid->addWidget(m_kanbanFpsVal, 2, 2);
    grid->setColumnStretch(1, 1);
    lay->addLayout(grid);

    // 互动三项：置顶与穿透互相独立(置顶只管层级，穿透只管鼠标)，所以用复选框而非单选。
    auto *checkRow = new QHBoxLayout();
    checkRow->setSpacing(14);
    m_kanbanTopBox = new QCheckBox(QStringLiteral("窗口置顶"), card);
    connect(m_kanbanTopBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setAlwaysOnTop(on);
    });
    m_kanbanThroughBox = new QCheckBox(QStringLiteral("鼠标穿透"), card);
    m_kanbanThroughBox->setToolTip(tooltipstyle::format(
        QStringLiteral("开启后鼠标对窗口隐形，点击全部落到桌面；右键菜单仍可关闭")));
    connect(m_kanbanThroughBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setMouseThrough(on);
    });
    m_kanbanInteractBox = new QCheckBox(QStringLiteral("允许点击互动"), card);
    connect(m_kanbanInteractBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setInteractionEnabled(on);
    });
    checkRow->addWidget(m_kanbanTopBox);
    checkRow->addWidget(m_kanbanThroughBox);
    checkRow->addWidget(m_kanbanInteractBox);
    checkRow->addStretch(1);
    lay->addLayout(checkRow);

    auto *sep = new QFrame(card);
    sep->setObjectName(QStringLiteral("SideCardSep"));
    sep->setFrameShape(QFrame::HLine);
    lay->addWidget(sep);

    auto *trayTitle = new QLabel(QStringLiteral("开机与托盘"), card);
    trayTitle->setObjectName(QStringLiteral("GroupTitle"));
    lay->addWidget(trayTitle);

    auto *trayRow1 = new QHBoxLayout();
    trayRow1->setSpacing(14);
    m_kanbanAutoStartBox = new QCheckBox(QStringLiteral("程序启动时自动运行看板娘"), card);
    connect(m_kanbanAutoStartBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setAutoStart(on);
    });
    m_kanbanPauseHiddenBox = new QCheckBox(QStringLiteral("主界面隐藏时暂停动画"), card);
    m_kanbanPauseHiddenBox->setToolTip(tooltipstyle::format(
        QStringLiteral("开启后收起主界面即停帧，省 CPU/GPU；关闭则继续动")));
    connect(m_kanbanPauseHiddenBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setPauseWhenMainHidden(on);
    });
    trayRow1->addWidget(m_kanbanAutoStartBox);
    trayRow1->addWidget(m_kanbanPauseHiddenBox);
    trayRow1->addStretch(1);
    lay->addLayout(trayRow1);

    auto *trayRow2 = new QHBoxLayout();
    trayRow2->setSpacing(14);
    m_trayMinimizeBox = new QCheckBox(QStringLiteral("关闭主窗口时收进托盘(需有后台任务)"), card);
    connect(m_trayMinimizeBox, &QCheckBox::toggled, this, [this](bool on) {
        if (m_kanbanSyncing)
            return;
        AppConfig::instance().setValue(ConfigKeys::Tray::MinimizeToTrayOnClose, on);
    });
    m_trayAlwaysBox = new QCheckBox(QStringLiteral("仅在后台任务运行时显示托盘"), card);
    m_trayAlwaysBox->setToolTip(tooltipstyle::format(
        QStringLiteral("取消勾选 = 托盘常驻，无论有没有启动壁纸或看板娘")));
    connect(m_trayAlwaysBox, &QCheckBox::toggled, this, [this](bool on) {
        if (m_kanbanSyncing)
            return;
        AppConfig::instance().setValue(ConfigKeys::Tray::ShowWhenBackgroundTaskRunning, on);
        if (m_tray)
            m_tray->updateRuntimeState();
    });
    trayRow2->addWidget(m_trayMinimizeBox);
    trayRow2->addWidget(m_trayAlwaysBox);
    trayRow2->addStretch(1);
    lay->addLayout(trayRow2);

    return card;
}

// 装配顺序有讲究：控件先建好(buildKanbanPage 已在构造函数前段跑完)，
// 这里才创建控制器(它的构造会 loadSettings)，最后创建托盘。
// 退出步骤按「后注册先执行」登记，于是实际收口顺序是
// 收托盘 → 停看板娘 → 停视频壁纸 → 存配置 → quit。
void MainWindow::setupKanbanAndTray()
{
    m_kanban = std::make_unique<kanban::KanbanController>(this);
    kanban::KanbanController &kan = *m_kanban;

    connect(&kan, &kanban::KanbanController::runningChanged, this,
            [this](bool running) {
                if (running)
                    refreshKanbanModels();
                updateKanbanControls();
            });
    connect(&kan, &kanban::KanbanController::pausedChanged, this,
            [this](bool paused) {
                updateKanbanControls();
                setKanbanLog(paused ? QStringLiteral("看板娘已暂停，资源和位置都保留。")
                                    : QStringLiteral("看板娘已恢复动画。"),
                             false);
            });
    connect(&kan, &kanban::KanbanController::stateChanged, this,
            [this](const QString &text) {
                updateKanbanControls();
                if (text == QStringLiteral("启动失败"))
                    setKanbanLog(m_kanban->lastError(), true);
            });
    connect(&kan, &kanban::KanbanController::backendChanged, this,
            [this](const QString &backend) {
                setKanbanLog(QStringLiteral("渲染后端：%1").arg(backend), false);
                updateKanbanControls();
            });
    connect(&kan, &kanban::KanbanController::currentModelChanged, this,
            [this](const QString &name) {
                updateKanbanControls();
                setKanbanLog(name.isEmpty() ? QStringLiteral("未装载模型，使用内置占位形象。")
                                            : QStringLiteral("当前模型：%1").arg(name),
                             false);
            });
    // 右键菜单「打开主界面设置」：把主窗口捞回来并停在看板娘页。
    connect(&kan, &kanban::KanbanController::openSettingsRequested, this, [this] {
        showFromTray();
        m_nav->setCurrentRow(2);
    });

    refreshKanbanModels();
    updateKanbanControls();

    // 构造函数开头就恢复了上次的视频壁纸，那时 playbackStateChanged 还没接上，
    // 聚合器里会是「没在跑」；托盘初始化依赖这个事实，先补一次。
    {
        VideoWallpaper &video = VideoWallpaper::instance();
        ApplicationRuntimeState::instance().setWallpaperState(
            video.isStarted(), video.isStarted() && !video.isPlaying());
    }

    m_tray = new SystemTrayController(this);
    m_tray->setKanbanController(m_kanban.get());
    const bool trayReady = m_tray->initialize();
    if (trayReady) {
        connect(m_tray, &SystemTrayController::showMainWindowRequested, this,
                &MainWindow::showFromTray);
        connect(m_tray, &SystemTrayController::quitRequested, this,
                &MainWindow::onTrayQuitRequested);
        // 托盘在的时候进程归托盘管：主窗口是最后一个可见窗口，
        // 若沿用 Qt 默认策略，隐藏主窗口就会顺手把进程结束掉。
        QApplication::setQuitOnLastWindowClosed(false);
    }

    ApplicationShutdown &shutdown = ApplicationShutdown::instance();
    shutdown.addStep(QStringLiteral("停止视频壁纸"), [] {
        VideoWallpaper::instance().stopAll();
    });
    shutdown.addStep(QStringLiteral("停止看板娘"), [this] {
        if (m_kanban)
            m_kanban->shutdownForExit();
    });
    shutdown.addStep(QStringLiteral("隐藏托盘"), [this] {
        if (m_tray)
            m_tray->hideTray();
    });

    // 自动启动：等事件循环转起来再拉起，避免在构造函数里 show 一个顶层窗口。
    if (m_kanban->autoStart()) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (m_kanban->start())
                    refreshKanbanModels();
                updateKanbanControls();
            },
            Qt::QueuedConnection);
    }
}

// 重扫模型目录并回填下拉框，尽量保住用户当前选中的那一项。
void MainWindow::refreshKanbanModels()
{
    if (!m_kanbanModelCombo)
        return;
    const QString current = m_kanban->modelPath();
    const QVector<kanban::ModelInfo> models = m_kanban->validModelList();

    m_kanbanSyncing = true;
    m_kanbanModelCombo->clear();
    if (models.isEmpty()) {
        m_kanbanModelCombo->addItem(QStringLiteral("未发现可用模型"));
        m_kanbanModelCombo->setEnabled(false);
    } else {
        m_kanbanModelCombo->setEnabled(true);
        int keep = 0;
        for (int i = 0; i < models.size(); ++i) {
            m_kanbanModelCombo->addItem(models.at(i).name);
            if (!current.isEmpty() && models.at(i).modelJsonPath == current)
                keep = i;
        }
        m_kanbanModelCombo->setCurrentIndex(keep);
    }
    m_kanbanSyncing = false;
}

// 看板娘页所有可见状态的唯一出口：按钮可用性 + 状态行 + 控件回填。
// 回填放在同一个函数里，是为了让「控制器里的值」和「滑块上的值」不可能长期不一致。
void MainWindow::updateKanbanControls()
{
    if (!m_kanban || !m_kanbanStartBtn)
        return;

    const bool running = m_kanban->isRunning();
    const bool paused = m_kanban->isPaused();
    const bool failed = m_kanban->state() == kanban::State::Error;

    m_kanbanStartBtn->setEnabled(!running);
    m_kanbanStartBtn->setText(failed ? QStringLiteral("重试启动") : QStringLiteral("启动"));
    m_kanbanPauseBtn->setEnabled(running);
    m_kanbanPauseBtn->setText(paused ? QStringLiteral("继续") : QStringLiteral("暂停"));
    m_kanbanNextBtn->setEnabled(running && !paused);
    m_kanbanStopBtn->setEnabled(running || failed);

    QString status = QStringLiteral("状态：%1").arg(m_kanban->stateText());
    if (running)
        status += QStringLiteral(" · 后端 %1 · 实测 %2 fps")
                      .arg(m_kanban->backendText())
                      .arg(m_kanban->measuredFps());
    else
        status += QStringLiteral(" · 后端 %1").arg(m_kanban->backendText().isEmpty()
                                                       ? QStringLiteral("未启动")
                                                       : m_kanban->backendText());
    if (!m_kanban->currentModelName().isEmpty())
        status += QStringLiteral(" · 模型 %1").arg(m_kanban->currentModelName());
    m_kanbanStatus->setText(status);

    // 模型明细：把校验结果如实摊开，比「能不能用」四个字有用得多。
    const QVector<kanban::ModelInfo> models = m_kanban->validModelList();
    QString info;
    if (models.isEmpty()) {
        info = QStringLiteral("可用模型 0 个。");
    } else {
        int index = m_kanbanModelCombo ? m_kanbanModelCombo->currentIndex() : -1;
        if (index < 0 || index >= models.size())
            index = 0;
        const kanban::ModelInfo &sel = models.at(index);
        info = QStringLiteral("可用模型 %1 个 · 当前：%2 · 贴图 %3 · 动作组 %4 · 表情 %5")
                   .arg(models.size())
                   .arg(sel.name)
                   .arg(sel.textureCount)
                   .arg(sel.motionCount)
                   .arg(sel.expressionCount);
    }
    if (!m_kanban->live2dAvailable())
        info += QStringLiteral("\n本程序未编译 Live2D 后端，模型只扫描校验，画面用内置占位形象。");
    m_kanbanModelInfo->setText(info);

    m_kanbanSyncing = true;
    m_kanbanScale->setValue(m_kanban->scalePercent());
    m_kanbanOpacity->setValue(m_kanban->opacityPercent());
    m_kanbanFps->setValue(m_kanban->targetFps());
    m_kanbanTopBox->setChecked(m_kanban->alwaysOnTop());
    m_kanbanThroughBox->setChecked(m_kanban->mouseThrough());
    m_kanbanInteractBox->setChecked(m_kanban->interactionEnabled());
    m_kanbanAutoStartBox->setChecked(m_kanban->autoStart());
    m_kanbanPauseHiddenBox->setChecked(m_kanban->pauseWhenMainHidden());
    // 托盘两项直接读配置：它们不归 KanbanController 管，不回填的话
    // 每次重绘都会显示成未勾选，用户以为设置丢了。
    auto &cfg = AppConfig::instance();
    m_trayMinimizeBox->setChecked(
        cfg.value(ConfigKeys::Tray::MinimizeToTrayOnClose, true).toBool());
    m_trayAlwaysBox->setChecked(
        cfg.value(ConfigKeys::Tray::ShowWhenBackgroundTaskRunning, true).toBool());
    m_kanbanSyncing = false;

    if (m_tray)
        m_tray->updateRuntimeState();
}

// 看板娘页自己的日志行：主界面日志条只属于文件夹美化页，这里不借用它。
void MainWindow::setKanbanLog(const QString &text, bool isError)
{
    if (!m_kanbanLog)
        return;
    m_kanbanLog->setText(text);
    m_kanbanLog->setProperty("data-err", isError ? 1 : 0);
    m_kanbanLog->style()->unpolish(m_kanbanLog);
    m_kanbanLog->style()->polish(m_kanbanLog);
}

// 托盘「显示窗口」/双击图标：窗口可能是 hide() 掉的，先 show 再解最小化。
void MainWindow::showFromTray()
{
    show();
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
}

// 托盘「关闭软件」：真退出。这里只留一条界面日志，清理交给统一收口。
void MainWindow::onTrayQuitRequested()
{
    setKanbanLog(QStringLiteral("正在退出…"), false);
    ApplicationShutdown::instance().requestQuit(CloseReason::TrayQuit);
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
    // 第三页(看板娘)没有顶部门签：两个分支都要显式判断，
    // 否则它会落进 else 去 selectWallTab，把壁纸页的门签选中态改乱。
    if (row == 0)
        selectHeaderTab(m_stack->currentIndex());
    else if (row == 1)
        selectWallTab(m_wallStack->currentIndex());
    else if (row == 2)
        updateKanbanControls();
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
    // v2: PNG 保留透明通道(旧 JPEG 缩略图作废)
    // 图库缩略图缓存统一落在 <程序目录>/.cache/thumbnails(见 CachePaths)。
    CachePaths::ensureDirectories();
    return QDir(CachePaths::thumbnails())
        .filePath(hash + QStringLiteral("_v2.png"));
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
            // 后台线程只做“解码+缩放+落盘”，不触碰任何 UI 对象；回到 GUI 线程的
            // 回调必须先过存活闸门(见 m_thumbTasksLive / ~MainWindow)。
            QThreadPool::globalInstance()->start([this, res, thumb] {
                QImage img(res);
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
    // 默认从当前浏览目录开始选择；选中后持久化到配置
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

// 预览框宽高比锁死为主屏(桌面)比例：宽度由右侧卡片决定，高度按比例反算，
// 这样预览里的模拟资源管理器窗口和真实桌面是同一种形状，所见即所得。
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
    const int want = qMax(90, qRound(frameW / aspect));
    // 迟滞 3px：滚动条/网格的 1~2px 宽度抖动不值得改高度，
    // 否则「高度→滚动条→宽度→高度」会锁死在两个状态之间来回翻转。
    if (qAbs(want - m_previewFrame->height()) >= 3)
        m_previewFrame->setFixedHeight(want);
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

    // 源图按“路径+修改时间”缓存：调参和拖动窗口会反复重画，磁盘解码只做一次。
    const qint64 mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    const QString srcKey = path + QLatin1Char('#') + QString::number(mtime);
    if (srcKey != m_previewSrcKey) {
        QImage decoded(path);
        m_previewSrcNative = decoded.size();
        if (!decoded.isNull() && decoded.width() > 900)
            decoded = decoded.scaledToWidth(900, Qt::SmoothTransformation);
        m_previewSrcCache = decoded;
        m_previewSrcKey = srcKey;
    }
    const QImage src = m_previewSrcCache;
    const QSize native = m_previewSrcNative;
    QImage processed = ImageProcess::adjust(src, m_brightness->value() / 100.0,
                                            m_contrast->value() / 100.0, m_blur->value());
    processed = ImageProcess::rotateAroundY(processed, m_rotate->value());

    // 尺寸调节：原尺寸(居中/四角)模式下按比例放大/缩小，与真实写入的图片一致；
    // 填充/拉伸模式下宽高比不变，预览结果不受影响，与真实行为一致。
    const double pct = m_scale ? m_scale->value() / 100.0 : 1.0;
    const QSize effNative(qRound(native.width() * pct), qRound(native.height() * pct));

    // 真实参照：主屏(物理像素)，用来把图片按真实比例画进预览。
    QSize realWin(1920, 1080);
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect sg = screen->geometry();
        const qreal sdpr = screen->devicePixelRatio();
        realWin = QSize(int(sg.width() * sdpr), int(sg.height() * sdpr));
    }

    // 画布铺满整个预览框。预览框本身已被 updatePreviewAspect() 锁成桌面宽高比，
    // 所以这里不再需要居中留白，也不会出现上下两条灰底。
    const QSize labelSize = m_previewLabel->size();
    const QSize mock(qMax(160, labelSize.width()), qMax(90, labelSize.height()));
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
        if (msg->message == WM_ENDSESSION && msg->wParam == TRUE) {
            // 注销/关机：系统给的收尾窗口很短，直接走统一收口(存配置+停两路+quit)，
            // 绝不隐藏到托盘，也不弹任何确认框。
            ApplicationShutdown::instance().requestQuit(CloseReason::SystemShutdown);
        }
        if (const unsigned int showMsg = fbswin::showMainWindowMessage();
            showMsg && msg->message == showMsg) {
            // 二次启动的实例请求唤起：转回主线程走托盘「显示主窗口」同一条路，
            // 保证 show/raise/状态回填只有一处实现。
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("收到二次启动唤起请求，从托盘恢复主窗口"),
                           QLatin1String("UI"));
            QMetaObject::invokeMethod(this, &MainWindow::showFromTray, Qt::QueuedConnection);
            *result = 0;
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
    m_imageChip->setProperty("data-warn", (img.dangling || img.stale) ? 1 : 0);
    m_effectChip->setProperty("data-ok", eff.ours ? 1 : 0);
    m_effectChip->setProperty("data-warn", (eff.dangling || eff.stale) ? 1 : 0);
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

void MainWindow::reportDllMigration()
{
    const ComponentStatus img = Engine::instance().imageStatus();
    const ComponentStatus eff = Engine::instance().effectStatus();
    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("Hook DLL 目录 %1 | 图片 %2 | 特效 %3")
                       .arg(QDir::toNativeSeparators(Engine::dllRoot()),
                            Engine::statusText(img), Engine::statusText(eff)),
                   QStringLiteral("Engine"));
    if (!img.stale && !eff.stale)
        return;

    // 注册表里的 InprocServer32 存的是绝对路径，目录搬家后旧注册指向的位置只能靠
    // 重新注册覆盖(需要管理员权限，且 Explorer 会重启)。这里只把新 DLL 预先投放好，
    // 让用户点一次「应用」就能完成迁移，不自行提权，也不回退到旧目录。
    Engine::instance().ensureDataDirs();
    QString derr;
    if (!Engine::instance().extractDlls(&derr))
        setLog(derr, true);
    setLog(QStringLiteral("Hook DLL 已改用程序目录(%1)，检测到注册表仍指向旧位置，"
                          "点一次「应用图片背景」或「应用特效」即可完成迁移(需要管理员权限)。")
               .arg(QDir::toNativeSeparators(Engine::dllRoot())),
           false);
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
    AppConfig &s = AppConfig::instance();
    m_themeCombo->setCurrentIndex(s.value(ConfigKeys::Ui::Theme, 0).toInt());
    m_rotate->setValue(s.value(ConfigKeys::Image::Rotate, 0).toInt());
    m_scale->setValue(s.value(ConfigKeys::Image::Scale, 100).toInt());
    m_brightness->setValue(s.value(ConfigKeys::Image::Brightness, 100).toInt());
    m_contrast->setValue(s.value(ConfigKeys::Image::Contrast, 100).toInt());
    m_blur->setValue(s.value(ConfigKeys::Image::Blur, 0).toInt());
    m_opacity->setValue(s.value(ConfigKeys::Image::Opacity, 255).toInt());
    m_posMode = qBound(0, s.value(ConfigKeys::Image::PosType, 6).toInt(), 6);
    setPosMode(m_posMode);
    m_folderExt->setChecked(s.value(ConfigKeys::Image::FolderExt, false).toBool());
    {
        // 恢复模式选择。加载期间屏蔽 toggled，免得回写配置打断本次读取。
        const int imgMode = qBound(0, s.value(ConfigKeys::Image::Mode, 0).toInt(), 1);
        QSignalBlocker blockSingle(m_imgModeSingle);
        QSignalBlocker blockRandom(m_imgModeRandom);
        if (imgMode == 1)
            m_imgModeRandom->setChecked(true);
        else
            m_imgModeSingle->setChecked(true);
    }
    // 图片浏览目录：优先恢复用户上次选择的目录，否则默认 软件目录/media/image
    const QString savedDir = s.value(ConfigKeys::Image::GalleryDir).toString();
    if (!savedDir.isEmpty() && QDir(savedDir).exists())
        m_presetDir = savedDir;
    else
        m_presetDir = QCoreApplication::applicationDirPath() + QStringLiteral("/media/image");
    QDir().mkpath(m_presetDir);
    rebuildGallery();
    m_selectedPreset = s.value(ConfigKeys::Image::Preset, 0).toInt();
    m_customImage = s.value(ConfigKeys::Image::CustomPath).toString();
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
    m_effectCombo->setCurrentIndex(s.value(ConfigKeys::Effect::Type, 1).toInt());
    m_lightColor = QColor(s.value(ConfigKeys::Effect::LightColor, QStringLiteral("#ffffff")).toString());
    m_darkColor = QColor(s.value(ConfigKeys::Effect::DarkColor, QStringLiteral("#000000")).toString());
    m_lightAlpha->setValue(s.value(ConfigKeys::Effect::LightAlpha, 200).toInt());
    m_darkAlpha->setValue(s.value(ConfigKeys::Effect::DarkAlpha, 120).toInt());
    m_clearAddress->setChecked(s.value(ConfigKeys::Effect::ClearAddress, true).toBool());
    m_clearBarBg->setChecked(s.value(ConfigKeys::Effect::ClearBarBg, true).toBool());
    m_clearWinUIBg->setChecked(s.value(ConfigKeys::Effect::ClearWinUIBg, true).toBool());
    m_showLine->setChecked(s.value(ConfigKeys::Effect::ShowLine, false).toBool());
    m_lightColorBtn->setStyleSheet(
        QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
            .arg(m_lightColor.name()));
    m_darkColorBtn->setStyleSheet(
        QStringLiteral("QPushButton{background:%1;border:1px solid #3a3b44;border-radius:8px;}")
            .arg(m_darkColor.name()));
    // video wallpaper settings
    const QStringList playlist = s.value(ConfigKeys::Video::Playlist).toStringList();
    VideoWallpaper::instance().setPlaylist(playlist);
    refreshVideoList();
    m_videoVolume->setValue(s.value(ConfigKeys::Video::Volume, 0).toInt());
    {
        // 旧配置里的"列表循环播放/随机播放"两个开关已由 AppConfig 迁移成
        // video/playMode，这里只需按模式点亮对应的单选框。
        const int mode = qBound(int(VideoWallpaper::SingleLoop),
                                s.value(ConfigKeys::Video::PlayMode,
                                        VideoWallpaper::SingleLoop).toInt(),
                                int(VideoWallpaper::Random));
        // 三个单选框同属 leftCard，Qt 自动互斥：点亮一个即自动取消其余两个。
        if (mode == VideoWallpaper::ListLoop)
            m_modeList->setChecked(true);
        else if (mode == VideoWallpaper::Random)
            m_modeRandom->setChecked(true);
        else
            m_modeSingle->setChecked(true);
        VideoWallpaper::instance().setPlayMode(mode);
    }
    m_fullscreenPauseBox->setChecked(s.value(ConfigKeys::Video::PauseFullscreen, true).toBool());
    m_batteryBox->setChecked(s.value(ConfigKeys::Video::PauseBattery, false).toBool());
    {
        const int targetFps = s.value(ConfigKeys::Video::TargetFps, 24).toInt();
        static const int fpsValues[] = {0, 15, 24, 30, 60};
        for (int i = 0; i < 5; ++i)
            if (fpsValues[i] == targetFps) {
                m_fpsBox->setCurrentIndex(i);
                break;
            }
        VideoWallpaper::instance().setTargetFps(targetFps);
    }
    m_reclaimBox->setChecked(s.value(ConfigKeys::Video::Reclaim, true).toBool());
    m_affinityBox->setChecked(s.value(ConfigKeys::Video::AffinityLimit, true).toBool());
    m_screenModeCombo->setCurrentIndex(s.value(ConfigKeys::Video::ScreenMode, 0).toInt());
    VideoWallpaper::instance().setPauseOnFullscreen(m_fullscreenPauseBox->isChecked());
    VideoWallpaper::instance().setReclaimMemory(m_reclaimBox->isChecked());
    VideoWallpaper::instance().setScreenMode(m_screenModeCombo->currentIndex());
    VideoWallpaper::instance().setVolume(m_videoVolume->value());
}

void MainWindow::saveImageSettings()
{
    AppConfig &s = AppConfig::instance();
    s.setValue(ConfigKeys::Image::Rotate, m_rotate->value());
    s.setValue(ConfigKeys::Image::Scale, m_scale->value());
    s.setValue(ConfigKeys::Image::Brightness, m_brightness->value());
    s.setValue(ConfigKeys::Image::Contrast, m_contrast->value());
    s.setValue(ConfigKeys::Image::Blur, m_blur->value());
    s.setValue(ConfigKeys::Image::Opacity, m_opacity->value());
    s.setValue(ConfigKeys::Image::PosType, m_posMode);
    s.setValue(ConfigKeys::Image::FolderExt, m_folderExt->isChecked());
    s.setValue(ConfigKeys::Image::Mode,
               m_imgModeRandom && m_imgModeRandom->isChecked() ? 1 : 0);
    s.setValue(ConfigKeys::Image::Preset, m_selectedPreset);
    s.setValue(ConfigKeys::Image::CustomPath, m_customImage);
    s.setValue(ConfigKeys::Image::GalleryDir, m_presetDir);
}

void MainWindow::saveEffectSettings()
{
    AppConfig &s = AppConfig::instance();
    s.setValue(ConfigKeys::Effect::Type, m_effectCombo->currentIndex());
    s.setValue(ConfigKeys::Effect::LightColor, m_lightColor.name());
    s.setValue(ConfigKeys::Effect::DarkColor, m_darkColor.name());
    s.setValue(ConfigKeys::Effect::LightAlpha, m_lightAlpha->value());
    s.setValue(ConfigKeys::Effect::DarkAlpha, m_darkAlpha->value());
    s.setValue(ConfigKeys::Effect::ClearAddress, m_clearAddress->isChecked());
    s.setValue(ConfigKeys::Effect::ClearBarBg, m_clearBarBg->isChecked());
    s.setValue(ConfigKeys::Effect::ClearWinUIBg, m_clearWinUIBg->isChecked());
    s.setValue(ConfigKeys::Effect::ShowLine, m_showLine->isChecked());
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

    // DLL 只扫描 folder 目录里的 *.png / *.jpg：单图指向单张成品图所在目录，
    // 随机指向图片池目录，两者互不串台。
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
                                             m_opacity->value(), m_folderExt->isChecked(),
                                             randomMode, &err)) {
        setLog(err, true);
        m_applyImageBtn->setEnabled(true);
        return;
    }
    setLog(QStringLiteral("正在注册 DLL(需要管理员权限)…"), false);
    QCoreApplication::processEvents();

    // 只动图片 Hook：特效 Hook 的注册状态与配置原样保留，两页互不影响。
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
    // 尺寸调节：DLL 没有缩放参数，通过缩放实际写入的图片改变其在
    // 资源管理器中的相对大小(仅原尺寸模式；填充/拉伸始终铺满窗口)。
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

    // 只动特效 Hook：图片 Hook 的注册状态与配置原样保留，两页互不影响。
    saveEffectSettings();
    if (!Engine::instance().registerEffectDll(&err)) {
        setLog(err, true);
        m_applyEffectBtn->setEnabled(true);
        return;
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
    const QString videoDir = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/media/video");
    QDir().mkpath(videoDir);
    const QStringList nameFilters = {
        QStringLiteral("*.mp4"),  QStringLiteral("*.webm"), QStringLiteral("*.mkv"),
        QStringLiteral("*.avi"),  QStringLiteral("*.mov"),  QStringLiteral("*.wmv")};

    // 先尝试选择文件；若取消则尝试选择文件夹
    QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择视频文件"), videoDir,
        QStringLiteral("视频文件 (*.mp4 *.webm *.mkv *.avi *.mov *.wmv);;所有文件 (*)"));

    if (files.isEmpty()) {
        // 用户未选文件，尝试选择文件夹
        QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择视频文件夹"), videoDir);
        if (dir.isEmpty())
            return;
        QDirIterator it(dir, nameFilters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            files.append(it.next());
    }

    if (files.isEmpty())
        return;
    QStringList list = VideoWallpaper::instance().playlist();
    for (const QString &f : files)
        if (!list.contains(f))
            list.append(f);
    VideoWallpaper::instance().setPlaylist(list);
    AppConfig &st = AppConfig::instance();
    st.setValue(ConfigKeys::Video::Playlist, list);
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
    AppConfig &st = AppConfig::instance();
    st.setValue(ConfigKeys::Video::Playlist, list);
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
    AppConfig &st = AppConfig::instance();
    st.setValue(ConfigKeys::Video::Playlist, list);
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
    AppConfig &st = AppConfig::instance();
    st.remove(ConfigKeys::Video::Playlist);
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
    AppConfig &st = AppConfig::instance();
    st.setValue(ConfigKeys::Video::WasPlaying, true);
    setLog(QStringLiteral("视频壁纸运行中：画面在桌面图标之后，保持程序运行即可。"), false);
}

void MainWindow::stopVideo()
{
    VideoWallpaper::instance().stopAll();
    AppConfig &st = AppConfig::instance();
    st.setValue(ConfigKeys::Video::WasPlaying, false);
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
    updateTranscodeButton();  // 列表重建后选中项已失效，“立即转码”要跟着置灰
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
    // 用户可见的壁纸状态单独落一条诊断日志。排查循环边界是否出现
    // "第 N 个 播放中 → 播放结束 → 第 N 个 播放中"这类可见抖动时，
    // 这是唯一直接的取证点(任务书 十三.2)。
    videodiag::log(videodiag::Level::Debug,
                   QStringLiteral("UI状态 %1").arg(text));
    if (m_videoStatus)
        m_videoStatus->setText(text);
    updateVideoButtons();
    updatePlayingHighlight();

    // 这里是视频壁纸所有状态变化(启动/暂停/自动挂起/取消)的唯一漏斗，
    // 把事实推进聚合器；托盘菜单和「关窗口是隐藏还是退出」只认那一份。
    // running 用 isStarted()(用户启动过且未取消)，不是 isPlaying()，
    // 否则自动挂起期间会被误判成「没在跑」，进程就被 Qt 顺手收掉了。
    VideoWallpaper &video = VideoWallpaper::instance();
    ApplicationRuntimeState::instance().setWallpaperState(
        video.isStarted(), video.isStarted() && !video.isPlaying());
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

void MainWindow::saveWindowGeometry()
{
    // 窗口几何持久化(统一配置)：退出时保存尺寸/位置/最大化状态
    // 使用 m_savedWindowSize 而非 width()/height() 防止布局漂移导致窗口尺寸逐次膨胀
    auto &cfg = AppConfig::instance();
    cfg.setValue(ConfigKeys::Window::Maximized, isMaximized());
    if (!isMaximized()) {
        cfg.setValue(ConfigKeys::Window::Width, m_savedWindowSize.width());
        cfg.setValue(ConfigKeys::Window::Height, m_savedWindowSize.height());
        cfg.setValue(ConfigKeys::Window::X, x());
        cfg.setValue(ConfigKeys::Window::Y, y());
    }
    cfg.save();
}

// 关闭请求的三分岔(任务书 §7.6)：
//   已经在退出流程里     → 放手，让 Qt 正常销毁窗口
//   有后台任务且允许最小化 → 只隐藏窗口，进程交给托盘
//   其余                 → 走统一退出收口，避免「隐藏后没人管进程」
void MainWindow::closeEvent(QCloseEvent *event)
{
    ApplicationRuntimeState &rt = ApplicationRuntimeState::instance();
    if (rt.isQuitting()) {
        saveWindowGeometry();
        QMainWindow::closeEvent(event);
        return;
    }

    const bool hideInstead =
        rt.shouldKeepProcessAlive() && m_tray && m_tray->isAvailable()
        && AppConfig::instance().value(ConfigKeys::Tray::MinimizeToTrayOnClose, true).toBool();

    saveWindowGeometry();
    if (hideInstead) {
        videodiag::log(videodiag::Level::Info,
                       QStringLiteral("主窗口关闭请求转为隐藏(后台任务运行中)"),
                       QLatin1String("UI"));
        hide();
        event->ignore();
        return;
    }

    // 无后台任务：这是唯一一条「点 ✕ 就该走人」的路，交给统一收口善后。
    event->ignore();
    ApplicationShutdown::instance().requestQuit(CloseReason::UserWindowClose);
}

void MainWindow::hideEvent(QHideEvent *event)
{
    QMainWindow::hideEvent(event);
    ApplicationRuntimeState::instance().setMainWindowVisible(false);
    if (m_kanban)
        m_kanban->applyMainWindowVisible(false);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // 首次 show 后的布局调整不要记录，否则布局漂移会逐次膨胀窗口尺寸
    if (m_windowShown)
        m_savedWindowSize = event->size();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    ApplicationRuntimeState::instance().setMainWindowVisible(true);
    if (m_kanban)
        m_kanban->applyMainWindowVisible(true);
    // 延迟到首次布局完成后再启用 resizeEvent 记录，避免构造期布局漂移
    QMetaObject::invokeMethod(this, [this] { m_windowShown = true; }, Qt::QueuedConnection);
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

void MainWindow::uninstallImage()
{
    setLog(QStringLiteral("正在卸载图片背景…"), false);
    QCoreApplication::processEvents();
    QString err;
    Engine::instance().unregisterImageDll(&err);
    Engine::restartExplorer(nullptr);
    refreshStatus();
    setLog(QStringLiteral("图片背景已卸载。"), false);
}

void MainWindow::uninstallEffect()
{
    setLog(QStringLiteral("正在卸载效果样式…"), false);
    QCoreApplication::processEvents();
    QString err;
    Engine::instance().unregisterEffectDll(&err);
    Engine::restartExplorer(nullptr);
    refreshStatus();
    setLog(QStringLiteral("效果样式已卸载。"), false);
}

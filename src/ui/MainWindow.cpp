#include "MainWindow.h"

#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "engine/Engine.h"
#include "ui/TooltipStyle.h"

#include "app/AppInfo.h"
#include "app/ApplicationRuntimeState.h"
#include "app/ApplicationShutdown.h"
#include "core/CachePaths.h"
#include "kanban/KanbanController.h"
#include "kanban/KanbanModelManager.h"
#include "tray/SystemTrayController.h"
#include "wallpaper/VideoWallpaper.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDirIterator>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHideEvent>
#include <QMessageBox>
#include <QScreen>
#include <QShowEvent>
#include <QStyleFactory>
#include <QTimer>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include "platform/windows/desktopmount.h"
#endif


#include "MainWindow.h"

#include "app/AppInfo.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/CachePaths.h"
#include "core/Diagnostics.h"
#include "wallpaper/VideoWallpaper.h"
#include "ui/TooltipStyle.h"
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
void runComboSelfTest(QWidget *window);

} // namespace

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

namespace {

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

} // namespace

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

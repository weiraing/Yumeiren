// MainWindow 核心实现：窗口构造、侧边栏导航、全局状态管理和退出流程。
#include "MainWindow.h"

#include "app/AppInfo.h"
#include "app/ApplicationRuntimeState.h"
#include "app/ApplicationShutdown.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/CachePaths.h"
#include "core/Diagnostics.h"
#include "explorerbg/Engine.h"
#include "kanban/KanbanController.h"
#include "kanban/KanbanModelManager.h"
#include "platform/windows/desktopmount.h"
#include "tray/SystemTrayController.h"
#include "ui/UiStyle.h"
#include "wallpaper/VideoWallpaper.h"
#include "wallpaper/WebWallpaper.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegion>
#include <QScreen>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleHints>
#include <QStyledItemDelegate>
#include <QTextStream>
#include <QThreadPool>
#include <QTreeWidget>
#include <QTimer>
#include <QWindow>

#include <functional>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#endif

#ifdef Q_OS_WIN
#define NOMINMAX

namespace {
// 旧版 MinGW SDK 头文件可能缺失这些定义
constexpr UINT kDwmwaCornerPreference = 33;
constexpr UINT kDwmwcpRound = 2;
// 显示器电源开关通知 GUID。必须与系统里的值逐字节一致：抄错一个字节就是"订阅了一个
// 不存在的设置"—— 注册照样返回成功，通知却永远不来，熄屏判定静默失效。
// 正确值取自 SDK winnt.h 的 DEFINE_GUID(GUID_MONITOR_POWER_ON, 0x02731015, ...)。
// 不用 SDK 的符号而另写一份，是因为它只是 winuser.h 里的 extern 声明，取用要牵进
// libuuid 这条链接依赖。
const GUID kMonitorPowerOnGuid = {0x02731015, 0x4510, 0x4526,
                                  {0x99, 0xE6, 0xE5, 0xA1, 0x7E, 0xBD, 0x1A, 0xEA}};
}
#endif

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(appinfo::windowTitle()); // 原生标题=Yumeiren(窗口行); 品牌名在自绘标题栏
    setMinimumSize(880, 660);
    // 初始窗口几何：优先恢复上次保存的尺寸位置(钳制在可用工作区 94% 内；越界自动拉回
    // 主屏)，无保存记录时用自适应首选 950×820。
    //
    // 950×820 是产品定的首次打开尺寸(2026-09-23)。宽度 950 自始未变，高度走过
    // 836 → 880 → 820 三个值(见 git log)，**以代码里的 kDefaultH 为准**(注释里出现别的数
    // 就是没跟上)。**这里填的是"请求值"，
    // 不是用户看到的窗口外框**：外框恒比它大 15×37(实测两组：900×700→915×737、
    // 950×880→965×917)。多出来的是 Qt 按"有边框窗口"算的 frame —— WS_THICKFRAME 还在，
    // 而 WM_NCCALCSIZE 又把客户区放成了整个窗口矩形，两边口径不一致。要外框正好
    // 950×820 得填 935×783(按上表那个恒定差值算)。**别顺手去"修"这个差**：
    // 它同时参与位置钳制与下次保存的尺寸回写，动它等于把所有历史配置的语义一起改掉。
    //
    // 高度方向的实测(请求值 → 外框，用 tools/winprobe/height_sweep.py 量的)：
    //     950×880 → 965×917：文件夹美化页右卡内空白 69、看板娘页无竖向滚动条
    //     950×836 → 965×873：文件夹美化页右卡内空白 25、**看板娘页冒出竖向滚动条**
    // 也就是说高度再往下走，先顶不住的是看板娘页(它的卡片有固有高度)，不是原先以为的
    // 文件夹美化页 —— 830 那档"整页左移 8 像素"的老结论只在更窄的窗口上成立。
    //
    // ⚠️ 上表是 2026-09-23「左列最小宽」修复**之前**、且在**更窄的窗口(外框 965)** 上量的。
    // 2026-09-23 重跑了一次(外框 989 / 请求宽 974，即当前保存的配置)：
    //     请求 820 → 外框 989×857：**文件夹美化页冒出竖向滚动条**、看板娘页也冒
    //     请求 836 → 外框 989×873：文件夹美化页干净、**看板娘页仍冒**
    //     请求 880 → 外框 989×917：三页全干净
    // 判据：`tmp/_edges3.py` 按 height_sweep 的 HANDLE 色(#d3d6dd) 扫窗口最右 4..18 像素条带，
    // 找连续的 handle 竖条。**别只看 height_sweep 自带的 analyse()** —— 它对看板娘页会失灵
    // (报「右卡底=100、卡内空白=-281」)，以像素扫描/截图为准。
    //
    // 就上表看，**820 比 836 严格更差**：白多出文件夹美化页那条滚动条，什么也没换来。
    // 但 820 是 2026-09-23 用户拍板的默认值，**没经他同意不要改回去**。
    {
        constexpr int kDefaultW = 950;
        constexpr int kDefaultH = 820;
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
        auto &cfg = AppConfig::instance();
        const int cw = cfg.value(ConfigKeys::Window::Width, 0).toInt();
        int ch = cfg.value(ConfigKeys::Window::Height, 0).toInt();

        // 一次性把历史遗留的「超高窗口」收下来。**只做一次**：之后用户把窗口拉高是他
        // 自己的选择，每次开机都按回去会让人以为窗口尺寸存不住。
        if (ch >= 660 && !cfg.value(ConfigKeys::Window::HeightFit, false).toBool()) {
            cfg.setValue(ConfigKeys::Window::HeightFit, true);
            ch = qMin(ch, kDefaultH);
        }

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
            m_savedWindowSize = QSize(kDefaultW, kDefaultH).boundedTo(avail.size() * 0.94);
            resize(m_savedWindowSize);
        }
    }
    // 标题栏自绘方案：保留 WS_THICKFRAME(圆角/阴影/贴边由 DWM 提供)，
    // 通过 WM_NCCALCSIZE 隐藏系统标题栏，WM_NCHITTEST 实现边缘缩放与标题拖动。

    // 立刻恢复上次的视频壁纸：解码器初始化约需 1.5-2s，必须赶在界面构建之前起跑。
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

    // 网页壁纸的启动恢复：与视频壁纸互斥，视频已起跑就让位。两者写同一个
    // 「壁纸在跑」的运行时状态，谁后启动谁说了算。
    if (WebWallpaper::instance().wasRunningLastTime()
        && !VideoWallpaper::instance().isStarted()) {
        QString err;
        if (!WebWallpaper::instance().start(&err) && !err.isEmpty())
            QTimer::singleShot(0, this, [this, err] { setLog(err, true); });
    }

    // Effect presets 取自三个开源项目的默认配置。
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

    auto *folderPage = new QWidget(m_topStack);
    folderPage->setObjectName(QStringLiteral("ContentArea"));
    auto *folderLay = new QVBoxLayout(folderPage);
    folderLay->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget(folderPage);
    m_stack->addWidget(buildImagePage());  // 0
    m_stack->addWidget(buildEffectPage()); // 1
    m_stack->addWidget(buildHelpPage());   // 2
    folderLay->addWidget(m_stack, 1);

    auto *logBar = new QFrame(folderPage);
    logBar->setObjectName(QStringLiteral("Header"));
    auto *logLayout = new QHBoxLayout(logBar);
    logLayout->setContentsMargins(18, 6, 18, 6);
    m_logLabel = new QLabel(imagePageHintText(), logBar);
    m_logLabel->setObjectName(QStringLiteral("LogLabel"));
    m_logLabel->setWordWrap(true);
    logLayout->addWidget(m_logLabel, 1);
    folderLay->addWidget(logBar);
    m_logShowsHint = true;

    m_topStack->addWidget(folderPage);

    m_topStack->addWidget(buildWallpaperPage());

    m_topStack->addWidget(buildKanbanPage());

    rightLayout->addWidget(m_topStack, 1);

    bodyLayout->addWidget(right, 1);
    rootLayout->addWidget(body, 1);
    setCentralWidget(central);

#ifdef Q_OS_WIN
    // 订阅显示器开/关通知：壁纸与看板娘都用它做「看不见就别画」的判据。
    // 注册失败时这条判据会静默退化成「永远不触发」，所以结果必须过目。
    m_powerNotify = RegisterPowerSettingNotification(reinterpret_cast<HWND>(winId()),
                                                    &kMonitorPowerOnGuid,
                                                    DEVICE_NOTIFY_WINDOW_HANDLE);
    videodiag::log(m_powerNotify ? videodiag::Level::Info : videodiag::Level::Warning,
                   m_powerNotify ? QStringLiteral("显示器电源通知：订阅成功")
                                 : QStringLiteral("显示器电源通知：订阅失败(%1)，熄屏自动暂停不可用")
                                       .arg(GetLastError()),
                   QLatin1String("Power"));
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
    // 装在只读目录(如 C:\Program Files)时 .cache 建不起来：明确提示用户，
    // 不静默回退到 AppData。
    QString cacheError;
    if (!CachePaths::isWritable(&cacheError))
        setLog(cacheError, true); // 诊断日志也写在 .cache 内，此时只能走界面日志
    reportDllMigration();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_themeMode == 0)
            applyTheme(0);
    });

    // 看板娘与托盘放在最后装配：控件树、设置回填、视频壁纸恢复都已就位。
    setupKanbanAndTray();
    videodiag::logObjectEvent("create", this,
                              QStringLiteral("size=%1x%2").arg(width()).arg(height()));
}

// 退出闸门：关闭缩略图后台任务的回调通路。锁内翻标志即可返回，不等待任务跑完(退出时
// 卡住比多一次无效投递更糟)；已经通过检查的任务在投递时对象必然仍然存活。
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

    // 左上角那枚小标。20px 是自绘标题栏(高 34)下最合适的一档 —— 再大就把文字挤走，
    // 再小那圈描边会糊掉。用 devicePixelRatioF() 取位图，150% 缩放下不糊；代价是窗口
    // 拖到另一块不同缩放比的屏幕上时不会重取，不值得为它挂一次 screenChanged。
    constexpr int kTitleIconSize = 20;
    auto *mark = new QLabel(m_titleBar);
    mark->setObjectName(QStringLiteral("TitleIcon"));
    mark->setFixedSize(kTitleIconSize, kTitleIconSize);
    mark->setPixmap(appinfo::appIcon().pixmap(QSize(kTitleIconSize, kTitleIconSize),
                                              devicePixelRatioF()));
    lay->addWidget(mark);
    lay->addSpacing(6);

    auto *title = new QLabel(appinfo::displayName(), m_titleBar);
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
    m_nav->addItem(QStringLiteral("📂  文件夹美化"));
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
    // 版本号由构建期决定（见 cmake/Version.cmake），这里只是显示出来。
    m_versionLabel = new QLabel(sideCard);
    m_versionLabel->setObjectName(QStringLiteral("EnvVersionLabel"));
    cardLay->addWidget(m_adminLabel);
    cardLay->addWidget(m_osLabel);
    cardLay->addWidget(m_versionLabel);

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

    // 三组页内页签同一套 HeaderTab 写法，仅标签与选中回调不同 —— 见 makeTabGroup。
    // 第一组默认可见(文件夹美化页)，后两组只在导航选中对应页时显示。
    m_headerTabs = makeTabGroup(lay, {QStringLiteral("图片背景"), QStringLiteral("效果样式"),
                                      QStringLiteral("使用说明")},
                                false, [this](int i) { selectHeaderTab(i); });
    lay->addSpacing(6);
    m_wallTabs = makeTabGroup(lay, {QStringLiteral("视频壁纸"), QStringLiteral("动态网页壁纸")},
                              true, [this](int i) { selectWallTab(i); });
    lay->addSpacing(6);
    m_kanbanTabs = makeTabGroup(lay, {QStringLiteral("看板娘"), QStringLiteral("设置")},
                                true, [this](int i) { selectKanbanTab(i); });
    lay->addStretch(1);

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

    // 动态壁纸页右上角的运行状态徽标：与文件夹美化页的 DLL 注册徽标同一套
    // StatusChip 样式，但反映的是**运行态**(未启动/运行中/暂停)而非注册态。
    // 只在导航选中动态壁纸页时显示(见 switchPage)，与 m_statusBox 互斥出现。
    m_wallStatusBox = new QWidget(header);
    m_wallStatusBox->setVisible(false);
    auto *wbox = new QHBoxLayout(m_wallStatusBox);
    wbox->setContentsMargins(0, 0, 0, 0);
    wbox->setSpacing(4);
    auto *lb3 = new QLabel(QStringLiteral("视频壁纸"), m_wallStatusBox);
    lb3->setObjectName(QStringLiteral("HeaderSub"));
    m_videoChip = new QLabel(m_wallStatusBox);
    m_videoChip->setObjectName(QStringLiteral("StatusChip"));
    auto *lb4 = new QLabel(QStringLiteral("网页壁纸"), m_wallStatusBox);
    lb4->setObjectName(QStringLiteral("HeaderSub"));
    m_webChip = new QLabel(m_wallStatusBox);
    m_webChip->setObjectName(QStringLiteral("StatusChip"));
    wbox->addWidget(lb3);
    wbox->addWidget(m_videoChip);
    wbox->addSpacing(10);
    wbox->addWidget(lb4);
    wbox->addWidget(m_webChip);
    lay->addWidget(m_wallStatusBox);

    // 视频/网页壁纸的启停与挂起都会汇聚到运行态单例(见各自 publishRuntimeState)，
    // 订阅它即可让徽标跟后台状态实时同步，不必逐个接两边的状态信号。
    connect(&ApplicationRuntimeState::instance(), &ApplicationRuntimeState::stateChanged,
            this, &MainWindow::setWallStatusChips);
    return header;
}

// 三组页内页签的构建器：生成可勾选的 HeaderTab 按钮并挂进标题栏布局，按下时把
// 自己的下标转发给 onSelect。hiddenByDefault 的组由 switchPage 在切到对应页时亮出。
QVector<QPushButton *> MainWindow::makeTabGroup(QHBoxLayout *lay, const QStringList &labels,
                                                bool hiddenByDefault,
                                                const std::function<void(int)> &onSelect)
{
    QVector<QPushButton *> tabs;
    for (int i = 0; i < labels.size(); ++i) {
        auto *b = new QPushButton(labels[i], lay->parentWidget());
        b->setObjectName(QStringLiteral("HeaderTab"));
        b->setCheckable(true);
        b->setVisible(!hiddenByDefault);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, i, onSelect] { onSelect(i); });
        tabs.append(b);
        lay->addWidget(b);
    }
    return tabs;
}

// 三组页内页签共用的选中骨架：越界判空、逐个 setChecked、切堆叠页。
// selectHeaderTab 在此之上还有「就绪提示跟随页签」等本页逻辑(见 KanbanSettingsPage.cpp)。
bool MainWindow::selectTabGroup(QVector<QPushButton *> &tabs, QStackedWidget *stack, int index)
{
    if (index < 0 || index >= tabs.size())
        return false;
    for (int i = 0; i < tabs.size(); ++i)
        tabs[i]->setChecked(i == index);
    if (stack)
        stack->setCurrentIndex(index);
    return true;
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
    // 双击视频库的**勾选框**时不要启动壁纸 —— 那条路径的语义是「勾选/取消」，不是「起播」。
    // ⚠️ 必须从事件本身取坐标：信号(itemDoubleClicked)不带位置，而 QCursor::pos() 在探针
    // 用 PostMessage 驱动时也不可靠(合成消息不会真的移动光标)。
    // ⚠️ 也别改成「itemClicked 里看勾选态变没变」——实测双击勾选框时那个时序对不上，
    // 照样会启动(2026-09-23 探针验出来是「不符」)。
    if (event->type() == QEvent::MouseButtonDblClick && m_videoList
        && obj == m_videoList->viewport()) {
        auto *me = static_cast<QMouseEvent *>(event);
        const QPoint p = me->position().toPoint();
        const QModelIndex idx = m_videoList->indexAt(p);
        if (idx.isValid()) {
            QStyleOptionViewItem opt;
            opt.initFrom(m_videoList);
            opt.rect = m_videoList->visualRect(idx);
            opt.features |= QStyleOptionViewItem::HasCheckIndicator;
            const QRect box = m_videoList->style()->subElementRect(
                QStyle::SE_ItemViewItemCheckIndicator, &opt, m_videoList);
            if (box.contains(p))
                return true;   // 吃掉这次双击：勾选框归勾选框
        }
    }
    if (event->type() == QEvent::Resize) {
        if (m_galleryList && obj == m_galleryList->viewport()) {
            updateGalleryGrid(); // 视口宽度变化时重算三列正方形网格
        } else if (m_previewLabel && obj == m_previewLabel
                   && m_previewLabel->size() != m_previewRenderSize) {
            scheduleImagePreview(); // 拖动过程中合并重绘，松手后再按最终尺寸画一次
        } else if (m_imageSourceLabel && obj == m_imageSourceLabel) {
            setImageSourceText(m_sourceText); // 宽度变化后重新按两行省略
        } else if (m_previewFrame && obj == m_previewFrame) {
            updatePreviewAspect(); // 宽度变了就按桌面比例重算预览框高度(内部推迟到事件循环)
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
                // lParam 由系统给出，为空时不能当结构体读
                if (!msg->lParam)
                    return false;
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
            WebWallpaper::instance().setMonitorOn(monitorOn);
            // 同一条判据的第二个消费者：看板娘也是「看不见就别画」的对象，
            // 而它有自己的挂起阈值(见 KanbanController::evaluateSuspend)。
            if (m_kanban) {
                m_kanban->setMonitorOn(monitorOn);
            }
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
            // 注销/关机：系统给的收尾窗口很短，直接走统一收口，绝不隐藏到托盘、
            // 也不弹任何确认框。
            ApplicationShutdown::instance().requestQuit(CloseReason::SystemShutdown);
        }
        if (const unsigned int showMsg = fbswin::showMainWindowMessage();
            showMsg && msg->message == showMsg) {
            // 二次启动的实例请求唤起：转回主线程走托盘「显示主窗口」同一条路。
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

QString MainWindow::imagePageHintText()
{
    return QStringLiteral("就绪。选择预设或自定义图片，然后点击“应用”。");
}

QString MainWindow::effectPageHintText()
{
    // 与图片页那句同构，动词按效果页自己的按钮(「✓ 应用效果样式」)与
    // 该页已有文案(「点击“应用效果”生效」)来写。
    return QStringLiteral("就绪。选择预设或微调参数，然后点击“应用效果”。");
}

void MainWindow::setLogHint(const QString &text)
{
    setLog(text, false);
    // setLog() 会把它清掉，所以放在它后面立回来。
    m_logShowsHint = true;
}

void MainWindow::setLog(const QString &text, bool isError)
{
    // 任何显式消息都意味着「这行不再是一条就绪提示」—— 于是切页签时不会去覆盖它。
    m_logShowsHint = false;
    m_logLabel->setText(text);
    m_logLabel->setProperty("data-err", isError ? 1 : 0);
    uistyle::restyleWidget(m_logLabel);
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
    uistyle::restyleWidget(m_imageChip);
    uistyle::restyleWidget(m_effectChip);

    const bool admin = Engine::isElevated();
    m_adminLabel->setText(admin ? QStringLiteral("● 管理员权限")
                                : QStringLiteral("● 非管理员"));
    m_adminLabel->setProperty("data-admin", admin ? 1 : 0);
    uistyle::restyleWidget(m_adminLabel);
    m_osLabel->setText(Engine::windowsProductName());
    m_versionLabel->setText(QStringLiteral("version ") + appinfo::version());
}

// 动态壁纸页右上角徽标：直接读两个壁纸组件本体，而不是运行态单例的聚合位 ——
// 那个聚合位由视频/网页共用(谁后发布算谁的)，分不出两枚徽标各自该显示什么。
// 状态文案与托盘 tooltip 同一套：未启动 / 运行中 / 暂停；
// 「暂停」涵盖手动暂停与自动挂起(全屏/遮挡/锁屏/熄屏/电池)，与运行态聚合语义一致。
void MainWindow::setWallStatusChips()
{
    if (!m_videoChip || !m_webChip)
        return;
    auto setChip = [](QLabel *chip, const QString &text, bool ok, bool warn) {
        chip->setText(text);
        chip->setProperty("data-ok", ok ? 1 : 0);
        chip->setProperty("data-warn", warn ? 1 : 0);
        uistyle::restyleWidget(chip);
    };
    VideoWallpaper &video = VideoWallpaper::instance();
    const bool vStarted = video.isStarted();
    const bool vPlaying = vStarted && video.isPlaying();
    setChip(m_videoChip, !vStarted ? QStringLiteral("未启动")
                         : vPlaying ? QStringLiteral("运行中")
                                    : QStringLiteral("暂停"),
            vPlaying, vStarted && !vPlaying);
    WebWallpaper &web = WebWallpaper::instance();
    const bool wRunning = web.isRunning();
    const bool wActive = wRunning && !web.isSuspended();
    setChip(m_webChip, !wRunning ? QStringLiteral("未启动")
                       : wActive ? QStringLiteral("运行中")
                                 : QStringLiteral("暂停"),
            wActive, wRunning && !wActive);
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
    // 让用户点一次「应用」就能完成迁移，不自行提权。
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
// through as a black plate. So the frame takes the rounded panel from the stylesheet and
// this helper clips its corners.
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

        uistyle::restyleWidget(frame);
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


void MainWindow::styleCombo(QComboBox *combo) const
{
    if (combo->property("fbsShape").isValid())
        return;
    combo->setProperty("fbsShape", true);
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
    // 透明度：旧「不透明度」键已在 AppConfig 默认值补齐前反算迁移到新键，这里直接读。
    m_transparency->setValue(s.value(ConfigKeys::Image::Transparency, 0).toInt());
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
    // 图片浏览目录：优先恢复用户上次选择的目录，否则默认 软件目录/data/image。
    const QString savedDir = s.value(ConfigKeys::Image::GalleryDir).toString();
    if (!savedDir.isEmpty() && QDir(savedDir).exists())
        m_presetDir = savedDir;
    else
        m_presetDir = QCoreApplication::applicationDirPath() + QStringLiteral("/data/image");
    // 刻意不 mkpath：运行目录的 data/ 由构建期从项目目录复制而来(见 CMakeLists 的
    // POST_BUILD 复制)，程序在这里建目录会和那份复制打架。目录不存在时图库为空，
    // 用户用「选择文件夹」指向别的目录即可。
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
    const QStringList playlist = s.value(ConfigKeys::Video::Playlist).toStringList();
    VideoWallpaper::instance().setPlaylist(playlist);
    refreshVideoList();
    m_videoVolume->setValue(s.value(ConfigKeys::Video::Volume, 0).toInt());
    {
        const int mode = qBound(int(VideoWallpaper::SingleLoop),
                                s.value(ConfigKeys::Video::PlayMode,
                                        VideoWallpaper::SingleLoop).toInt(),
                                int(VideoWallpaper::Random));
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
        // 出厂默认 30 fps(2026-09-23 用户定案，原为 24)。四档 = 四个单选按钮的 id。
        const int saved = s.value(ConfigKeys::Video::TargetFps, 30).toInt();
        int targetFps = saved;
        if (!m_fpsGroup->button(targetFps)) {
            targetFps = snapToFpsOption(saved);   // 为何不吸到 0：见该函数定义处
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("视频帧率：配置里的 %1 已不在可选档位内，吸附到 %2")
                               .arg(saved).arg(targetFps),
                           QLatin1String("UI"));
            s.setValue(ConfigKeys::Video::TargetFps, targetFps);
        }
        if (QAbstractButton *btn = m_fpsGroup->button(targetFps))
            btn->setChecked(true);
        VideoWallpaper::instance().setTargetFps(targetFps);
    }
    m_fpsKeepSpeedBox->setChecked(s.value(ConfigKeys::Video::FpsKeepSpeed, true).toBool());
    VideoWallpaper::instance().setKeepSpeed(m_fpsKeepSpeedBox->isChecked());
    m_reclaimBox->setChecked(s.value(ConfigKeys::Video::Reclaim, true).toBool());
    m_affinityBox->setChecked(s.value(ConfigKeys::Video::AffinityLimit, true).toBool());
    VideoWallpaper::instance().setPauseOnFullscreen(m_fullscreenPauseBox->isChecked());
    VideoWallpaper::instance().setReclaimMemory(m_reclaimBox->isChecked());
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
    s.setValue(ConfigKeys::Image::Transparency, m_transparency->value());
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
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (m_windowShown)
        m_savedWindowSize = event->size();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    ApplicationRuntimeState::instance().setMainWindowVisible(true);
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

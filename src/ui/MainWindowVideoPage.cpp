// MainWindow 动态壁纸页的构建与交互逻辑（含播放列表与播放控制）。
#include "MainWindow.h"

#include "app/ApplicationRuntimeState.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "ui/TooltipStyle.h"
#include "ui/UiMetrics.h" // 左列宽度：与看板娘页共用同一个常量
#include "wallpaper/VideoWallpaper.h"
#include "wallpaper/WebWallpaper.h"

#include <QButtonGroup>
#include <QDesktopServices>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>


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

    auto *leftCard = new QFrame(page);
    leftCard->setObjectName(QStringLiteral("PageCard"));
    // 宽度由下面的 leftCol 统一决定(本卡是唯一子件、列内边距为 0，会自然撑满整列)；
    // 两处各写一个宽度迟早会漂开。
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

    m_playBtn = new QPushButton(QStringLiteral("▶ 启动"), leftCard);
    m_playBtn->setObjectName(QStringLiteral("PrimaryButton"));
    m_playBtn->setMinimumHeight(40);
    m_playBtn->setProperty("data-active", 0);
    connect(m_playBtn, &QPushButton::clicked, this, [this] {
        if (VideoWallpaper::instance().isStarted())
            stopVideo();
        else
            startVideo();
    });
    m_pauseBtn = new QPushButton(QStringLiteral("⏸ 暂停"), leftCard);
    m_pauseBtn->setMinimumHeight(40);
    m_pauseBtn->setEnabled(false);
    connect(m_pauseBtn, &QPushButton::clicked, this, [] {
        VideoWallpaper::instance().pauseResume();
    });
    auto *playRow = new QHBoxLayout();
    playRow->setSpacing(10);
    playRow->addWidget(m_playBtn, 1);
    playRow->addWidget(m_pauseBtn, 1);
    leftLay->addLayout(playRow);

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
            "有应用全屏或完全遮住桌面时暂停视频壁纸(省 GPU/电量)，回到桌面 1 秒内自动恢复")));
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

    // 帧率上限：默认 24 FPS；仅当视频帧率高于上限时生效
    auto *fpsRow = new QHBoxLayout();
    fpsRow->addWidget(new QLabel(QStringLiteral("帧率上限"), leftCard));
    m_fpsBox = new QComboBox(leftCard);
    m_fpsBox->addItems({QStringLiteral("跟随视频"), QStringLiteral("15 FPS"),
                        QStringLiteral("24 FPS"), QStringLiteral("30 FPS"),
                        QStringLiteral("60 FPS")});
    m_fpsBox->setCurrentIndex(2); // 默认 24 FPS
    styleCombo(m_fpsBox);
    m_fpsBox->setToolTip(tooltipstyle::format(QStringLiteral(
            "限制壁纸呈现帧率：视频帧率高于上限时，多出来的帧不再提交呈现。\n"
            "GPU 的 3D 引擎（色彩转换+缩放）占用按呈现帧数线性下降。\n"
            "「跟随视频」保持原生帧率（最费资源）。\n"
            "分辨率高于屏幕的素材会被自动限到 24 FPS，改回「跟随视频」即可取消。")));
    connect(m_fpsBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        static const int fpsValues[] = {0, 15, 24, 30, 60};
        VideoWallpaper::instance().setTargetFps(fpsValues[qBound(0, index, 4)]);
        AppConfig &st = AppConfig::instance();
        st.setValue(ConfigKeys::Video::TargetFps, fpsValues[qBound(0, index, 4)]);
    });
    fpsRow->addWidget(m_fpsBox, 1);
    leftLay->addLayout(fpsRow);

    // 限帧方式(二选一，默认保速丢帧)。两档的省法完全不同，用词必须写清楚。
    m_fpsKeepSpeedBox = new QCheckBox(QStringLiteral("限帧时保持播放速度"), leftCard);
    m_fpsKeepSpeedBox->setChecked(true);
    m_fpsKeepSpeedBox->setToolTip(tooltipstyle::format(QStringLiteral(
            "勾选（默认）：丢掉多余的帧，画面速度与素材一致。解码器照常满速跑，"
            "省不到解码那一段开销 —— 4K60 限 24 时 3D 引擎约降 20%。\n"
            "取消勾选：按上限放慢播放（慢动作），解码器一起减速 —— 同样条件下 "
            "3D 引擎约降 50%，内存/显存同步下降。资源最省的档位，代价是画面明显变慢。")));
    connect(m_fpsKeepSpeedBox, &QCheckBox::toggled, this, [this](bool on) {
        VideoWallpaper::instance().setKeepSpeed(on);
        AppConfig &st = AppConfig::instance();
        st.setValue(ConfigKeys::Video::FpsKeepSpeed, on);
    });
    leftLay->addWidget(m_fpsKeepSpeedBox);

    // 列宽固定、不参与拉伸：窗口变宽时多出来的空间全给右侧播放列表；宽度与看板娘页
    // 共用同一个常量，免得切页时左卡横向跳。卡片高度随内容收缩，空白集中到列尾。
    auto *leftCol = new QWidget(page);
    leftCol->setFixedWidth(uimetrics::kPageLeftColWidth);
    auto *leftColLay = new QVBoxLayout(leftCol);
    leftColLay->setContentsMargins(0, 0, 0, 0);
    leftColLay->setSpacing(18);
    leftColLay->addWidget(leftCard);
    leftColLay->addStretch(1);
    lay->addWidget(leftCol);

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
    addStripBtn(QStringLiteral("↻ 扫描"), "VideoScanButton", [this] { scanVideoDir(); });
    strip->addSpacing(46); // 扫描(发现类)与列表管理三键之间空一个按键距离
    addStripBtn(QStringLiteral("＋ 添加"), "VideoAddButton", [this] { addVideos(); });
    addStripBtn(QStringLiteral("✕ 删除"), "VideoDeleteButton", [this] { removeSelectedVideos(); });
    addStripBtn(QStringLiteral("⌫ 清空"), "VideoClearButton", [this] { clearVideos(); });
    strip->addStretch(1);
    listRow->addLayout(strip);
    rightLay->addLayout(listRow, 1);

    m_videoStatus = new QLabel(QStringLiteral("共 0 个视频 · 停止"), rightCard);
    m_videoStatus->setObjectName(QStringLiteral("HintLabel"));
    rightLay->addWidget(m_videoStatus);

    lay->addWidget(rightCard, 1);

    // 选项改动立即生效
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
        // 看板娘页的同名复选框是同一个注册表项，两处勾选状态保持一致。
        if (m_kanbanAutostartBox && m_kanbanAutostartBox->isChecked() != on) {
            QSignalBlocker blocker(m_kanbanAutostartBox);
            m_kanbanAutostartBox->setChecked(on);
        }
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
    card->setFixedWidth(uimetrics::kPageLeftColWidth);
    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(18, 14, 18, 18);
    cardLay->setSpacing(10);

    auto *t = new QLabel(QStringLiteral("动态网页壁纸"), card);
    t->setObjectName(QStringLiteral("GroupTitle"));
    cardLay->addWidget(t);

    // 运行时体检：WebView2 是系统组件，缺了就把整页置灰并说明装法 —— 与 Live2D
    // 后端缺失同一套「明确告诉你为什么不能用」的处理。
    QString version;
    const bool runtimeOk = WebWallpaper::runtimeAvailable(&version);

    auto *hint = new QLabel(
        runtimeOk
            ? QStringLiteral("把网页(HTML5)挂到桌面当壁纸，浏览器内核直接渲染到桌面层。"
                             "本地页面放在程序目录 data/web 下。运行时 %1。").arg(version)
            : QStringLiteral("WebView2 运行时不可用：%1\n"
                             "请到微软官网安装「Evergreen WebView2 Runtime」后重启软件。").arg(version),
        card);
    hint->setObjectName(QStringLiteral("HintLabel"));
    hint->setWordWrap(true);
    cardLay->addWidget(hint);

    auto *srcRow = new QHBoxLayout();
    m_webSourceEdit = new QLineEdit(card);
    m_webSourceEdit->setPlaceholderText(
        QStringLiteral("网页地址 https://… 或 data/web 下的文件名"));
    m_webSourceEdit->setText(WebWallpaper::instance().source());
    connect(m_webSourceEdit, &QLineEdit::editingFinished, this, [this] {
        // 只记录不启动：地址写进配置，「应用」或下次启动时才生效。
        AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Source),
                                       m_webSourceEdit->text().trimmed());
    });
    srcRow->addWidget(m_webSourceEdit, 1);
    auto *pickBtn = new QPushButton(QStringLiteral("选择页面…"), card);
    connect(pickBtn, &QPushButton::clicked, this, [this] {
        const QString dir = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/data/web");
        const QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择本地网页"), dir,
            QStringLiteral("网页 (*.html *.htm);;所有文件 (*)"));
        if (file.isEmpty())
            return;
        m_webSourceEdit->setText(QDir::toNativeSeparators(file));
        AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Source), file);
    });
    srcRow->addWidget(pickBtn);
    cardLay->addLayout(srcRow);

    // 本地页面库：data/web 下的 *.html 一键直达。目录是用户资产区，构建只补缺。
    auto *openDirBtn = new QPushButton(QStringLiteral("打开 data/web 页面目录"), card);
    connect(openDirBtn, &QPushButton::clicked, this, [] {
        const QString dir = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/data/web");
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    cardLay->addWidget(openDirBtn);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->addWidget(new QLabel(QStringLiteral("刷新策略"), card), 0, 0);
    m_webRefreshCombo = new QComboBox(card);
    m_webRefreshCombo->addItems({QStringLiteral("实时渲染"), QStringLiteral("快照·每分钟"),
                                 QStringLiteral("快照·每小时")});
    m_webRefreshCombo->setCurrentIndex(WebWallpaper::instance().refreshMode());
    styleCombo(m_webRefreshCombo);
    m_webRefreshCombo->setToolTip(tooltipstyle::format(
        QStringLiteral("快照模式：准静态页面(时钟/天气)定时截一张图贴桌面，"
                       "两次刷新之间几乎零 CPU/GPU；实时渲染则让页面持续动画")));
    connect(m_webRefreshCombo, &QComboBox::currentIndexChanged, this, [](int idx) {
        WebWallpaper::instance().setRefreshMode(idx);
    });
    grid->addWidget(m_webRefreshCombo, 0, 1);

    grid->addWidget(new QLabel(QStringLiteral("交互模式"), card), 1, 0);
    m_webInteractCombo = new QComboBox(card);
    m_webInteractCombo->addItems({QStringLiteral("允许鼠标交互"), QStringLiteral("仅展示(穿透点击)")});
    m_webInteractCombo->setCurrentIndex(WebWallpaper::instance().interactive() ? 0 : 1);
    styleCombo(m_webInteractCombo);
    connect(m_webInteractCombo, &QComboBox::currentIndexChanged, this, [](int idx) {
        WebWallpaper::instance().setInteractive(idx == 0);
    });
    grid->addWidget(m_webInteractCombo, 1, 1);

    grid->addWidget(new QLabel(QStringLiteral("帧率上限"), card), 2, 0);
    m_webFpsCombo = new QComboBox(card);
    m_webFpsCombo->addItems({QStringLiteral("跟随页面"), QStringLiteral("24 fps"),
                             QStringLiteral("30 fps"), QStringLiteral("60 fps")});
    const int capVals[4] = {0, 24, 30, 60};
    const int curCap = WebWallpaper::instance().fpsCap();
    int capIdx = 0;
    for (int i = 0; i < 4; ++i)
        if (capVals[i] == curCap)
            capIdx = i;
    m_webFpsCombo->setCurrentIndex(capIdx);
    styleCombo(m_webFpsCombo);
    m_webFpsCombo->setToolTip(tooltipstyle::format(
        QStringLiteral("限制页面脚本的动画帧率(requestAnimationFrame)，页面动效越少越省 GPU/CPU；"
                       "对 CSS 过渡类动画不起作用")));
    connect(m_webFpsCombo, &QComboBox::currentIndexChanged, this, [](int idx) {
        const int vals[4] = {0, 24, 30, 60};
        WebWallpaper::instance().setFpsCap(idx >= 0 && idx < 4 ? vals[idx] : 0);
    });
    grid->addWidget(m_webFpsCombo, 2, 1);

    grid->addWidget(new QLabel(QStringLiteral("音量"), card), 3, 0);
    auto *volRow = new QHBoxLayout();
    m_webVolumeSlider = new QSlider(Qt::Horizontal, card);
    m_webVolumeSlider->setRange(0, 100);
    m_webVolumeSlider->setValue(WebWallpaper::instance().volume());
    m_webVolumeVal = new QLabel(QStringLiteral("%1%").arg(WebWallpaper::instance().volume()), card);
    m_webVolumeVal->setObjectName(QStringLiteral("FieldLabel"));
    m_webVolumeVal->setMinimumWidth(44);
    m_webVolumeVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(m_webVolumeSlider, &QSlider::valueChanged, this, [this](int v) {
        m_webVolumeVal->setText(QStringLiteral("%1%").arg(v));
        WebWallpaper::instance().setVolume(v);
    });
    m_webVolumeSlider->setToolTip(tooltipstyle::format(
        QStringLiteral("WebView2 只能整体静音/取消静音，没有音量级：拉到 0 即静音，"
                       "大于 0 为出声(音量跟随系统)")));
    volRow->addWidget(m_webVolumeSlider, 1);
    volRow->addWidget(m_webVolumeVal);
    grid->addLayout(volRow, 3, 1);

    grid->addWidget(new QLabel(QStringLiteral("页面缩放"), card), 4, 0);
    auto *zoomRow = new QHBoxLayout();
    m_webZoomSlider = new QSlider(Qt::Horizontal, card);
    m_webZoomSlider->setRange(50, 200);
    m_webZoomSlider->setValue(WebWallpaper::instance().zoomPercent());
    m_webZoomVal = new QLabel(QStringLiteral("%1%").arg(WebWallpaper::instance().zoomPercent()), card);
    m_webZoomVal->setObjectName(QStringLiteral("FieldLabel"));
    m_webZoomVal->setMinimumWidth(44);
    m_webZoomVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(m_webZoomSlider, &QSlider::valueChanged, this, [this](int v) {
        m_webZoomVal->setText(QStringLiteral("%1%").arg(v));
        WebWallpaper::instance().setZoomPercent(v);
    });
    zoomRow->addWidget(m_webZoomSlider, 1);
    zoomRow->addWidget(m_webZoomVal);
    grid->addLayout(zoomRow, 4, 1);
    grid->setColumnStretch(1, 1);
    cardLay->addLayout(grid);

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    m_webStartBtn = new QPushButton(QStringLiteral("▶ 应用网页壁纸"), card);
    m_webStartBtn->setObjectName(QStringLiteral("PrimaryButton"));
    m_webStartBtn->setMinimumHeight(40);
    connect(m_webStartBtn, &QPushButton::clicked, this, [this] {
        auto &web = WebWallpaper::instance();
        if (web.isRunning()) {
            web.stop();
        } else {
            QString err;
            if (!web.start(&err, m_webSourceEdit->text().trimmed()) && !err.isEmpty())
                setLog(err, true);
        }
        updateWebWallpaperControls();
    });
    btnRow->addWidget(m_webStartBtn, 1);
    cardLay->addLayout(btnRow);

    m_webStateLabel = new QLabel(WebWallpaper::instance().stateText(), card);
    m_webStateLabel->setObjectName(QStringLiteral("LogLabel"));
    m_webStateLabel->setWordWrap(true);
    cardLay->addWidget(m_webStateLabel);
    connect(&WebWallpaper::instance(), &WebWallpaper::stateChanged, this, [this](const QString &text) {
        if (m_webStateLabel)
            m_webStateLabel->setText(text);
    });
    connect(&WebWallpaper::instance(), &WebWallpaper::runningChanged, this,
            [this](bool) { updateWebWallpaperControls(); });

    if (!runtimeOk) {
        m_webSourceEdit->setDisabled(true);
        m_webStartBtn->setDisabled(true);
        pickBtn->setDisabled(true);
    }
    updateWebWallpaperControls();

    lay->addWidget(card, 0, Qt::AlignTop | Qt::AlignLeft);
    lay->addStretch(1);
    scroll->setWidget(page);
    return scroll;
}

// 网页壁纸页按钮/状态与运行态保持一致(启动后变「停止」)。
void MainWindow::updateWebWallpaperControls()
{
    if (!m_webStartBtn)
        return;
    auto &web = WebWallpaper::instance();
    const bool running = web.isRunning();
    m_webStartBtn->setText(running ? QStringLiteral("■ 停止网页壁纸")
                                   : QStringLiteral("▶ 应用网页壁纸"));
    if (m_webStateLabel)
        m_webStateLabel->setText(web.stateText());
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
void MainWindow::addVideos()
{
    // 同图库目录：data/video 只用于定位，不创建(运行目录的 data 归构建期复制管)。
    const QString videoDir = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/data/video");

    QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择视频文件"), videoDir,
        QStringLiteral("视频文件 (*.mp4 *.webm *.mkv *.avi *.mov *.wmv);;所有文件 (*)"));

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

// 扫描 data/video(含子目录)下的视频，去重后并入播放列表；只增不删。
void MainWindow::scanVideoDir()
{
    const QString videoDir = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/data/video");
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
    // 就地删除不重建列表，计数标签需要单独刷新
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
    updateVideoButtons();
}

void MainWindow::stopVideo()
{
    VideoWallpaper::instance().stopAll();
    AppConfig &st = AppConfig::instance();
    st.setValue(ConfigKeys::Video::WasPlaying, false);
    refreshVideoList();
    setLog(QStringLiteral("视频壁纸已取消。"), false);
    updateVideoButtons();
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

// 正在播放(含暂停/自动挂起)的条目以底色高亮，便于辨别当前曲目。
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
    videodiag::log(videodiag::Level::Debug,
                   QStringLiteral("UI状态 %1").arg(text));
    if (m_videoStatus)
        m_videoStatus->setText(text);
    updateVideoButtons();
    updatePlayingHighlight();

    VideoWallpaper &video = VideoWallpaper::instance();
    ApplicationRuntimeState::instance().setWallpaperState(
        video.isStarted(), video.isStarted() && !video.isPlaying());
}

void MainWindow::updateVideoButtons()
{
    if (!m_playBtn)
        return;
    const bool empty = VideoWallpaper::instance().playlist().isEmpty();
    const bool started = VideoWallpaper::instance().isStarted();

    m_playBtn->setEnabled(!empty || started);
    m_playBtn->setText(started ? QStringLiteral("■ 取消") : QStringLiteral("▶ 启动"));
    m_playBtn->setProperty("data-active", started ? 1 : 0);
    m_playBtn->style()->unpolish(m_playBtn);
    m_playBtn->style()->polish(m_playBtn);
    if (m_pauseBtn) {
        m_pauseBtn->setEnabled(started);
        m_pauseBtn->setText(VideoWallpaper::instance().isManualPaused()
                                ? QStringLiteral("⏸ 继续") : QStringLiteral("⏸ 暂停"));
    }
}
void MainWindow::saveWindowGeometry()
{
    // 用 m_savedWindowSize 而非 width()/height()，防止布局漂移导致窗口尺寸逐次膨胀。
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


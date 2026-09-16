#include "MainWindow.h"

#include "app/ApplicationRuntimeState.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "engine/Engine.h"
#include "ui/TooltipStyle.h"
#include "wallpaper/VideoWallpaper.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardPaths>
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

namespace {

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
void MainWindow::addVideos()
{
    const QString videoDir = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/data/video");
    QDir().mkpath(videoDir);

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

// 扫描软件目录 data/video(含子目录)下的视频文件，去重后并入播放列表。
// 只增不删：不影响现有条目与正在播放的曲目(setPlaylist 按文件名保持当前曲)。
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

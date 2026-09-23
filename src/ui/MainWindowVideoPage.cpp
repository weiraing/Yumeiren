// MainWindow 动态壁纸页的构建与交互逻辑（含播放列表与播放控制）。
#include "MainWindow.h"

#include "app/ApplicationRuntimeState.h"
#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "platform/windows/shellfileops.h"
#include "ui/LibraryCard.h"
#include "ui/TooltipStyle.h"
#include "ui/UiMetrics.h" // 左右列宽度：与看板娘页共用同一个常量
#include "wallpaper/VideoWallpaper.h"
#include "wallpaper/WebWallpaper.h"

#include <QElapsedTimer>
#include <algorithm>
#include <functional>
#include <memory>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kWebSourceRole = Qt::UserRole;
constexpr int kWebKindRole = Qt::UserRole + 1;

enum WebLibraryKind {
    WebLibraryDirectory = 0,
    WebLibraryPage,
    WebLibraryProject,
};

QString webLibraryRoot()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("data/web"));
}

QString findWebEntryDocument(const QDir &dir)
{
    const QFileInfoList files =
        dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &preferred :
         {QStringLiteral("index.html"), QStringLiteral("index.htm")}) {
        for (const QFileInfo &file : files) {
            if (file.fileName().compare(preferred, Qt::CaseInsensitive) == 0)
                return file.absoluteFilePath();
        }
    }
    return QString();
}

QString cleanWebSourceKey(const QString &source)
{
    QString key = source.trimmed();
    if (key.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || key.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        return key.toLower();
    }
    if (key.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) {
        const QUrl url(key);
        if (url.isLocalFile())
            key = url.toLocalFile();
    }
    key = QDir::fromNativeSeparators(key);
    return QDir::cleanPath(key).toLower();
}

// 库根下的顶层条目直接挂树上(parent 为空)，分类内部的条目挂分类行下 ——
// 不再包一层「data/web」根行：根行只是目录本身没有功能，还会在每行左侧
// 顶出一块缩进空位。
void addWebLibraryDirectory(MediaLibraryCard *card, QTreeWidgetItem *parent,
                            const QDir &dir, const QString &relative, bool root,
                            int depth, int *pageCount, int *projectCount)
{
    if (depth > 12)
        return;
    auto addRow = [card, parent](const QString &name, const QString &type,
                                 const QString &path, bool checkable) {
        return card->addRow(parent, name, type, path, checkable);
    };

    const QString entry = findWebEntryDocument(dir);
    if (!entry.isEmpty()) {
        QTreeWidgetItem *project =
            addRow(root ? QStringLiteral("默认 Web 项目") : dir.dirName(),
                   QStringLiteral("Web 项目"), entry, true);
        project->setData(0, kWebSourceRole,
                         root ? QStringLiteral(".") : QDir::fromNativeSeparators(relative));
        project->setData(0, kWebKindRole, int(WebLibraryProject));
        ++(*projectCount);
        if (!root)
            return;
    }

    QTreeWidgetItem *container = parent;
    if (!root)
        container = addRow(dir.dirName(), QStringLiteral("分类"), dir.absolutePath(), false);

    const QFileInfoList childDirs =
        dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &child : childDirs) {
        const QString childRel =
            relative.isEmpty() ? child.fileName()
                               : relative + QLatin1Char('/') + child.fileName();
        addWebLibraryDirectory(card, container, QDir(child.absoluteFilePath()), childRel,
                               false, depth + 1, pageCount, projectCount);
    }

    // 含 index 的目录整体作为 Web 项目：其余 HTML/子目录是项目资源，不重复展开。
    if (!entry.isEmpty())
        return;
    const QFileInfoList files =
        dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &file : files) {
        const QString suffix = file.suffix().toLower();
        if (suffix != QLatin1String("html") && suffix != QLatin1String("htm"))
            continue;
        QTreeWidgetItem *page =
            addRow(file.fileName(), QStringLiteral("页面"), file.absoluteFilePath(), true);
        const QString pageRel =
            relative.isEmpty() ? file.fileName()
                               : relative + QLatin1Char('/') + file.fileName();
        page->setData(0, kWebSourceRole, QDir::fromNativeSeparators(pageRel));
        page->setData(0, kWebKindRole, int(WebLibraryPage));
        ++(*pageCount);
    }

    if (!root && container->childCount() == 0)
        delete container;
}

} // namespace


void MainWindow::refreshWebLibrary()
{
    if (!m_webTree)
        return;
    m_webTree->clear();

    const QString root = webLibraryRoot();
    QDir().mkpath(root);
    QDir rootDir(root);
    int pageCount = 0;
    int projectCount = 0;
    // 不再包「data/web」根行：顶层页面/项目直接从行首开始，勾选框前没有缩进空位
    addWebLibraryDirectory(m_webLib, nullptr, rootDir, QString(), true, 0,
                           &pageCount, &projectCount);
    const QString current = WebWallpaper::instance().source();
    bool matched = false;
    if (!current.isEmpty()) {
        QTreeWidgetItemIterator it(m_webTree);
        while (*it) {
            QTreeWidgetItem *item = *it;
            const int kind = item->data(0, kWebKindRole).toInt();
            if (kind == WebLibraryPage || kind == WebLibraryProject) {
                const QString key = item->data(0, kWebSourceRole).toString();
                if (cleanWebSourceKey(key) == cleanWebSourceKey(current)) {
                    m_webTree->setCurrentItem(item);
                    if (item->parent())
                        item->parent()->setExpanded(true);
                    matched = true;
                    break;
                }
            }
            ++it;
        }
    }
    if (!matched)
        m_webTree->setCurrentItem(nullptr);

    QStringList summary;
    if (pageCount > 0)
        summary << QStringLiteral("%1 个页面").arg(pageCount);
    if (projectCount > 0)
        summary << QStringLiteral("%1 个 Web 项目").arg(projectCount);
    const QString text = summary.isEmpty()
        ? QStringLiteral("data/web 还没有网页。放进 HTML 或整个 Web 项目后点“刷新”即可。")
        : summary.join(QLatin1String(" · "));
    if (m_webLibraryStatus)
        m_webLibraryStatus->setText(text);
}


void MainWindow::setWebSource(const QString &source, bool activate)
{
    QString key = source.trimmed();
    if (!key.isEmpty() && !key.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        && !key.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        const QFileInfo info(key);
        const QDir root(webLibraryRoot());
        if (info.isAbsolute()) {
            const QString rel = root.relativeFilePath(info.absoluteFilePath());
            if (!rel.startsWith(QLatin1String("..")) && !QDir::isAbsolutePath(rel))
                key = QDir::fromNativeSeparators(rel);
        }
    }
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Web::Source), key);
    if (activate) {
        auto &web = WebWallpaper::instance();
        if (web.isRunning()) {
            QString err;
            if (!web.navigateTo(key, &err) && !err.isEmpty()) {
                setLog(err, true);
                if (m_webStateLabel)
                    m_webStateLabel->setText(err);
            }
        } else {
            QString err;
            if (!web.start(&err, key) && !err.isEmpty()) {
                setLog(err, true);
                if (m_webStateLabel)
                    m_webStateLabel->setText(err);
            }
        }
    }
    updateWebWallpaperControls();
}


// 删除网页库里勾选的页面/Web 项目(移入回收站，可撤销)。Web 项目删的是整个
// 项目目录(index.html 所在文件夹，只删 index 会留下孤儿资源，重扫后又以坏
// 项目出现)；库根上的「默认 Web 项目」只删它的 index.html —— data/web 本身
// 是库根，绝不能整目录删掉。
void MainWindow::removeCheckedWebItems()
{
    if (!m_webTree)
        return;
    struct Pending {
        QString path;      // 页面文件 / 项目 index.html 的绝对路径
        bool isProject;
        QString sourceKey; // 配置里记的相对来源(匹配清理用)
        QString label;     // 失败提示用
    };
    QList<Pending> pending;
    QTreeWidgetItemIterator checked(m_webTree, QTreeWidgetItemIterator::Checked);
    while (*checked) {
        QTreeWidgetItem *item = *checked;
        const int kind = item->data(0, kWebKindRole).toInt();
        if (kind == WebLibraryPage || kind == WebLibraryProject) {
            pending.append({item->data(0, MediaLibraryCard::PathRole).toString(),
                            kind == WebLibraryProject,
                            item->data(0, kWebSourceRole).toString(),
                            item->text(0)});
        }
        ++checked;
    }
    if (pending.isEmpty())
        return;

    const QString rootPath = QDir(webLibraryRoot()).canonicalPath();
    int done = 0;
    QStringList failed;
    for (const Pending &p : pending) {
        QFileInfo target(p.path);
        if (p.isProject) {
            const QString projectDir = target.dir().canonicalPath();
            if (projectDir != rootPath)
                target = QFileInfo(projectDir);
        }
        QString err;
        if (fbswin::moveToRecycleBin(target.absoluteFilePath(), &err)) {
            ++done;
        } else {
            failed << QStringLiteral("%1(%2)").arg(p.label, err);
            videodiag::log(videodiag::Level::Warning,
                           QStringLiteral("网页库删除失败: %1 -> %2")
                               .arg(QDir::toNativeSeparators(target.absoluteFilePath()), err),
                           QStringLiteral("WebLibrary"));
        }
    }

    // 删掉的若是当前配置来源，清掉它，避免下次启动撞「本地网页不存在」
    const QString current = WebWallpaper::instance().source();
    if (!current.isEmpty()) {
        for (const Pending &p : pending) {
            if (cleanWebSourceKey(p.sourceKey) == cleanWebSourceKey(current)) {
                AppConfig::instance().remove(QString::fromLatin1(ConfigKeys::Web::Source));
                break;
            }
        }
    }

    refreshWebLibrary(); // 重建树(勾选集随之清空，删除键经 itemChanged 置灰)
    if (done > 0)
        setLog(QStringLiteral("已删除 %1 项(移入回收站，可在回收站还原)。").arg(done), false);
    if (!failed.isEmpty())
        setLog(QStringLiteral("删除失败：%1").arg(failed.join(QStringLiteral("；"))), true);
    updateWebWallpaperControls();
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

    // 左列**不再固定宽度**，而是拿到一个可收缩的下限：窗口变宽时多出来的空间依然
    // 全给右侧播放列表(左列被 stretch 压到最小)，窗口变窄时左列能跟着收。
    //
    // 原来是 setFixedWidth(300)：向右拉伸没问题，但窗口一窄，整页的最小宽度就等于
    // 「左列 300 + 右侧内容宽」且左列一个像素不让，页面很快顶到底、再拖窗口边框
    // 页面纹丝不动 —— 表现出来就是"右侧栏缩不动，内容被边上盖住"。
    // 下限取「左列内容恰好放得下」的宽度：再窄一点左侧的勾选框和下拉框就开始截字。
    auto *leftCol = new QWidget(page);
    leftCol->setMinimumWidth(uimetrics::kPageLeftColWidth);
    auto *leftColLay = new QVBoxLayout(leftCol);
    leftColLay->setContentsMargins(0, 0, 0, 0);
    leftColLay->setSpacing(18);
    leftColLay->addWidget(leftCard);
    leftColLay->addStretch(1);
    lay->addWidget(leftCol);

    // 右侧多包一层容器，卡片在里面吃满 —— 卡片于是与右边界严格对齐，
    // 拖窗口边框时右侧模块宽度跟着变。
    auto *rightCol = new QWidget(page);
    rightCol->setMinimumWidth(uimetrics::kPageRightColMinWidth);
    auto *rightColLay = new QVBoxLayout(rightCol);
    rightColLay->setContentsMargins(0, 0, 0, 0);
    rightColLay->setSpacing(0);

    // 右侧复用统一的媒体库卡片：标题 → 刷新/打开目录/删除 → 双列列表 → 状态 → 注释
    auto *rightCard = new MediaLibraryCard(
        QStringLiteral("data/video 视频库"), QStringLiteral("视频"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/data/video"),
        QStringLiteral("「刷新」以 data/video(含子目录)为准重建视频列表；\n"
                       "「✕ 删除」把勾选的视频文件移入回收站(可还原)。\n"
                       "运行中双击条目：立即切换该视频为壁纸；"
                       "选中条目后点「启动」：从该视频开始播放。"),
        QStringLiteral("视频文件放在程序目录 data/video 下；「✕ 删除」会把勾选的视频移入回收站。"));
    m_videoLib = rightCard;
    m_videoList = rightCard->tree();
    m_videoStatus = rightCard->statusLabel();
    connect(rightCard, &MediaLibraryCard::scanRequested, this,
            [this] { scanVideoDir(); });
    connect(rightCard, &MediaLibraryCard::deleteCheckedRequested, this,
            &MainWindow::removeCheckedVideos);

    // 运行中双击列表条目 → 立即切换该视频为动态壁纸
    connect(m_videoList, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
        auto &vp = VideoWallpaper::instance();
        if (!vp.isStarted() || vp.playlist().isEmpty())
            return; // 未启动时双击仅作选中
        const int row = item ? m_videoList->indexOfTopLevelItem(item) : -1;
        if (row >= 0 && row < vp.playlist().size())
            vp.switchToTrack(row);
    });

    rightColLay->addWidget(rightCard, 1);
    lay->addWidget(rightCol, 1);

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
    // 网页壁纸起停时同步锁死/解锁视频侧的启动按钮(互斥的界面反馈)
    connect(&WebWallpaper::instance(), &WebWallpaper::runningChanged,
            this, [this](bool) { updateVideoButtons(); });

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
    auto *lay = new QHBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(14);

    auto *leftCard = new QFrame(page);
    leftCard->setObjectName(QStringLiteral("PageCard"));
    auto *leftLay = new QVBoxLayout(leftCard);
    leftLay->setContentsMargins(14, 14, 14, 14);
    leftLay->setSpacing(10);

    auto *t = new QLabel(QStringLiteral("动态网页壁纸"), leftCard);
    t->setObjectName(QStringLiteral("GroupTitle"));
    leftLay->addWidget(t);

    // 运行时体检：WebView2 是系统组件，缺了就把整页置灰并说明装法 —— 与 Live2D
    // 后端缺失同一套「明确告诉你为什么不能用」的处理。
    QString version;
    const bool runtimeOk = WebWallpaper::runtimeAvailable(&version);
    m_webRuntimeOk = runtimeOk;

    auto *hint = new QLabel(
        runtimeOk
            ? QStringLiteral("把网页(HTML5)挂到桌面当壁纸，浏览器内核直接渲染到桌面层。"
                             "本地页面放在程序目录 data/web 下。运行时 %1。").arg(version)
            : QStringLiteral("WebView2 运行时不可用：%1\n"
                             "请到微软官网安装「Evergreen WebView2 Runtime」后重启软件。").arg(version),
        leftCard);
    hint->setObjectName(QStringLiteral("HintLabel"));
    hint->setWordWrap(true);
    leftLay->addWidget(hint);

    m_webStartBtn = new QPushButton(QStringLiteral("▶ 启动"), leftCard);
    m_webStartBtn->setObjectName(QStringLiteral("PrimaryButton"));
    m_webStartBtn->setMinimumHeight(40);
    m_webStartBtn->setProperty("data-active", 0);
    connect(m_webStartBtn, &QPushButton::clicked, this, [this] {
        auto &web = WebWallpaper::instance();
        if (web.isRunning()) {
            web.stop();
        } else {
            // 来源由右侧网页库单选/双击写入配置；启动沿用当前配置来源。
            QString err;
            if (!web.start(&err) && !err.isEmpty()) {
                setLog(err, true);
                if (m_webStateLabel)
                    m_webStateLabel->setText(err);
            }
        }
        updateWebWallpaperControls();
    });
    leftLay->addWidget(m_webStartBtn);

    // 来源选择只在右侧「data/web 网页库」里做：单选记忆、双击应用。
    // (旧的地址输入框与「打开目录」按钮已由网页库覆盖，按用户要求移除。)

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->addWidget(new QLabel(QStringLiteral("刷新策略"), leftCard), 0, 0);
    m_webRefreshCombo = new QComboBox(leftCard);
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

    grid->addWidget(new QLabel(QStringLiteral("交互模式"), leftCard), 1, 0);
    m_webInteractCombo = new QComboBox(leftCard);
    m_webInteractCombo->addItems({QStringLiteral("允许鼠标交互"), QStringLiteral("仅展示(穿透点击)")});
    m_webInteractCombo->setCurrentIndex(WebWallpaper::instance().interactive() ? 0 : 1);
    styleCombo(m_webInteractCombo);
    connect(m_webInteractCombo, &QComboBox::currentIndexChanged, this, [](int idx) {
        WebWallpaper::instance().setInteractive(idx == 0);
    });
    grid->addWidget(m_webInteractCombo, 1, 1);

    grid->addWidget(new QLabel(QStringLiteral("帧率上限"), leftCard), 2, 0);
    m_webFpsCombo = new QComboBox(leftCard);
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
        QStringLiteral("默认 24：网页壁纸不需要满屏幕刷新率，页面按 requestAnimationFrame"
                       "驱动的动效(canvas/WebGL)会随之降帧，大幅省 GPU/CPU；"
                       "对 CSS 过渡类动画不起作用")));
    connect(m_webFpsCombo, &QComboBox::currentIndexChanged, this, [](int idx) {
        const int vals[4] = {0, 24, 30, 60};
        WebWallpaper::instance().setFpsCap(idx >= 0 && idx < 4 ? vals[idx] : 0);
    });
    grid->addWidget(m_webFpsCombo, 2, 1);

    grid->addWidget(new QLabel(QStringLiteral("音量"), leftCard), 3, 0);
    auto *volRow = new QHBoxLayout();
    m_webVolumeSlider = new QSlider(Qt::Horizontal, leftCard);
    m_webVolumeSlider->setRange(0, 100);
    m_webVolumeSlider->setValue(WebWallpaper::instance().volume());
    m_webVolumeVal = new QLabel(QStringLiteral("%1%").arg(WebWallpaper::instance().volume()), leftCard);
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

    grid->addWidget(new QLabel(QStringLiteral("页面缩放"), leftCard), 4, 0);
    auto *zoomRow = new QHBoxLayout();
    m_webZoomSlider = new QSlider(Qt::Horizontal, leftCard);
    m_webZoomSlider->setRange(50, 200);
    m_webZoomSlider->setValue(WebWallpaper::instance().zoomPercent());
    m_webZoomVal = new QLabel(QStringLiteral("%1%").arg(WebWallpaper::instance().zoomPercent()), leftCard);
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

    // 全屏自动暂停：默认不勾选(勾选后检测到全屏应用即挂起，退出全屏自动恢复)
    grid->addWidget(new QLabel(QStringLiteral("全屏自动暂停"), leftCard), 5, 0);
    auto *fsBox = new QCheckBox(leftCard);
    fsBox->setChecked(WebWallpaper::instance().pauseOnFullscreen());
    fsBox->setToolTip(tooltipstyle::format(QStringLiteral(
        "检测到全屏应用时自动暂停网页壁纸以省电，\n退出全屏后自动恢复。")));
    connect(fsBox, &QCheckBox::toggled, this,
            [](bool on) { WebWallpaper::instance().setPauseOnFullscreen(on); });
    grid->addWidget(fsBox, 5, 1);

    grid->setColumnStretch(1, 1);
    leftLay->addLayout(grid);

    m_webStateLabel = new QLabel(WebWallpaper::instance().stateText(), leftCard);
    m_webStateLabel->setObjectName(QStringLiteral("LogLabel"));
    m_webStateLabel->setWordWrap(true);
    leftLay->addWidget(m_webStateLabel);
    connect(&WebWallpaper::instance(), &WebWallpaper::stateChanged, this, [this](const QString &text) {
        if (m_webStateLabel)
            m_webStateLabel->setText(text);
    });
    connect(&WebWallpaper::instance(), &WebWallpaper::runningChanged, this,
            [this](bool) { updateWebWallpaperControls(); });

    // 与视频壁纸页同一套：左列给可收缩的下限(不是固定宽)，右侧包容器吃满。
    auto *leftCol = new QWidget(page);
    leftCol->setMinimumWidth(uimetrics::kPageLeftColWidth);
    auto *leftColLay = new QVBoxLayout(leftCol);
    leftColLay->setContentsMargins(0, 0, 0, 0);
    leftColLay->setSpacing(18);
    leftColLay->addWidget(leftCard);
    leftColLay->addStretch(1);
    lay->addWidget(leftCol);

    auto *rightCol = new QWidget(page);
    rightCol->setMinimumWidth(uimetrics::kPageRightColMinWidth);
    auto *rightColLay = new QVBoxLayout(rightCol);
    rightColLay->setContentsMargins(0, 0, 0, 0);
    rightColLay->setSpacing(0);

    // 右侧复用统一的媒体库卡片：标题 → 刷新/打开目录/删除 → 双列列表 → 状态 → 注释
    auto *rightCard = new MediaLibraryCard(
        QStringLiteral("data/web 网页库"), QStringLiteral("页面 / 项目"),
        webLibraryRoot(),
        QStringLiteral("data/web 下每个子文件夹都是一个分类。\n"
                       "分类里的 .html/.htm 会作为页面列出；\n"
                       "包含 index.html 或 index.htm 的文件夹会作为完整 Web 项目列出，"
                       "其余 js/css/图片资源不用手动挑选。"),
        QStringLiteral("目录里还可放嵌套分类；含 index.html/index.htm 的目录自动识别为 Web 项目。"));
    m_webLib = rightCard;
    m_webTree = rightCard->tree();
    m_webLibraryStatus = rightCard->statusLabel();
    m_webLibraryStatus->setText(QStringLiteral("正在刷新 data/web…"));
    m_webLibraryStatus->setWordWrap(true);
    connect(rightCard, &MediaLibraryCard::scanRequested, this,
            [this] { refreshWebLibrary(); });
    connect(rightCard, &MediaLibraryCard::deleteCheckedRequested, this,
            &MainWindow::removeCheckedWebItems);

    // 勾选框点击与「单击记忆来源」的区分：按下(itemPressed)先记录勾选态，
    // 释放(itemClicked)发现状态变了即视为点了勾选框 —— 只影响删除集，不改来源。
    struct PressTrack {
        QTreeWidgetItem *item = nullptr;
        Qt::CheckState state = Qt::Unchecked;
        bool checkToggled = false;
        QElapsedTimer clock;
    };
    const auto press = std::make_shared<PressTrack>();
    connect(m_webTree, &QTreeWidget::itemPressed, this, [press](QTreeWidgetItem *item, int) {
        press->item = item;
        press->state = item ? item->checkState(0) : Qt::Unchecked;
    });
    connect(m_webTree, &QTreeWidget::itemDoubleClicked, this,
            [this, press](QTreeWidgetItem *item, int) {
        if (!item)
            return;
        // 连续双击勾选框时，双击不应触发「应用来源」
        if (press->checkToggled && press->clock.elapsed() < 400)
            return;
        const int kind = item->data(0, kWebKindRole).toInt();
        if (kind != WebLibraryPage && kind != WebLibraryProject)
            return;
        setWebSource(item->data(0, kWebSourceRole).toString(), true);
    });
    connect(m_webTree, &QTreeWidget::itemClicked, this,
            [this, press](QTreeWidgetItem *item, int) {
        if (!item)
            return;
        // 点击的是勾选框(勾选态在按下→释放间变化)：只影响删除集，不改来源
        if (item == press->item && item->checkState(0) != press->state) {
            press->checkToggled = true;
            press->clock.start();
            return;
        }
        press->checkToggled = false;
        const int kind = item->data(0, kWebKindRole).toInt();
        if (kind == WebLibraryPage || kind == WebLibraryProject)
            setWebSource(item->data(0, kWebSourceRole).toString(), false);
    });

    rightColLay->addWidget(rightCard, 1);
    lay->addWidget(rightCol, 1);

    if (!runtimeOk) {
        m_webStartBtn->setDisabled(true);
        m_webLib->setLibraryEnabled(false);
    }
    refreshWebLibrary();
    updateWebWallpaperControls();
    scroll->setWidget(page);
    return scroll;
}

// 网页壁纸页按钮/状态与运行态保持一致：启动键切换「▶ 启动/■ 停止」+ data-active 换色。
void MainWindow::updateWebWallpaperControls()
{
    if (!m_webStartBtn)
        return;
    auto &web = WebWallpaper::instance();
    const bool running = web.isRunning();
    const bool runtimeOk = m_webRuntimeOk;
    // 与视频壁纸互斥的界面侧：视频在跑时应用入口锁死并说明原因。
    const bool videoRunning = VideoWallpaper::instance().isStarted();
    m_webStartBtn->setEnabled(runtimeOk && (!videoRunning || running));
    m_webStartBtn->setToolTip(videoRunning && !running
                                  ? tooltipstyle::format(
                                        QStringLiteral("视频壁纸运行中，两者只能应用一个。\n"

                                                       "先到「视频壁纸」页取消它，再回来应用网页壁纸"))
                                  : QString());
    m_webStartBtn->setText(running ? QStringLiteral("■ 停止") : QStringLiteral("▶ 启动"));
    m_webStartBtn->setProperty("data-active", running ? 1 : 0);
    m_webStartBtn->style()->unpolish(m_webStartBtn);
    m_webStartBtn->style()->polish(m_webStartBtn);
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
// 以 data/video(含子目录)为准重建视频列表 —— 与网页库刷新同语义：列表即目录镜像。
void MainWindow::scanVideoDir()
{
    const QString videoDir = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/data/video");
    QDir().mkpath(videoDir);
    const QStringList nameFilters = {
        QStringLiteral("*.mp4"),  QStringLiteral("*.webm"), QStringLiteral("*.mkv"),
        QStringLiteral("*.avi"),  QStringLiteral("*.mov"),  QStringLiteral("*.wmv")};
    QStringList found;
    QDirIterator it(videoDir, nameFilters, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        found << it.next();
    found.sort();

    VideoWallpaper::instance().setPlaylist(found);
    AppConfig &st = AppConfig::instance();
    st.setValue(ConfigKeys::Video::Playlist, found);
    refreshVideoList();
    setLog(QStringLiteral("刷新完成：data/video 共 %1 个视频").arg(found.size()), false);
}

// 删除勾选的视频：文件移入回收站(可撤销)并同步移出播放列表。若正在播放其中
// 之一，先停止视频壁纸再删，避免文件被占用导致回收失败。
void MainWindow::removeCheckedVideos()
{
    const QStringList paths = m_videoLib->checkedPaths();
    if (paths.isEmpty())
        return;

    auto &vp = VideoWallpaper::instance();
    const QString playingPath =
        (vp.currentIndex() >= 0 && vp.currentIndex() < vp.playlist().size())
            ? vp.playlist().at(vp.currentIndex()) : QString();
    if (vp.isStarted() && paths.contains(playingPath))
        vp.stopAll();

    int done = 0;
    QStringList failed;
    for (const QString &p : paths) {
        QString err;
        if (fbswin::moveToRecycleBin(p, &err))
            ++done;
        else
            failed << QStringLiteral("%1(%2)").arg(QFileInfo(p).fileName(), err);
    }

    QStringList list = vp.playlist();
    for (const QString &p : paths)
        list.removeAll(p);
    vp.setPlaylist(list);
    AppConfig &st = AppConfig::instance();
    st.setValue(ConfigKeys::Video::Playlist, list);
    refreshVideoList();

    if (done > 0)
        setLog(QStringLiteral("已删除 %1 个视频(移入回收站，可在回收站还原)。").arg(done), false);
    if (!failed.isEmpty())
        setLog(QStringLiteral("删除失败：%1").arg(failed.join(QStringLiteral("；"))), true);
    updateVideoButtons();
}

void MainWindow::startVideo()
{
    QString err;
    // 列表中选中了条目时，从选中项开始播放壁纸；未选中则沿用上次进度
    const int selected = (m_videoList && m_videoList->currentItem())
                             ? m_videoList->indexOfTopLevelItem(m_videoList->currentItem())
                             : -1;
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
        QFileInfo fi(f);
        m_videoLib->addRow(nullptr, fi.fileName(), fi.suffix().toUpper(), f, true, true);
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
    for (int i = 0; i < m_videoList->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_videoList->topLevelItem(i);
        const bool playing =
            highlight && item->data(0, MediaLibraryCard::PathRole).toString() == current;
        const QBrush bg = playing ? QBrush(QColor(64, 118, 227, 46)) : QBrush();
        if (item->background(0) != bg || item->background(1) != bg) {
            item->setBackground(0, bg);
            item->setBackground(1, bg);
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
    updateWebWallpaperControls(); // 视频起停时同步网页壁纸侧的应用入口
}

void MainWindow::updateVideoButtons()
{
    if (!m_playBtn)
        return;
    const bool empty = VideoWallpaper::instance().playlist().isEmpty();
    const bool started = VideoWallpaper::instance().isStarted();
    // 两种壁纸互斥：网页壁纸在跑时视频不可启动(底层的自动互斥只是兜底，
    // 界面上直接把入口锁死并说明原因，用户不用猜)。
    const bool webRunning = WebWallpaper::instance().isRunning();

    m_playBtn->setEnabled((!empty || started) && !webRunning);
    m_playBtn->setToolTip(webRunning && !started
                              ? tooltipstyle::format(
                                    QStringLiteral("动态网页壁纸运行中，两者只能应用一个。\n"

                                                   "先到「动态网页壁纸」页停止它，再回来启动视频壁纸"))
                              : QString());
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

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

    // 页脚第一段(统计信息)。**只在这儿算、只存这段**，整行由 updateWebLibraryInfo()
    // 把状态与当前来源名凑齐后交给 card->setInfo()。
    //
    // ⚠️ 段**内**用顿号，段**间**才用 " · "。原来这里 join(" · ")，页脚就成了
    // 「1 个页面 · 14 个 Web 项目 · 未运行 · —」—— 看着是四段，跟「三条」对不上，
    // 分不清统计到哪儿为止(2026-09-23 探针截图里一眼看出来的)。
    // 分隔符一律 QStringLiteral，**不能用 QLatin1String**：· 和 、 都是非 ASCII，
    // 窄字面量会被编成 UTF-8 多字节，再被当 Latin-1 逐字节解 → 界面上显示 "Â·"。
    QStringList summary;
    if (pageCount > 0)
        summary << QStringLiteral("%1 个页面").arg(pageCount);
    if (projectCount > 0)
        summary << QStringLiteral("%1 个 Web 项目").arg(projectCount);
    // 空库时不再在这儿写整句指引(那句太长，会把页脚第一段撑爆)：
    // 「放进 HTML…点刷新」已经挪进 ? 的 tooltip，这里只留短标签。
    m_webStatsText = summary.isEmpty() ? QStringLiteral("还没有网页")
                                       : summary.join(QStringLiteral("、"));
    updateWebLibraryInfo();
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
    // 计量是百分比；0% 不是"音量很小"而是取消声音播放——底层连音频轨一并停掉
    // (见 VideoWallpaper::applyAudioPolicy)。
    auto *volVal = new QLabel(QStringLiteral("0%"), leftCard);
    volVal->setObjectName(QStringLiteral("FieldLabel"));
    volVal->setMinimumWidth(44); // "100%" 四字符，与网页壁纸音量标签同宽
    volVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(m_videoVolume, &QSlider::valueChanged, this, [volVal](int v) {
        volVal->setText(QStringLiteral("%1%").arg(v));
        VideoWallpaper::instance().setVolume(v);
    });
    m_videoVolume->setToolTip(tooltipstyle::format(QStringLiteral(
            "视频壁纸音量(相对系统音量的百分比)：0% 取消声音播放，向右越大声")));
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

    // 帧率：互斥单选四档，按钮 id **就是 fps 值**(0=跟随视频帧率，>0=手动上限)，
    // 于是不需要再维护一张「下标→值」的对照表 —— 旧版是下拉框，下标与值错位
    // (下标 2 才是 24)，回填时得逐项比对，多一档 15 FPS 更容易错。
    //
    // ⚠️ 两个名字容易混：「默认」这一**档** = 跟随视频帧率(值 0，不设上限)；
    // 而**出厂值**是 30 fps(2026-09-23 用户定案，原为 24)。两者不是一回事。
    // ⚠️ 15 FPS 这一档已撤掉；老配置里若存着 15，回填时按「最接近的档位」吸附
    // (MainWindow::loadVideoSettings)。
    auto *fpsRow = new QHBoxLayout();
    fpsRow->setSpacing(6);   // 与上面「模式」单选行同一观感
    fpsRow->addWidget(new QLabel(QStringLiteral("帧率"), leftCard));
    fpsRow->addStretch(1);   // 空白推到中间，四个按钮靠右(与「模式」行对齐)
    m_fpsGroup = new QButtonGroup(this);
    m_fpsGroup->setExclusive(true);
    const QString fpsTip = tooltipstyle::format(QStringLiteral(
            "限制壁纸呈现帧率：视频帧率高于上限时，多出来的帧不再提交呈现。\n"
            "GPU 的 3D 引擎（色彩转换+缩放）占用按呈现帧数线性下降。\n"
            "「默认」跟随视频帧率，保持原生帧率（最费资源）。\n"
            "分辨率高于屏幕的素材会被自动限到 24 FPS，选「默认」即可取消。"));
    auto addFpsOption = [&](int fps, const QString &label) {
        auto *btn = new QRadioButton(label, leftCard);
        btn->setToolTip(fpsTip);
        m_fpsGroup->addButton(btn, fps);
        fpsRow->addWidget(btn);
    };
    // 顺序即用户看到的左右顺序：默认 / 24 / 30 / 60
    addFpsOption(0, QStringLiteral("默认"));
    addFpsOption(24, QStringLiteral("24 fps"));
    addFpsOption(30, QStringLiteral("30 fps"));
    addFpsOption(60, QStringLiteral("60 fps"));
    // 出厂默认档 30 fps。loadVideoSettings() 随后会用配置里的值覆盖它；
    // 这里先选中是为了「配置读取失败/键缺失」时单选组不至于一个都不选中。
    m_fpsGroup->button(30)->setChecked(true);
    // idClicked 只在**用户点击**时发；程序 setChecked 不发 → 回填不会反过来写配置。
    connect(m_fpsGroup, &QButtonGroup::idClicked, this, [](int fps) {
        VideoWallpaper::instance().setTargetFps(fps);
        AppConfig::instance().setValue(ConfigKeys::Video::TargetFps, fps);
    });
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

    // 右侧复用统一的媒体库卡片：标题 → 刷新/打开目录/删除 → 双列列表 → 一行信息。
    // 第 4 个参数是 ? 的 tooltip，第 5 个参数会追加在它后面 —— 常驻说明全在 tooltip 里，
    // 卡片底部只剩一行「统计信息 · 状态 · 播放数据名」(2026-09-23 用户要求)。
    auto *rightCard = new MediaLibraryCard(
        QStringLiteral("data/video 视频库"), QStringLiteral("视频"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/data/video"),
        QStringLiteral("「刷新」以 data/video(含子目录)为准重建视频列表；\n"
                       "「✕ 删除」把勾选的视频文件移入回收站(可还原)。\n"
                       "双击条目：直接用它启动壁纸(已在运行时则立即切换)；"
                       "选中条目后点「启动」：从该视频开始播放。"),
        QStringLiteral("视频文件放在程序目录 data/video 下。"));
    m_videoLib = rightCard;
    m_videoList = rightCard->tree();
    connect(rightCard, &MediaLibraryCard::scanRequested, this,
            [this] { scanVideoDir(); });
    connect(rightCard, &MediaLibraryCard::deleteCheckedRequested, this,
            &MainWindow::removeCheckedVideos);

    // 双击列表条目 → **直接用它启动壁纸**，与网页壁纸库一致(那边双击 = 应用并启动)。
    // 旧版是「未启动时双击仅作选中」，还得再点一次「启动」按钮，两个库的操作不统一。
    //
    // 「双击勾选框不启动」的保护放在 MainWindow::eventFilter 里(吃掉 viewport 的
    // MouseButtonDblClick)。**别改成「itemClicked 时看勾选态变没变」那种写法**：
    // 实测双击勾选框时那个时序对不上，照样会启动(2026-09-23 探针验出来是「不符」)。
    m_videoList->viewport()->installEventFilter(this);
    connect(m_videoList, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
        if (!item)
            return;
        auto &vp = VideoWallpaper::instance();
        const int row = m_videoList->indexOfTopLevelItem(item);
        if (row < 0 || row >= vp.playlist().size())
            return;
        if (!vp.isStarted() || vp.playlist().isEmpty()) {
            // 未启动：把这一行设成当前项再走「启动」按钮那条路 —— startVideo() 内部
            // 就是按 currentItem 的索引起播，这样两边行为完全一致(含写 WasPlaying、
            // 更新按钮态与状态栏文案)。
            m_videoList->setCurrentItem(item);
            startVideo();
            return;
        }
        vp.switchToTrack(row);   // 运行中：立即切换
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
    // 三档互斥单选，按钮 id 直接取 WebWallpaper::RefreshMode 的枚举值
    // (Realtime=0 / SnapshotMinute=1 / SnapshotHour=2)，于是不需要「下标→模式」的对照表。
    m_webRefreshGroup = new QButtonGroup(this);
    m_webRefreshGroup->setExclusive(true);
    auto *refreshRow = new QHBoxLayout();
    refreshRow->setSpacing(6);
    refreshRow->addStretch(1);   // 按钮靠右，与下面「交互模式」「帧率」两行对齐
    const QString refreshTip = tooltipstyle::format(
        QStringLiteral("快照模式：准静态页面(时钟/天气)定时截一张图贴桌面，"
                       "两次刷新之间几乎零 CPU/GPU；实时渲染则让页面持续动画"));
    auto addRefreshOption = [&](int mode, const QString &label) {
        auto *btn = new QRadioButton(label, leftCard);
        btn->setToolTip(refreshTip);
        m_webRefreshGroup->addButton(btn, mode);
        refreshRow->addWidget(btn);
    };
    // 顺序即左右顺序：实时渲染 / 快照·每分钟 / 快照·每小时
    addRefreshOption(WebWallpaper::Realtime, QStringLiteral("实时渲染"));
    addRefreshOption(WebWallpaper::SnapshotMinute, QStringLiteral("快照·每分钟"));
    addRefreshOption(WebWallpaper::SnapshotHour, QStringLiteral("快照·每小时"));
    // idClicked 只在**用户点击**时发；程序 setChecked 不发 → 回填不会反过来写配置。
    connect(m_webRefreshGroup, &QButtonGroup::idClicked, this, [](int mode) {
        WebWallpaper::instance().setRefreshMode(mode);
    });
    {
        // 配置写盘时 AppConfig 会把 web/refreshMode 钳到 0..2，正常不会越界；
        // 但 loadSettings() 是直接 toInt() 读的，手改配置文件仍可能塞进 5 —— 那种情况下
        // button(5) 返回 nullptr，直接解引用就是崩溃。所以这里兜一下底。
        const int saved = WebWallpaper::instance().refreshMode();
        const int mode = m_webRefreshGroup->button(saved) ? saved : int(WebWallpaper::Realtime);
        if (mode != saved) {
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("网页壁纸刷新策略：配置里的 %1 不在可选档位内，回落到实时渲染")
                               .arg(saved),
                           QLatin1String("UI"));
            WebWallpaper::instance().setRefreshMode(mode);
        }
        m_webRefreshGroup->button(mode)->setChecked(true);
    }
    grid->addLayout(refreshRow, 0, 1);

    grid->addWidget(new QLabel(QStringLiteral("交互模式"), leftCard), 1, 0);
    // 两档互斥单选，按钮 id 直接承载语义值：0 = 网页展示(鼠标穿透)、1 = 网页交互。
    // ⚠️ 这两个名字最容易记反：「网页展示」才是**穿透**（只看不摸，鼠标落到桌面），
    // 「网页交互」才吃鼠标。档位名是用户定的(2026-09-23)，别再改回上一版的
    // 「允许鼠标交互 / 仅展示(穿透点击)」—— 那一版还是下拉框，且默认选的是「允许交互」。
    // 出厂默认「网页展示」= false：见 AppConfig 的 kBoolDefaultTrue(该项已移出那张表)。
    m_webInteractGroup = new QButtonGroup(this);
    m_webInteractGroup->setExclusive(true);
    auto *interactRow = new QHBoxLayout();
    interactRow->setSpacing(6);
    interactRow->addStretch(1);   // 按钮靠右，与上面「刷新策略」同一观感
    const QString interactTip = tooltipstyle::format(
        QStringLiteral("网页壁纸挂在桌面图标后面，两种模式都不影响点图标，差别只在桌面空白处：\n"
                       "「网页展示」鼠标穿透，空白处的点击/框选/右键菜单照常落到桌面；\n"
                       "「网页交互」网页接收鼠标，页面里的按钮、链接、滚动才能用。"));
    auto addInteractOption = [&](int id, const QString &label) {
        auto *btn = new QRadioButton(label, leftCard);
        btn->setToolTip(interactTip);
        m_webInteractGroup->addButton(btn, id);
        interactRow->addWidget(btn);
    };
    // 顺序即左右顺序：网页展示 / 网页交互
    addInteractOption(0, QStringLiteral("网页展示"));
    addInteractOption(1, QStringLiteral("网页交互"));
    // idClicked 只在**用户点击**时发；程序 setChecked 不发 → 回填不会反过来写配置。
    connect(m_webInteractGroup, &QButtonGroup::idClicked, this, [](int id) {
        WebWallpaper::instance().setInteractive(id == 1);   // 内部会顺手写配置
    });
    m_webInteractGroup->button(WebWallpaper::instance().interactive() ? 1 : 0)->setChecked(true);
    grid->addLayout(interactRow, 1, 1);

    grid->addWidget(new QLabel(QStringLiteral("帧率"), leftCard), 2, 0);
    // 与视频壁纸页同一套档位与写法(见 buildVideoWallpaperPage 里那段注释)：
    // 按钮 id 就是 fps 值，「默认」= 跟随页面(值 0，不设上限)，出厂默认 30。
    // 页面这边的值取自 WebWallpaper 已加载的设置(不是直接读配置)，与旧版一致。
    m_webFpsGroup = new QButtonGroup(this);
    m_webFpsGroup->setExclusive(true);
    auto *webFpsRow = new QHBoxLayout();
    webFpsRow->setSpacing(6);
    webFpsRow->addStretch(1);   // 按钮靠右，与视频页那一行同一观感
    const QString webFpsTip = tooltipstyle::format(
        QStringLiteral("网页壁纸不需要满屏幕刷新率：页面按 requestAnimationFrame 驱动的动效"
                       "(canvas/WebGL)会随上限降帧，大幅省 GPU/CPU；"
                       "对 CSS 过渡类动画不起作用。\n"
                       "「默认」跟随页面自身帧率(不设上限，最费资源)。"));
    auto addWebFpsOption = [&](int fps, const QString &label) {
        auto *btn = new QRadioButton(label, leftCard);
        btn->setToolTip(webFpsTip);
        m_webFpsGroup->addButton(btn, fps);
        webFpsRow->addWidget(btn);
    };
    // 顺序即左右顺序：默认 / 24 / 30 / 60
    addWebFpsOption(0, QStringLiteral("默认"));
    addWebFpsOption(24, QStringLiteral("24 fps"));
    addWebFpsOption(30, QStringLiteral("30 fps"));
    addWebFpsOption(60, QStringLiteral("60 fps"));
    connect(m_webFpsGroup, &QButtonGroup::idClicked, this, [](int fps) {
        WebWallpaper::instance().setFpsCap(fps);   // 内部会顺手写配置
    });
    {
        const int saved = WebWallpaper::instance().fpsCap();
        int cap = saved;
        if (!m_webFpsGroup->button(cap)) {
            cap = snapToFpsOption(saved);
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("网页壁纸帧率：配置里的 %1 已不在可选档位内，吸附到 %2")
                               .arg(saved).arg(cap),
                           QLatin1String("UI"));
            WebWallpaper::instance().setFpsCap(cap);
        }
        m_webFpsGroup->button(cap)->setChecked(true);
    }
    grid->addLayout(webFpsRow, 2, 1);

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
        QStringLiteral("网页壁纸音量(%)：0% 取消声音播放；WebView2 没有音量级，"
                       "大于 0 即出声(音量跟随系统)")));
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

    grid->setColumnStretch(1, 1);
    leftLay->addLayout(grid);

    m_webStateLabel = new QLabel(WebWallpaper::instance().stateText(), leftCard);
    m_webStateLabel->setObjectName(QStringLiteral("LogLabel"));
    m_webStateLabel->setWordWrap(true);
    leftLay->addWidget(m_webStateLabel);
    connect(&WebWallpaper::instance(), &WebWallpaper::stateChanged, this, [this](const QString &text) {
        if (m_webStateLabel)
            m_webStateLabel->setText(text);
        // 同一个状态也要反映到右栏页脚(第二段) —— 那是两处独立的显示，都要更新。
        updateWebLibraryInfo();
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

    // 右侧复用统一的媒体库卡片：标题 → 刷新/打开目录/删除 → 双列列表 → 一行信息。
    // 与视频页同一个卡片类，页脚也是同一套三段式(统计信息 · 状态 · 当前来源名)。
    auto *rightCard = new MediaLibraryCard(
        QStringLiteral("data/web 网页库"), QStringLiteral("页面 / 项目"),
        webLibraryRoot(),
        QStringLiteral("data/web 下每个子文件夹都是一个分类。\n"
                       "分类里的 .html/.htm 会作为页面列出；\n"
                       "包含 index.html 或 index.htm 的文件夹会作为完整 Web 项目列出，"
                       "其余 js/css/图片资源不用手动挑选。"),
        QStringLiteral("目录里还可放嵌套分类；放进 HTML 或整个 Web 项目后点「刷新」即可。"));
    m_webLib = rightCard;
    m_webTree = rightCard->tree();
    // 初始值：refreshWebLibrary() 在本页构造末尾会立刻跑一遍并覆盖它。
    // 它只是让页脚在刷新完成前不是空的 —— 过渡态只填第一段。
    m_webLib->setInfo(QStringLiteral("正在刷新 data/web…"), QString(), QString());
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

// 网页壁纸页按钮/状态与运行态保持一致：启动键切换「▶ 启动/■ 取消」+ data-active 换色。
// (启动后叫「取消」与视频壁纸键同款措辞，取消的是壁纸不是任务。)
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
    m_webStartBtn->setText(running ? QStringLiteral("■ 取消") : QStringLiteral("▶ 启动"));
    m_webStartBtn->setProperty("data-active", running ? 1 : 0);
    m_webStartBtn->style()->unpolish(m_webStartBtn);
    m_webStartBtn->style()->polish(m_webStartBtn);
    if (m_webStateLabel)
        m_webStateLabel->setText(web.stateText());
    // 起停也会改变页脚第二段(状态)与第三段(来源名)，顺手一起刷新。
    updateWebLibraryInfo();
}

// 网页库页脚那一行：统计信息 · 状态 · 当前来源名。与视频库同一套三段式。
// **唯一写方** —— refreshWebLibrary(第一段变了)与状态/起停变化都走这里。
void MainWindow::updateWebLibraryInfo()
{
    if (!m_webLib)
        return;
    auto &web = WebWallpaper::instance();
    // 来源名只在真的在跑时报：停着时 source() 还留着上次那个键，
    // 直接显示会让人以为它正挂着。
    const QString name = (web.isRunning() && !web.source().isEmpty())
                             ? web.source()
                             : QStringLiteral("—");
    // 未运行时 stateText() 是空串(WebWallpaper 只在 setState 时才写它)，
    // 这时兜一个词，别让状态那格空着。
    const QString state = web.stateText().isEmpty() ? QStringLiteral("未运行") : web.stateText();
    m_webLib->setInfo(m_webStatsText.isEmpty() ? QStringLiteral("正在刷新 data/web…")
                                               : m_webStatsText,
                      state, name);
}

int MainWindow::snapToFpsOption(int saved)
{
    // 把配置里存着的、已不在可选档位里的值(例如老版本的 15 FPS)吸到最近的档位。
    // 视频壁纸页与网页壁纸页的档位集合相同(都是 {0,24,30,60})，所以共用一个。
    //
    // **只在 24/30/60 里挑最近的，不吸到 0**：存着一个数值说明用户要的是「限帧」，
    // 吸到「默认」(不设上限)等于把他最费资源的那档悄悄打开。
    // 而完全不吸附的后果是单选组一个都不选中 —— 界面看着像坏了。
    static const int kCaps[] = {24, 30, 60};
    int best = kCaps[0];
    for (const int cap : kCaps)
        if (qAbs(cap - saved) < qAbs(best - saved))
            best = cap;
    return best;
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
    // 页脚那一行交给唯一入口组装(以前这里自己拼「共 N 个视频 · 播放中/停止」，
    // 而 onVideoStateChanged 又会整行覆盖掉它 —— 同一行两个写方，内容随缘)。
    updateVideoLibraryInfo();
    updateVideoButtons();
    updatePlayingHighlight();
}

// 视频库页脚那一行：统计信息 · 状态 · 播放数据名。
// **唯一写方** —— refreshVideoList / onVideoStateChanged 都走这里，别在别处再 setText。
// 状态用壁纸层给的原文(「检测到全屏应用，已自动暂停」这类"为什么"不能丢)；
// 曲目名只在真的在跑时报，停着的时候 m_index 还留着上次的值，直接显示会让人以为还在播。
void MainWindow::updateVideoLibraryInfo()
{
    if (!m_videoLib)
        return;
    auto &vp = VideoWallpaper::instance();
    const QStringList &pl = vp.playlist();
    const int idx = vp.currentIndex();
    const bool live = vp.isStarted() && idx >= 0 && idx < pl.size();
    // 构造期 m_videoStateText 还是空的(恢复播放的那次 emit 可能早于本页构建)，
    // 这时按运行态兜一个词，别让状态那格空着。
    const QString state = !m_videoStateText.isEmpty()
                              ? m_videoStateText
                              : (vp.isStarted() ? QStringLiteral("播放中")
                                                : QStringLiteral("未启动"));
    m_videoLib->setInfo(QStringLiteral("共 %1 个视频").arg(pl.size()), state,
                        live ? QFileInfo(pl.at(idx)).fileName() : QStringLiteral("—"));
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
    // 只存原文，组装交给 updateVideoLibraryInfo() —— 页脚那行有三段，
    // 光有状态拼不出完整一行。
    m_videoStateText = text;
    updateVideoLibraryInfo();
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

                                                   "先到「动态网页壁纸」页取消它，再回来启动视频壁纸"))
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

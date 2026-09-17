// MainWindow 看板娘页的构建与交互逻辑（含模型选择、参数调节、托盘联动）。
#include "MainWindow.h"

#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "ui/TooltipStyle.h"
#include "ui/UiMetrics.h" // 左列宽度：与动态壁纸页共用同一个常量

#include "app/ApplicationRuntimeState.h"
#include "app/ApplicationShutdown.h"
#include "kanban/KanbanController.h"
#include "kanban/KanbanModelManager.h"
// 视线档位枚举(GazeOff/Weak/Medium/Strong)与它的译名函数。
#include "kanban/KanbanRenderer.h"
#include "tray/SystemTrayController.h"
#include "wallpaper/VideoWallpaper.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QUrl>


QWidget *MainWindow::buildKanbanPage()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget(scroll);
    auto *lay = new QHBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(14);

    // ---- 左：运行控制 + 状态，以及「显示与互动 / 开机与托盘」参数 ----
    //
    // 左列整体定宽、装两张卡片，与动态壁纸页同一套写法（见 MainWindowVideoPage
    // 里那段注释）：列宽固定不参与拉伸，窗口变宽时多出来的空间全给右侧；
    // 卡片高度随内容收缩，空白集中到列尾 —— 所以**卡内不要 addStretch**，
    // 否则卡片会被撑成一大片空框（这正是本次改动前左卡的样子）。
    auto *leftCol = new QWidget(page);
    leftCol->setFixedWidth(uimetrics::kPageLeftColWidth);
    auto *leftColLay = new QVBoxLayout(leftCol);
    leftColLay->setContentsMargins(0, 0, 0, 0);
    leftColLay->setSpacing(18);

    auto *leftCard = new QFrame(leftCol);
    leftCard->setObjectName(QStringLiteral("PageCard"));
    auto *leftLay = new QVBoxLayout(leftCard);
    leftLay->setContentsMargins(14, 14, 14, 14);
    leftLay->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("看板娘"), leftCard);
    title->setObjectName(QStringLiteral("GroupTitle"));
    leftLay->addWidget(title);

    auto *hint = new QLabel(QStringLiteral("桌面上的小人常驻窗口，无边框、不抢焦点，可用鼠标拖动。"),leftCard);
    hint->setObjectName(QStringLiteral("HintLabel"));
    hint->setWordWrap(true);
    leftLay->addWidget(hint);

    m_kanbanStartBtn = new QPushButton(QStringLiteral("▶ 启动"), leftCard);
    m_kanbanStartBtn->setObjectName(QStringLiteral("PrimaryButton"));
    m_kanbanStartBtn->setMinimumHeight(40);
    m_kanbanStartBtn->setProperty("data-active", 0);
    connect(m_kanbanStartBtn, &QPushButton::clicked, this, [this] {
        if (m_kanban->isRunning() || m_kanban->state() == kanban::State::Error) {
            m_kanban->stop();
        } else {
            if (!m_kanban->start())
                setKanbanLog(m_kanban->lastError().isEmpty() ? QStringLiteral("看板娘启动失败")
                                                              : m_kanban->lastError(),
                             true);
            refreshKanbanModels();
        }
        updateKanbanControls();
    });
    m_kanbanPauseBtn = new QPushButton(QStringLiteral("⏸ 暂停"), leftCard);
    m_kanbanPauseBtn->setMinimumHeight(40);
    m_kanbanPauseBtn->setEnabled(false);
    connect(m_kanbanPauseBtn, &QPushButton::clicked, this, [this] {
        m_kanban->pauseResume();
        updateKanbanControls();
    });
    m_kanbanNextBtn = new QPushButton(QStringLiteral("⏭ 播放下一个动作"), leftCard);
    m_kanbanNextBtn->setMinimumHeight(40);
    m_kanbanNextBtn->setEnabled(false);
    connect(m_kanbanNextBtn, &QPushButton::clicked, this, [this] {
        m_kanban->playNext();
        updateKanbanControls();
    });
    m_kanbanExprBtn = new QPushButton(QStringLiteral("☺ 切换表情"), leftCard);
    m_kanbanExprBtn->setMinimumHeight(40);
    m_kanbanExprBtn->setEnabled(false);
    connect(m_kanbanExprBtn, &QPushButton::clicked, this, [this] {
        m_kanban->playNextExpression();
        updateKanbanControls();
    });
    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(m_kanbanStartBtn);
    btnRow->addWidget(m_kanbanPauseBtn);
    leftLay->addLayout(btnRow);
    leftLay->addWidget(m_kanbanNextBtn);
    leftLay->addWidget(m_kanbanExprBtn);
    m_kanbanStatus = new QLabel(QStringLiteral("未启动"), leftCard);
    m_kanbanStatus->setObjectName(QStringLiteral("HintLabel"));
    m_kanbanStatus->setWordWrap(true);
    leftLay->addWidget(m_kanbanStatus);

    m_kanbanLog = new QLabel(QStringLiteral("就绪。"), leftCard);
    m_kanbanLog->setObjectName(QStringLiteral("LogLabel"));
    m_kanbanLog->setWordWrap(true);
    leftLay->addWidget(m_kanbanLog);

    leftColLay->addWidget(leftCard);
    // 「显示与互动 / 开机与托盘」原本在右列，2026-09-17 按用户要求移到左列。
    // 左卡本来就有一大片空底，参数放这里正好填上；右列只剩模型卡。
    leftColLay->addWidget(buildKanbanParamCard(leftCol));
    leftColLay->addStretch(1);
    lay->addWidget(leftCol);

    // ---- 右：模型 ----
    auto *right = new QWidget(page);
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(14);
    rightLay->addWidget(buildKanbanModelCard(right));
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

    auto *refreshBtn = new QPushButton(QStringLiteral("↻ 刷新"), card);
    refreshBtn->setMinimumHeight(32);
    refreshBtn->setToolTip(tooltipstyle::format(
        QStringLiteral("重新扫描模型目录(新增/删除模型后点一下)")));
    connect(refreshBtn, &QPushButton::clicked, this, [this] {
        refreshKanbanModels();
        setKanbanLog(QStringLiteral("已重新扫描模型目录。"), false);
    });
    row->addWidget(refreshBtn);

    auto *openBtn = new QPushButton(QStringLiteral("📂 打开模型目录"), card);
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
        QStringLiteral("把 Cubism 模型整个文件夹放进 <程序目录>\\data\\models，"
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
    //
    // 分两行排：这三项并排约需 275 逻辑像素，而移到左列后卡片内宽只有 ~272
    // （列宽 300 减左右各 14 内边距）—— 硬挤一行会把「允许点击互动」压到省略号。
    // 前两项短、第三项长，所以 2+1 分行最省高度。
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
    auto *checkRow = new QHBoxLayout();
    checkRow->setSpacing(14);
    checkRow->addWidget(m_kanbanTopBox);
    checkRow->addWidget(m_kanbanThroughBox);
    checkRow->addStretch(1);
    lay->addLayout(checkRow);

    m_kanbanInteractBox = new QCheckBox(QStringLiteral("允许点击互动"), card);
    connect(m_kanbanInteractBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setInteractionEnabled(on);
    });
    auto *interactRow = new QHBoxLayout();
    interactRow->setSpacing(14);
    interactRow->addWidget(m_kanbanInteractBox);
    interactRow->addStretch(1);
    lay->addLayout(interactRow);

    // 视线追踪单独占一行，四档互斥。
    //
    // 为什么不是复选框：用户要的是「多明显」这个刻度，而不是「开/关」。做成四个
    // 选项后，「关掉」和「调强弱」合成同一个动作 —— 不会出现「勾着开关却看不出
    // 任何变化」(因为默认档太弱)这种要来回试的困惑。
    auto *gazeRow = new QHBoxLayout();
    gazeRow->setSpacing(10);
    auto *gazeLbl = new QLabel(QStringLiteral("视线追踪"), card);
    gazeRow->addWidget(gazeLbl);
    gazeRow->addSpacing(4);

    auto makeGazeRadio = [this, card, gazeRow](const QString &text, int strength,
                                               const QString &tip) {
        auto *radio = new QRadioButton(text, card);
        radio->setToolTip(tooltipstyle::format(tip));
        gazeRow->addWidget(radio);
        return radio;
    };
    m_kanbanGazeOff = makeGazeRadio(
        QStringLiteral("无"), kanban::KanbanRenderer::GazeOff,
        QStringLiteral("关闭视线追踪，模型保持正面"));
    m_kanbanGazeWeak = makeGazeRadio(
        QStringLiteral("弱"), kanban::KanbanRenderer::GazeWeak,
        QStringLiteral("轻微跟随：鼠标要移开较远才看得出转头，安静不打扰"));
    m_kanbanGazeMedium = makeGazeRadio(
        QStringLiteral("中"), kanban::KanbanRenderer::GazeMedium,
        QStringLiteral("默认档：鼠标在窗口附近移动就能明显看到头眼跟随"));
    m_kanbanGazeStrong = makeGazeRadio(
        QStringLiteral("强"), kanban::KanbanRenderer::GazeStrong,
        QStringLiteral("追得最紧：鼠标稍动即大幅转头，存在感最强"));

    // id 直接用档位值：槽里拿到的就是能交给控制器的数，不用再映射一次。
    m_kanbanGazeGroup = new QButtonGroup(this);
    m_kanbanGazeGroup->setExclusive(true);
    m_kanbanGazeGroup->addButton(m_kanbanGazeOff, kanban::KanbanRenderer::GazeOff);
    m_kanbanGazeGroup->addButton(m_kanbanGazeWeak, kanban::KanbanRenderer::GazeWeak);
    m_kanbanGazeGroup->addButton(m_kanbanGazeMedium, kanban::KanbanRenderer::GazeMedium);
    m_kanbanGazeGroup->addButton(m_kanbanGazeStrong, kanban::KanbanRenderer::GazeStrong);
    connect(m_kanbanGazeGroup, &QButtonGroup::idClicked, this, [this](int strength) {
        // idClicked 只在用户点击时发出(不是回填)，所以这里不必再查 m_kanbanSyncing
        // —— 回填走的是 setChecked()，不触发这个信号。与视频页播放模式同一写法。
        m_kanban->setGazeStrength(strength);
    });
    gazeRow->addStretch(1);
    lay->addLayout(gazeRow);

    auto *sep = new QFrame(card);
    sep->setObjectName(QStringLiteral("SideCardSep"));
    sep->setFrameShape(QFrame::HLine);
    lay->addWidget(sep);

    auto *trayTitle = new QLabel(QStringLiteral("开机与托盘"), card);
    trayTitle->setObjectName(QStringLiteral("GroupTitle"));
    lay->addWidget(trayTitle);

    // 这一节原有四个勾选框，2026-09-17 按用户要求删掉三个，只剩托盘那一条：
    //   「主界面隐藏时暂停动画」—— 开关本身说不通：主界面收进托盘时看板娘还露在
    //     桌面上，把它冻住只会看起来像坏了。`applyMainWindowVisible` 一并撤掉。
    //   「程序启动时自动运行看板娘」—— 换成「记住上次状态」，见 wasRunningLastTime()
    //     与 setupKanbanAndTray() 里的自动拉起；`kanban/autoStart` 键一并撤掉。
    //   「仅在后台任务运行时显示托盘」—— 托盘现在常驻，有没有后台任务都看得见它。
    auto *trayRow = new QHBoxLayout();
    trayRow->setSpacing(14);
    m_trayMinimizeBox = new QCheckBox(QStringLiteral("关闭主窗口时收进托盘(需有后台任务)"), card);
    connect(m_trayMinimizeBox, &QCheckBox::toggled, this, [this](bool on) {
        if (m_kanbanSyncing)
            return;
        AppConfig::instance().setValue(ConfigKeys::Tray::MinimizeToTrayOnClose, on);
    });
    trayRow->addWidget(m_trayMinimizeBox);
    trayRow->addStretch(1);
    lay->addLayout(trayRow);

    return card;
}
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

    // 自动拉起：**记住上次状态**（2026-09-17 按用户要求改）。
    //
    // 判据是 `kanban/enabled` —— 由 publishState() 实时维护、stop()/enterError()
    // 会清掉、而 shutdownForExit() **不碰**，所以它恰好等于「上次退出时在不在跑」。
    // 首次安装没有这个键 → 默认 false → 不启动。
    //
    // 以前读的是设置页那个独立的「程序启动时自动运行看板娘」勾选框，于是出现
    // 「明明关了它，下次启动又自己冒出来」：退出走的是 shutdownForExit()，
    // 它根本不改那个键。勾选框已删除。
    //
    // 等事件循环转起来再拉起，避免在构造函数里 show 一个顶层窗口。
    if (m_kanban->wasRunningLastTime()) {
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
    m_kanban->refreshModels(); // 触发实际扫描，而非仅读缓存
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
    // Stopping 是「正在收口」的中间态：窗口与渲染器正在拆，此时再点一次既没有
    // 可撤销的对象，也会在 stop() 里撞上 Starting->Stopping 之外的非法转移。
    // 所以它是唯一该置灰的「在跑」状态 —— 它只存活一瞬，用户几乎撞不上。
    const bool stopping = m_kanban->state() == kanban::State::Stopping;

    // 这个按钮是**双态开关**：未启动时是「启动」，运行中(或失败待重试)时是
    // 「取消」。两种状态都必须可点，禁用条件只能是「正在收口」这一瞬。
    //
    // 曾经写成 setEnabled(!running || failed)，于是 Running 时 !running=false、
    // failed=false，按钮在变成「■ 取消」的同时被一起禁用 —— 用户看到的就是
    // 「文字变了、颜色变灰、点不动」。enabled 与 text 必须由同一个判据驱动，
    // 不要再各写一套。
    const bool canToggle = !stopping;
    m_kanbanStartBtn->setEnabled(canToggle);
    m_kanbanStartBtn->setText((running || failed) ? QStringLiteral("■ 取消")
                                                   : QStringLiteral("▶ 启动"));
    m_kanbanStartBtn->setProperty("data-active", (running || failed) ? 1 : 0);
    m_kanbanStartBtn->style()->unpolish(m_kanbanStartBtn);
    m_kanbanStartBtn->style()->polish(m_kanbanStartBtn);
    m_kanbanPauseBtn->setEnabled(running);
    m_kanbanPauseBtn->setText(paused ? QStringLiteral("⏸ 继续") : QStringLiteral("⏸ 暂停"));
    // 动作入口：可播动作不足两个就置灰，并把原因写进提示 —— 一个不解释原因的
    // 灰按钮，用户只会当成 bug。实测 13 个模型里有 8 个可播动作不足 2 个，
    // 所以这里灰掉是常态而不是异常，更要把原因说清楚。
    const int motionCount = m_kanban->playableMotionCount();
    const bool canPlayMotion = m_kanban->canPlayNextMotion();
    m_kanbanNextBtn->setEnabled(running && !paused && canPlayMotion);
    m_kanbanNextBtn->setToolTip(canPlayMotion
                                    ? QStringLiteral("当前模型有 %1 个动作，点击逐个切换")
                                          .arg(motionCount)
                                    : (motionCount == 0
                                           ? QStringLiteral("当前模型只有待机动作，没有可播放的动作")
                                           : QStringLiteral("当前模型只有 1 个动作，没有可切换的对象")));
    // 表情入口：当前模型/后端没有表情就置灰，并把原因写进提示 —— 一个不解释
    // 原因的灰按钮，用户只会当成 bug；写清楚「当前模型没有表情文件」，
    // 他就能自己换一个带表情的模型。
    const int exprCount = m_kanban->expressionCount();
    m_kanbanExprBtn->setEnabled(running && !paused && exprCount > 0);
    m_kanbanExprBtn->setToolTip(exprCount > 0
                                    ? QStringLiteral("当前模型有 %1 个表情，点击逐个切换")
                                          .arg(exprCount)
                                    : QStringLiteral("当前模型没有表情文件"));

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
    // 四选一回填：按档位选中对应的那一个。用 setChecked 而不是 group 的
    // checkedId 设置器 —— 逐个 setChecked 时 QButtonGroup 的互斥会自动把
    // 其他三个取消选中，不必自己写「其余置 false」那种容易漏的状态同步。
    switch (kanban::KanbanRenderer::clampGazeStrength(m_kanban->gazeStrength())) {
    case kanban::KanbanRenderer::GazeOff:
        m_kanbanGazeOff->setChecked(true);
        break;
    case kanban::KanbanRenderer::GazeWeak:
        m_kanbanGazeWeak->setChecked(true);
        break;
    case kanban::KanbanRenderer::GazeStrong:
        m_kanbanGazeStrong->setChecked(true);
        break;
    case kanban::KanbanRenderer::GazeMedium:
    default:
        m_kanbanGazeMedium->setChecked(true);
        break;
    }
    // 托盘这项直接读配置：它不归 KanbanController 管，不回填的话
    // 每次重绘都会显示成未勾选，用户以为设置丢了。
    auto &cfg = AppConfig::instance();
    m_trayMinimizeBox->setChecked(
        cfg.value(ConfigKeys::Tray::MinimizeToTrayOnClose, true).toBool());
    m_kanbanSyncing = false;

    if (m_tray)
        m_tray->updateRuntimeState();

    // 诊断探针：把「启动/取消」按钮的可用性与位置写进日志。
    //
    // 起因是一个只靠读代码很难自证的问题：按钮的文字与 enabled 曾经由两套判据
    // 驱动，点击启动后文字变成「■ 取消」却被同时禁用。自动化验证需要能**不问像素**
    // 地拿到「这个按钮此刻可不可点、在哪」，所以在这里如实打印。
    // 只在 YUMEIREN_DIAG=1 时落盘(Debug 级)，正常运行不多写一行。
    if (videodiag::diagEnabled()) {
        const QSize sz = m_kanbanStartBtn->size();
        // 相对整窗的坐标才是可点的：按钮嵌在「卡片 → 滚动区 → 页 → 堆栈」里，
        // geometry() 给的是它在**直接父容器**里的位置(实测恒为 0,0)，
        // mapToGlobal 那时算出来的也是错的。mapTo(this) 逐级换算到主窗口客户区。
        const QPoint inWin = m_kanbanStartBtn->mapTo(this, QPoint(0, 0));
        const QPoint inWinCenter = m_kanbanStartBtn->mapTo(
            this, QPoint(sz.width() / 2, sz.height() / 2));
        videodiag::log(videodiag::Level::Debug,
                       QStringLiteral("[KanbanPage] 启动按钮 enabled=%1 text=%2 "
                                      "尺寸=%3x%4 窗内=%5,%6 窗内中心=%7,%8 "
                                      "可见=%9 状态=%10")
                           .arg(m_kanbanStartBtn->isEnabled() ? 1 : 0)
                           .arg(m_kanbanStartBtn->text())
                           .arg(sz.width()).arg(sz.height())
                           .arg(inWin.x()).arg(inWin.y())
                           .arg(inWinCenter.x()).arg(inWinCenter.y())
                           .arg(m_kanbanStartBtn->isVisible() ? 1 : 0)
                           .arg(m_kanban->stateText()),
                       QStringLiteral("Kanban"));

        // 「暂停/继续」按钮同样打印。
        //
        // 起因：2026-09-17 删掉了 `applyMainWindowVisible()`（主窗口隐藏时冻结看板娘），
        // 那个函数里也调 `m_renderer->pause()/resume()`，于是需要一条能证明
        // **用户自己的暂停仍然好使** 的证据。走的是 `pauseResume()`，与窗口可见性无关。
        //
        // 坐标给**窗内中心**（`mapTo(this, ...)` = 主窗口客户区逻辑像素）：探针往顶层
        // 窗口 PostMessage 时用的就是客户区坐标，直接拿这个值点，不必过 ClientToScreen
        // —— 实测那条路两次运行会差 16 逻辑像素，点整排控件时会偏一格。
        if (m_kanbanPauseBtn) {
            const QSize psz = m_kanbanPauseBtn->size();
            const QPoint pc = m_kanbanPauseBtn->mapTo(
                this, QPoint(psz.width() / 2, psz.height() / 2));
            videodiag::log(videodiag::Level::Debug,
                           QStringLiteral("[KanbanPage] 暂停按钮 enabled=%1 text=%2 "
                                          "窗内中心=%3,%4 状态=%5")
                               .arg(m_kanbanPauseBtn->isEnabled() ? 1 : 0)
                               .arg(m_kanbanPauseBtn->text())
                               .arg(pc.x()).arg(pc.y())
                               .arg(m_kanban->stateText()),
                           QStringLiteral("Kanban"));
        }

        // 视线四档的选中态与可点坐标。
        //
        // 为什么也要打印：四选一是「必须恰好选中一个」的控件，而回填走的是
        // setChecked()、点击走的是 QButtonGroup::idClicked —— 这两条路一旦有一条
        // 接错，表现就是「点不动」或者「显示的和存的不一样」，光看代码很难确认。
        // 打印 checked 状态 + 窗内坐标，就能既核对互斥性、又能直接照着坐标点。
        {
            struct GazeRadio {
                QRadioButton *radio;
                int strength;
            };
            const GazeRadio radios[] = {
                {m_kanbanGazeOff, kanban::KanbanRenderer::GazeOff},
                {m_kanbanGazeWeak, kanban::KanbanRenderer::GazeWeak},
                {m_kanbanGazeMedium, kanban::KanbanRenderer::GazeMedium},
                {m_kanbanGazeStrong, kanban::KanbanRenderer::GazeStrong},
            };
            int checkedCount = 0;
            QStringList parts;
            for (const GazeRadio &g : radios) {
                if (g.radio->isChecked())
                    ++checkedCount;
                const QSize rsz = g.radio->size();
                const QPoint rc = g.radio->mapTo(
                    this, QPoint(rsz.width() / 2, rsz.height() / 2));
                // 同时给出**屏幕坐标**：自动化要照着点，而「窗内坐标」还要经过
                // 客户区原点换算，窗口一移动/多屏/DPI 一变就容易算错。
                // mapToGlobal 是 Qt 自己算的，拿它直接点不会错。
                const QPoint rg = g.radio->mapToGlobal(QPoint(rsz.width() / 2, rsz.height() / 2));
                parts << QStringLiteral("%1[checked=%2 中心=%3,%4 屏幕=%5,%6 %7x%8]")
                             .arg(kanban::KanbanRenderer::gazeStrengthName(g.strength))
                             .arg(g.radio->isChecked() ? 1 : 0)
                             .arg(rc.x()).arg(rc.y())
                             .arg(rg.x()).arg(rg.y())
                             .arg(rsz.width()).arg(rsz.height());
            }
            videodiag::log(videodiag::Level::Debug,
                           QStringLiteral("[KanbanPage] 视线档位=%1(%2) 选中数=%3 可见=%4 | %5")
                               .arg(kanban::KanbanRenderer::gazeStrengthName(
                                        m_kanban->gazeStrength()))
                               .arg(m_kanban->gazeStrength())
                               .arg(checkedCount)
                               .arg(m_kanbanGazeOff->isVisible() ? 1 : 0)
                               .arg(parts.join(QStringLiteral(" "))),
                           QStringLiteral("Kanban"));
        }
    }
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

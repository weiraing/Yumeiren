#include "MainWindow.h"

#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "engine/Engine.h"
#include "ui/TooltipStyle.h"

#include "app/ApplicationRuntimeState.h"
#include "app/ApplicationShutdown.h"
#include "kanban/KanbanController.h"
#include "kanban/KanbanModelManager.h"
#include "tray/SystemTrayController.h"
#include "wallpaper/VideoWallpaper.h"

#include <QApplication>
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
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>


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

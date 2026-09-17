// MainWindow 看板娘页的构建与交互逻辑（含模型选择、参数调节、托盘联动）。
#include "MainWindow.h"

#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "kanban/ModelThumbCache.h"
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
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QUrl>

namespace {

// —— 模型网格的格子尺寸 ——
//
// 格子是**竖的**(宽:高 ≈ 1:1.65)，不是图片浏览页那种正方形：模型预览图是
// 320×480 的竖图，塞进正方形格子只能缩成一小条，「能看清全貌」就无从谈起。
// 宽度 121 时，三列正好落在右列(约 377px 可用宽)里。
constexpr int kModelCellWidth = 121;
constexpr int kModelCellHeight = 201;
constexpr int kModelIconWidth = 105;
constexpr int kModelIconHeight = 157;

class ModelCardDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QIcon icon = opt.icon;
        const QString name = opt.text;
        opt.icon = QIcon();
        opt.text.clear();
        opt.features &= ~(QStyleOptionViewItem::HasDecoration | QStyleOptionViewItem::HasDisplay);
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        painter->save();
        painter->setFont(opt.font);
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        // Fixed image and footer regions keep every name on the same baseline.
        const QRect card = option.rect.adjusted(4, 4, -4, -4);
        const int footerHeight = 28;
        const int dividerY = card.bottom() - footerHeight;
        const QRect imageRect(card.left() + 4, card.top() + 4,
                              card.width() - 8, dividerY - card.top() - 8);
        icon.paint(painter, imageRect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        painter->setPen(QColor(128, 128, 128, 65));
        painter->drawLine(card.left(), dividerY, card.right(), dividerY);

        // Let QSS supply the theme's text color, but draw only the footer label.
        opt.rect = QRect(card.left() + 4, dividerY + 1, card.width() - 8, footerHeight - 1);
        opt.text = opt.fontMetrics.elidedText(name, Qt::ElideRight, opt.rect.width());
        opt.displayAlignment = Qt::AlignCenter;
        style->drawItemText(painter, opt.rect, Qt::AlignCenter, opt.palette,
                            opt.state & QStyle::State_Enabled, opt.text, QPalette::Text);
        painter->restore();
    }
};

// 还没出图的格子用什么占位。
//
// 用**半透明**灰而不是某个具体颜色：这套界面有浅色/深色两套主题，写死任何一种
// 颜色都会在另一套上显得脏。半透明灰在两套底色上都读作「这里是空的」。
QPixmap kanbanThumbPlaceholder(const QSize &size)
{
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(128, 128, 128, 38));
    painter.drawRoundedRect(QRectF(pm.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    painter.setPen(QColor(128, 128, 128, 150));
    painter.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("…"));
    return pm;
}

// 按缓存给一个格子贴图标。缓存里没有就贴占位图。
//
// 抽成自由函数是因为它有两条调用路径：整体重贴(reloadKanbanModelIcons)与
// 单张出图后即时贴(applyKanbanModelThumb)，两处必须给出完全一致的结果。
void applyKanbanThumbIcon(QListWidgetItem *item, const QSize &iconSize)
{
    if (!item)
        return;
    const QString id = item->data(Qt::UserRole + 1).toString();
    const QString path = kanban::ModelThumbCache::pathFor(id);

    // 缩放要按**设备**像素算，不是逻辑像素。
    // iconSize 是逻辑尺寸，本机 150% 缩放意味着屏幕上的图标实际有 158×236 个物理
    // 像素；只缩到 105×157 再交给 QIcon，Qt 还得把它放大 1.5 倍填满，
    // 结果是白糊一道 —— 截图里看得很清楚。缩到物理尺寸并告知 dpr，
    // Qt 就一比一贴上去，不再有二次重采样。
    qreal dpr = 1.0;
    if (QWidget *view = item->listWidget())
        dpr = view->devicePixelRatioF();
    const QSize pixelSize(int(qRound(iconSize.width() * dpr)),
                          int(qRound(iconSize.height() * dpr)));

    QPixmap thumb;
    // 先缩到格子要显示的大小再交给 QIcon：原图是 320×480，16 个模型全按原尺寸
    // 留在内存里要占近 10MB，而格子里只显示 105×157(物理 158×236)。
    // 用户明确要求省资源。
    if (!path.isEmpty() && kanban::ModelThumbCache::has(id) && thumb.load(path)) {
        QPixmap scaled =
            thumb.scaled(pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        item->setIcon(QIcon(scaled));
        item->setData(Qt::UserRole + 2, true);
    } else {
        QPixmap placeholder = kanbanThumbPlaceholder(pixelSize);
        placeholder.setDevicePixelRatio(dpr);
        item->setIcon(QIcon(placeholder));
        item->setData(Qt::UserRole + 2, false);
    }
}

} // namespace


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
    //
    // 与左列相反，这一列唯一的卡片要**占满高度**(addWidget 带 stretch=1、列尾不留
    // stretch)：卡里是一面网格墙，高度不够时该由网格自己滚动，而不是让卡片缩成
    // 几行、下面留一大片空白。左列那种「卡片收缩、空白集中到列尾」的写法在这里
    // 恰好是反的。
    auto *right = new QWidget(page);
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(14);
    rightLay->addWidget(buildKanbanModelCard(right), 1);
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

    auto *refreshBtn = new QPushButton(QStringLiteral("↻ 刷新"), card);
    refreshBtn->setMinimumHeight(32);
    refreshBtn->setToolTip(tooltipstyle::format(
        QStringLiteral("重新扫描模型目录，并重新生成全部预览图\n"
                       "换过贴图或动作的模型靠这个更新封面")));
    connect(refreshBtn, &QPushButton::clicked, this, [this] {
        refreshKanbanModels();
        // 刷新 = 重扫 + **强制**重出图。缓存只按名字找、刻意不做失效判断
        // (见 ModelThumbCache 的说明)，所以「素材换过了」这件事只能由用户
        // 显式点一下来传达。
        ensureKanbanModelThumbs(true);
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
    row->addStretch(1);
    lay->addLayout(row);

    // —— 模型网格：一格一个模型，格子里是它的静态效果图 ——
    //
    // 2026-09-17 按用户要求，由「下拉框选模型」改成图片浏览那样的网格墙：
    // 每个模型一张能看清全貌的静态预览图，不播动作。
    m_kanbanModelGrid = new QListWidget(card);
    auto *grid = m_kanbanModelGrid;
    // 复用图库浏览那套 objectName：格子卡片(圆角/描边/悬停/选中)直接继承
    // 文件夹美化页「图片浏览」的样式，浅色深色两套主题都不必再写一遍 QSS。
    grid->setObjectName(QStringLiteral("GalleryList"));
    grid->setProperty("modelCards", true);
    grid->setItemDelegate(new ModelCardDelegate(grid));
    grid->setViewMode(QListView::IconMode);
    grid->setResizeMode(QListView::Adjust); // 视口变宽就自动多排一列
    grid->setMovement(QListView::Static);
    grid->setFlow(QListView::LeftToRight);
    grid->setWrapping(true);
    grid->setIconSize(QSize(kModelIconWidth, kModelIconHeight));
    grid->setGridSize(QSize(kModelCellWidth, kModelCellHeight));
    grid->setSpacing(0);      // 间隙由 gridSize 与 QSS 的 margin 提供，叠加会挤掉一列
    grid->setWordWrap(false); // 模型名单行，放不下就省略
    grid->setUniformItemSizes(true);
    grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 横向 sizeHint 不参与布局分配：否则会形成「宽度→列数→sizeHint→宽度」的自激环，
    // 拖动窗口边框时界面无限重排直至未响应。与图片浏览页同一处理。
    grid->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    grid->setMinimumHeight(240);
    grid->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(grid, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item, QListWidgetItem *) {
                if (m_kanbanSyncing || !item)
                    return;
                // 路径存在条目里而不是按下标回查：网格顺序由扫描结果决定，
                // 将来一加排序，按下标就会切错模型。
                const QString jsonPath = item->data(Qt::UserRole).toString();
                if (jsonPath.isEmpty() || jsonPath == m_kanban->modelPath())
                    return;
                m_kanban->setModelPath(jsonPath);
                updateKanbanControls();
            });
    lay->addWidget(grid, 1);

    m_kanbanModelInfo = new QLabel(card);
    m_kanbanModelInfo->setObjectName(QStringLiteral("HintLabel"));
    m_kanbanModelInfo->setWordWrap(true);
    lay->addWidget(m_kanbanModelInfo);

    auto *pathHint = new QLabel(
        QStringLiteral("把 Cubism3/4 模型整个文件夹放进 <程序目录>\\data\\models，"
                       "每个模型一个子目录，内含 *.model3.json。\n"),
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

    connect(&kan, &kanban::KanbanController::measuredFpsChanged,
            this, &MainWindow::updateKanbanStatus);
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

// 重扫模型目录并重建模型网格，尽量保住用户当前选中的那一项。
void MainWindow::refreshKanbanModels()
{
    if (!m_kanbanModelGrid)
        return;
    m_kanban->refreshModels(); // 触发实际扫描，而非仅读缓存
    const QString current = m_kanban->modelPath();
    const QVector<kanban::ModelInfo> models = m_kanban->validModelList();

    m_kanbanSyncing = true;
    m_kanbanModelGrid->clear();
    int keepRow = models.isEmpty() ? -1 : 0;
    for (int i = 0; i < models.size(); ++i) {
        const kanban::ModelInfo &model = models.at(i);
        auto *item = new QListWidgetItem(m_kanbanModelGrid);
        item->setText(model.id);
        // UserRole   = .model3.json 绝对路径：点击时直接拿它切模型
        // UserRole+1 = 模型文件夹名：预览图的文件名就是它
        // UserRole+2 = 该格子的图是不是真预览图(占位图时为 false)，用于 tooltip
        item->setData(Qt::UserRole, model.modelJsonPath);
        item->setData(Qt::UserRole + 1, model.id);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setSizeHint(m_kanbanModelGrid->gridSize());
        if (!current.isEmpty() && model.modelJsonPath == current)
            keepRow = i;
    }
    m_kanbanSyncing = false;

    if (keepRow >= 0)
        m_kanbanModelGrid->setCurrentRow(keepRow);

    reloadKanbanModelIcons();
}

// 按当前缓存重贴全部格子图标。
// 缺图的贴占位图 —— 生成是异步的，每张图落地后会再叫一次 applyKanbanModelThumb。
void MainWindow::reloadKanbanModelIcons()
{
    if (!m_kanbanModelGrid)
        return;
    const QSize iconSize = m_kanbanModelGrid->iconSize();
    for (int i = 0; i < m_kanbanModelGrid->count(); ++i) {
        QListWidgetItem *item = m_kanbanModelGrid->item(i);
        applyKanbanThumbIcon(item, iconSize);
        if (!item)
            continue;
        const bool real = item->data(Qt::UserRole + 2).toBool();
        item->setToolTip(real
                             ? tooltipstyle::format(
                                   QStringLiteral("点击把看板娘换成「%1」").arg(item->text()))
                             : tooltipstyle::format(
                                   QStringLiteral("「%1」的预览图还没生成好\n"
                                                  "生成在后台进行，完成后会自动出现")
                                       .arg(item->text())));
    }
}

// 单个模型出图后即时贴图：不必整面墙重贴，用户能看着格子一个个亮起来。
void MainWindow::applyKanbanModelThumb(const QString &modelId)
{
    if (!m_kanbanModelGrid || modelId.isEmpty())
        return;
    const QSize iconSize = m_kanbanModelGrid->iconSize();
    for (int i = 0; i < m_kanbanModelGrid->count(); ++i) {
        QListWidgetItem *item = m_kanbanModelGrid->item(i);
        if (!item || item->data(Qt::UserRole + 1).toString() != modelId)
            continue;
        applyKanbanThumbIcon(item, iconSize);
        item->setToolTip(tooltipstyle::format(
            QStringLiteral("点击把看板娘换成「%1」").arg(item->text())));
        return;
    }
}

// 发现缺图就起一个生成进程。
//
// 整条链路只有「本程序再起一个自己」这一步，没有线程、没有共享 GL 上下文 ——
// 原因见 src/kanban/ModelThumbJob.h(Cubism 的着色器缓存是进程级单例，
// 在第二个上下文里渲染要么出空图、要么把桌面上正在跑的看板娘搞黑)。
void MainWindow::ensureKanbanModelThumbs(bool force)
{
    if (!m_kanbanModelGrid)
        return;
    // m_kanbanThumbJob 非空即「正在生成」，这是唯一的重入闸门。
    if (m_kanbanThumbJob)
        return;
    // 没有 Live2D 后端就没有离屏渲染，起了也是白起(它只会回一句「本构建无 Live2D」)。
    if (!m_kanban->live2dAvailable())
        return;

    // 先算清楚缺几张。全都在缓存里就什么都不做 —— 这是绝大多数启动的情形，
    // 也是「省资源」的关键：只有第一次进这个页面才真的会跑渲染。
    QStringList missing;
    for (int i = 0; i < m_kanbanModelGrid->count(); ++i) {
        const QListWidgetItem *item = m_kanbanModelGrid->item(i);
        if (!item)
            continue;
        const QString id = item->data(Qt::UserRole + 1).toString();
        if (id.isEmpty())
            continue;
        if (force || !kanban::ModelThumbCache::has(id))
            missing << id;
    }
    if (missing.isEmpty())
        return;

    // 走到这里说明确实要出图了，也说明此刻没有别的生成进程在跑(上面那道闸门)。
    // 顺手清掉上次被杀留下的 .tmp —— 用户被告知过这个目录在哪，别让他看到垃圾。
    kanban::ModelThumbCache::sweepTempFiles();

    auto *job = new QProcess(this);
    QStringList args;
    args << QStringLiteral("--render-model-thumbs");
    if (force)
        args << QStringLiteral("--force");
    // 用本程序自己的 exe：渲染代码与应用绝对同版本，不必额外发布一个生成器，
    // 也不用担心它跟主程序版本漂移。正式版是 requireAdministrator 清单，
    // 但父进程已经提权，子进程直接继承令牌，不会弹 UAC。
    job->setProgram(QCoreApplication::applicationFilePath());
    job->setArguments(args);

    m_kanbanThumbJob = job;

    // 记下基线：任务开始前每张图是什么样。之后「修改时间变了」= 这张图刚生成好。
    // 这个哈希同时充当「还没出图的待办清单」—— 谁完成就从里面摘掉。
    m_kanbanThumbBaseline.clear();
    for (const QString &id : missing) {
        const QString path = kanban::ModelThumbCache::pathFor(id);
        m_kanbanThumbBaseline.insert(
            id, path.isEmpty() ? 0 : QFileInfo(path).lastModified().toMSecsSinceEpoch());
    }
    m_kanbanThumbTotal = missing.size();

    connect(job, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onKanbanThumbFinished(exitCode); });
    connect(job, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // 只有「压根没起来」需要在这里收尾 —— FailedToStart 不会再有 finished 信号。
        // 其余错误(崩溃等)随后一定有 finished，交给它统一收口。
        if (error != QProcess::FailedToStart)
            return;
        const QString reason = m_kanbanThumbJob ? m_kanbanThumbJob->errorString() : QString();
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("预览图生成进程启动失败: %1").arg(reason),
                       QStringLiteral("Kanban"));
        onKanbanThumbFinished(-1);
    });

    // 边生成边贴：每 250ms 看一眼缓存目录，谁先落盘谁先亮。
    // 不读子进程的 stdout —— 见 m_kanbanThumbPoll 的说明。
    if (!m_kanbanThumbPoll) {
        m_kanbanThumbPoll = new QTimer(this);
        m_kanbanThumbPoll->setInterval(250);
        connect(m_kanbanThumbPoll, &QTimer::timeout, this, &MainWindow::pollKanbanModelThumbs);
    }
    m_kanbanThumbPoll->start();

    setKanbanLog(QStringLiteral("正在生成 %1 个模型的预览图…(后台进行，不影响使用)")
                     .arg(missing.size()),
                 false);
    job->start();
}

// 任务期间的轮询：已经落盘的图先贴上去。
//
// 判据是「文件修改时间与基线不同」，而不是「文件存在」—— 强制重建时文件本来
// 就在，只有修改时间能说明它刚被重写过。
void MainWindow::pollKanbanModelThumbs()
{
    if (m_kanbanThumbBaseline.isEmpty())
        return;
    for (auto it = m_kanbanThumbBaseline.begin(); it != m_kanbanThumbBaseline.end();) {
        const QString path = kanban::ModelThumbCache::pathFor(it.key());
        if (path.isEmpty()) {
            ++it;
            continue;
        }
        const QFileInfo info(path);
        if (!info.exists() || info.size() <= 0) {
            ++it;
            continue;
        }
        const qint64 stamp = info.lastModified().toMSecsSinceEpoch();
        if (stamp == it.value()) {
            ++it;
            continue; // 还是任务开始前那一张，没被重写
        }
        // erase 已返回下一项，不能再 ++it，否则会跳项，甚至递增 end() 导致崩溃。
        applyKanbanModelThumb(it.key());
        it = m_kanbanThumbBaseline.erase(it);
    }
}

void MainWindow::onKanbanThumbFinished(int exitCode)
{
    if (!m_kanbanThumbJob)
        return;

    if (m_kanbanThumbPoll)
        m_kanbanThumbPoll->stop();

    QProcess *job = m_kanbanThumbJob;
    m_kanbanThumbJob = nullptr;
    job->deleteLater();

    // 收尾时再轮询一次：进程退出与上一次定时器触发之间落盘的那几张，别漏掉。
    pollKanbanModelThumbs();

    // 剩下的就是没出图的。这里不去解析子进程的标准输出 —— 完成与否只看磁盘，
    // 因为文件一定写得出来，而 GUI 子系统进程的 stdout 不一定(见 report 的说明)。
    const int failed = int(m_kanbanThumbBaseline.size());
    const int ok = m_kanbanThumbTotal - failed;
    m_kanbanThumbBaseline.clear();
    m_kanbanThumbTotal = 0;

    // 整面重贴一遍兜底：万一某个文件的修改时间恰好与基线相同(同一毫秒内被
    // 重写)，轮询会漏掉它，而这里按「有没有图」重贴一定对。
    reloadKanbanModelIcons();

    if (failed > 0) {
        setKanbanLog(QStringLiteral("预览图生成完成：成功 %1 张，%2 个模型没能出图，详见日志。")
                         .arg(ok)
                         .arg(failed),
                     true);
    } else if (exitCode < 0) {
        setKanbanLog(QStringLiteral("预览图生成进程没能启动，详见 .cache/logs。"), true);
    } else if (exitCode != 0) {
        setKanbanLog(QStringLiteral("预览图生成进程异常退出(码 %1)，详见 .cache/logs。")
                         .arg(exitCode),
                     true);
    } else {
        setKanbanLog(QStringLiteral("预览图已就绪(本次生成 %1 张)。").arg(ok), false);
    }
}

void MainWindow::updateKanbanStatus()
{
    if (!m_kanban || !m_kanbanStatus)
        return;

    QString status = QStringLiteral("状态：%1").arg(m_kanban->stateText());
    if (m_kanban->isRunning())
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
}

// 同步按钮、模型选中项与设置控件；帧率通知不触发整页回填。
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

    updateKanbanStatus();

    // 模型明细：把校验结果如实摊开，比「能不能用」四个字有用得多。
    //
    // 顺带把网格的选中态同步过来。放在这里回填(而不是只在点击时设)，是为了让
    // 「控制器里的当前模型」与「网格上高亮的那一格」不可能长期不一致 ——
    // 托盘菜单、右键菜单、自动拉起都会换模型，它们都不经过网格。
    const QVector<kanban::ModelInfo> models = m_kanban->validModelList();
    QString info;
    int selectedRow = -1;
    if (models.isEmpty()) {
        info = QStringLiteral("可用模型 0 个。");
    } else {
        const bool wasSyncing = m_kanbanSyncing;
        m_kanbanSyncing = true;
        if (m_kanbanModelGrid) {
            const QString currentPath = m_kanban->modelPath();
            for (int i = 0; i < m_kanbanModelGrid->count(); ++i) {
                const QListWidgetItem *item = m_kanbanModelGrid->item(i);
                if (item && item->data(Qt::UserRole).toString() == currentPath) {
                    selectedRow = i;
                    break;
                }
            }
            // 当前模型不在列表里(被删掉/被换掉)时高亮第一个，与改版前下拉框的
            // 兜底行为一致 —— 空着不高亮会让用户以为「一个模型都没有」。
            if (selectedRow < 0)
                selectedRow = 0;
            m_kanbanModelGrid->setCurrentRow(selectedRow);
        }
        m_kanbanSyncing = wasSyncing;

        const int index = (selectedRow >= 0 && selectedRow < models.size()) ? selectedRow : 0;
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
    else if (row == 2) {
        updateKanbanControls();
        // 预览图**按需生成**：切到这一页才去补缺的图，而不是程序一启动就把
        // 十几个模型全渲染一遍。缓存齐全时这一句什么都不做(见 ensureKanbanModelThumbs)。
        ensureKanbanModelThumbs();
    }
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

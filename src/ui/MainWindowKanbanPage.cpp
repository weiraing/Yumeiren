// MainWindow 看板娘页的构建与交互逻辑（含模型选择、参数调节、托盘联动）。
#include "MainWindow.h"

#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "kanban/ModelThumbCache.h"
#include "platform/windows/shellfileops.h"
#include "ui/TooltipStyle.h"
#include "ui/UiMetrics.h" // 左列宽度：与动态壁纸页共用同一个常量

#include "app/ApplicationRuntimeState.h"
#include "app/ApplicationShutdown.h"
#include "app/AppInfo.h"
#include "kanban/KanbanController.h"
#include "kanban/KanbanModelManager.h"
#include "kanban/KanbanRenderer.h"
#include "platform/windows/globalhotkey.h"
#include "tray/SystemTrayController.h"
#include "wallpaper/VideoWallpaper.h"

#include <QApplication>
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
// QDir 是给 QDir::toNativeSeparators 用的，别当成遗留 include 删掉。
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSlider>
#include <QKeySequenceEdit>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>

namespace {

// 格子取竖形(≈1:1.65)以适配 320×480 的预览图；宽 121 让三列正好落在右列(~377px)。
constexpr int kModelCellWidth = 121;
constexpr int kModelCellHeight = 201;
constexpr int kModelIconWidth = 105;
constexpr int kModelIconHeight = 157;

// 问号徽标边长。QSS #HelpBadge 的 border-radius 取一半即正圆，改这里要同步改 QSS。
constexpr int kHelpBadgeSize = 22;

// 「按清单隐藏网格」的提示词。末尾动态补一段「当前模型的清单情况」：这个复选框只管总开关，
// 用户勾着却什么都没变时，得能自己看出是「这个模型没有清单」而不是「功能坏了」。
QString meshHideTooltip(const QString &summary)
{
    QString text = QStringLiteral(
        "默认开启：装载模型时读取模型目录里的 *.hidden.json，把清单中列出的部件与网格隐藏掉。\n"
        "清单由模型查看器导出，原样放进模型目录即可；改完清单要重新装载模型"
        "（重开看板娘，或换个模型再换回来）。\n"
        "取消勾选则完全照模型原样显示，不改动任何文件。");
    text += summary.isEmpty()
                ? QStringLiteral("\n当前模型目录里没有 *.hidden.json，这个开关对它没有影响。")
                : QStringLiteral("\n当前模型：%1").arg(summary);
    return text;
}

QString kanbanValueColor(const QWidget *widget)
{
    const QPalette palette = widget ? widget->palette() : QApplication::palette();
    return palette.color(QPalette::WindowText).lightness() < 128
               ? QStringLiteral("#2f6fd6")
               : QStringLiteral("#7db1ff");
}

QString kanbanValueHtml(const QString &value, const QString &color)
{
    return QStringLiteral("<span style=\"color:%1;font-weight:600\">%2</span>")
        .arg(color, value.toHtmlEscaped());
}

QString kanbanPairHtml(const QString &label, const QString &value, const QString &color)
{
    return QStringLiteral("%1：%2")
        .arg(label.toHtmlEscaped(), kanbanValueHtml(value, color));
}

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

        const QRect card = option.rect.adjusted(4, 4, -4, -4);
        const int footerHeight = 28;
        const int dividerY = card.bottom() - footerHeight;
        const QRect imageRect(card.left() + 4, card.top() + 4,
                              card.width() - 8, dividerY - card.top() - 8);
        icon.paint(painter, imageRect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        painter->setPen(QColor(128, 128, 128, 65));
        painter->drawLine(card.left(), dividerY, card.right(), dividerY);

        opt.rect = QRect(card.left() + 4, dividerY + 1, card.width() - 8, footerHeight - 1);
        opt.text = opt.fontMetrics.elidedText(name, Qt::ElideRight, opt.rect.width());
        opt.displayAlignment = Qt::AlignCenter;
        style->drawItemText(painter, opt.rect, Qt::AlignCenter, opt.palette,
                            opt.state & QStyle::State_Enabled, opt.text, QPalette::Text);
        painter->restore();
    }
};

// 用半透明灰而非具体颜色：两套主题下写死任何一色都会在另一套上显脏。
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

// 按缓存给格子贴图标，没有就贴占位图；两条路径共用，保证结果一致。
void applyKanbanThumbIcon(QListWidgetItem *item, const QSize &iconSize)
{
    if (!item)
        return;
    const QString key = item->data(Qt::UserRole + 1).toString();
    const QString path = kanban::ModelThumbCache::pathFor(key);

    // 必须缩到**设备**像素并告知 dpr：只缩到逻辑尺寸，Qt 会再放大 dpr 倍填满，糊成一片。
    qreal dpr = 1.0;
    if (QWidget *view = item->listWidget())
        dpr = view->devicePixelRatioF();
    const QSize pixelSize(int(qRound(iconSize.width() * dpr)),
                          int(qRound(iconSize.height() * dpr)));

    QPixmap thumb;
    // 先缩到格子大小再交给 QIcon：原图 320×480，全按原尺寸留在内存要占近 10MB。
    if (!path.isEmpty() && kanban::ModelThumbCache::has(key) && thumb.load(path)) {
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


// 看板娘页 = 顶部页签「看板娘 / 设置」的容器。页签本体在 buildHeader() 的第三组
// HeaderTab，切页逻辑与壁纸页的 m_wallStack 同构。
QWidget *MainWindow::buildKanbanPage()
{
    m_kanbanStack = new QStackedWidget(this);
    m_kanbanStack->addWidget(buildKanbanMainPage()); // 0
    m_kanbanStack->addWidget(buildKanbanLabPage());  // 1
    return m_kanbanStack;
}

// 页签 0：看板娘主体(启动控制、显示与互动、模型网格)。
QWidget *MainWindow::buildKanbanMainPage()
{
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget(scroll);
    auto *lay = new QHBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(14);

    // 左列定宽装两张卡(与动态壁纸页同一套写法)：多出的宽度全给右侧，卡片随内容收缩、
    // 空白集中列尾 —— 所以**卡内不要 addStretch**，否则撑成一大片空框。
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
    m_kanbanNextBtn = new QPushButton(QStringLiteral("⏭ 切换动作"), leftCard);
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
    // 运行状态与模型信息合并显示在右卡底部那一条，见 updateKanbanStatus()。
    m_kanbanLog = new QLabel(QStringLiteral("就绪。"), leftCard);
    m_kanbanLog->setObjectName(QStringLiteral("LogLabel"));
    m_kanbanLog->setWordWrap(true);
    leftLay->addWidget(m_kanbanLog);

    leftColLay->addWidget(leftCard);
    leftColLay->addWidget(buildKanbanParamCard(leftCol));
    leftColLay->addStretch(1);
    lay->addWidget(leftCol);

    // 与左列相反，右卡要**占满高度**(带 stretch=1、列尾不留 stretch)：卡里是网格墙，
    // 高度不够时该由网格自己滚动，而不是卡片缩成几行、下面留一大片空白。
    auto *right = new QWidget(page);
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(14);
    rightLay->addWidget(buildKanbanModelCard(right), 1);
    lay->addWidget(right, 1);

    scroll->setWidget(page);
    return scroll;
}

// 页签 1：实验性功能的落脚点。每行都是一个**规划中**的能力，先用禁用态占位——
// 做完一个换成一个真控件，别在这里堆假开关。
QWidget *MainWindow::buildKanbanLabPage()
{
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 16);

    auto *card = new QFrame(page);
    card->setObjectName(QStringLiteral("PageCard"));
    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(14, 14, 14, 14);
    cardLay->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("设置"), card);
    title->setObjectName(QStringLiteral("GroupTitle"));
    cardLay->addWidget(title);

    auto *hint = new QLabel(
        QStringLiteral("这里是看板娘扩展功能的入口。下面每一条都在规划中、尚未实装，"
                       "做完一个就会在这里变成真的设置项。"), card);
    hint->setObjectName(QStringLiteral("HintLabel"));
    hint->setWordWrap(true);
    cardLay->addWidget(hint);

    const QStringList plannedNames = {
        QStringLiteral("悬停提示"),
        QStringLiteral("自由走动"),
        QStringLiteral("文件拖入"),
        QStringLiteral("语音播报"),
    };
    const QStringList plannedTips = {
        QStringLiteral("鼠标移入/移出模型主体时，在小人旁边浮现文字信息"),
        QStringLiteral("看板娘沿桌面自行走动，而不是固定在一处"),
        QStringLiteral("把文件拖到看板娘身上触发互动(接住、播报、打开…)"),
        QStringLiteral("接入大模型：让看板娘开口说话、读通知、陪聊"),
    };
    for (int i = 0; i < plannedNames.size(); ++i) {
        auto *row = new QHBoxLayout();
        row->setSpacing(10);
        auto *box = new QCheckBox(plannedNames.at(i), card);
        box->setEnabled(false); // 占位：规划中的能力不给可点的假相
        row->addWidget(box);
        auto *desc = new QLabel(plannedTips.at(i), card);
        desc->setObjectName(QStringLiteral("HintLabel"));
        desc->setToolTip(tooltipstyle::format(
            QStringLiteral("%1\n（规划中，尚未实现）").arg(plannedTips.at(i))));
        row->addWidget(desc);
        row->addStretch(1);
        cardLay->addLayout(row);
    }
    cardLay->addStretch(1);

    card->setFixedWidth(uimetrics::kPageLeftColWidth);
    lay->addWidget(card, 0, Qt::AlignTop | Qt::AlignLeft);
    lay->addStretch(1);
    return page;
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
        // 刷新 = 重扫 + **强制**重出图：缓存只按名字找、刻意不做失效判断，所以
        // 「素材换过了」只能由用户显式点一下来传达。
        ensureKanbanModelThumbs(true);
        setKanbanLog(QStringLiteral("已重新扫描模型目录。"), false);
    });
    row->addWidget(refreshBtn);

    // 模型目录的位置说明挂在下面那颗问号上。
    row->addStretch(1);

    // 问号徽标，悬停出提示。用 QLabel 而非 QPushButton：它没有点击行为，做成按钮会误导
    // 用户去点，还得覆盖 QPushButton 的通用样式。尺寸 setFixedSize 定死。
    auto *helpBadge = new QLabel(QStringLiteral("?"), card);
    helpBadge->setObjectName(QStringLiteral("HelpBadge"));
    helpBadge->setFixedSize(kHelpBadgeSize, kHelpBadgeSize);
    helpBadge->setAlignment(Qt::AlignCenter);
    helpBadge->setCursor(Qt::WhatsThisCursor);
    helpBadge->setToolTip(tooltipstyle::format(QStringLiteral(
        "模型目录：<程序目录>\\data\\models\n"
        "\n"
        "每个模型占一个子目录，里面放它的 *.model3.json 与贴图、动作文件。\n"
        "放好后点「↻ 刷新」重新扫描，封面会自动生成。")));
    row->addWidget(helpBadge);
    lay->addLayout(row);

    // 网格墙：每个模型一张静态预览图，不播动作。
    m_kanbanModelGrid = new QListWidget(card);
    auto *grid = m_kanbanModelGrid;
    // 复用图库浏览的 objectName：格子卡片直接继承「图片浏览」样式，两套主题不必再写 QSS。
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
    // 横向 sizeHint 不参与布局分配：否则形成「宽度→列数→sizeHint→宽度」自激环，拖窗口
    // 时无限重排直至未响应。与图片浏览页同一处理。
    grid->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    grid->setMinimumHeight(240);
    grid->setSelectionMode(QAbstractItemView::SingleSelection);
    // **策略必须装在 grid(QAbstractScrollArea)上，不能装 viewport 上**：前者给 viewport
    // 装了事件过滤器，ContextMenu 会被它截走转给 viewportEvent()，现象是「右键毫无反应」
    // 且不打日志。槽里拿到的 pos 与 itemAt() 同一套视口坐标。
    grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid, &QWidget::customContextMenuRequested, this,
            &MainWindow::showKanbanModelMenu);
    connect(grid, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item, QListWidgetItem *) {
                if (m_kanbanSyncing || !item)
                    return;
                // 路径存在条目里而不是按下标回查：将来一加排序，按下标就会切错模型。
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
    m_kanbanModelInfo->setTextFormat(Qt::RichText);
    lay->addWidget(m_kanbanModelInfo);

    // 常驻说明塞进刷新行右端的 #HelpBadge 提示里：一次性说明不该长期占版面(实测两行)。
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

    // 三行滑块用「标签 | 滑块 | 数值」的网格，数值列等宽，来回拉动时不抖版。
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


    auto addCheckRow = [lay, card](QCheckBox *box) {
        auto *row = new QHBoxLayout();
        row->setSpacing(14);
        row->addWidget(box);
        row->addStretch(1);
        lay->addLayout(row);
    };

    m_kanbanTopBox = new QCheckBox(QStringLiteral("窗口置顶"), card);
    m_kanbanTopBox->setToolTip(tooltipstyle::format(
        QStringLiteral("让小人始终浮在其它窗口之上，不被别的程序挡住")));
    connect(m_kanbanTopBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setAlwaysOnTop(on);
    });
    addCheckRow(m_kanbanTopBox);

    // 这个勾选框只管「用不用清单」，清单本身在模型目录里（查看器导出的 *.hidden.json）。
    // 提示词必须写清「改文件要重新装载」—— 否则用户改完清单发现没生效，只会当成坏了。
    m_kanbanMeshHideBox = new QCheckBox(QStringLiteral("按清单隐藏网格"), card);
    m_kanbanMeshHideBox->setToolTip(tooltipstyle::format(meshHideTooltip(QString())));
    connect(m_kanbanMeshHideBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setMeshHideEnabled(on);
    });
    addCheckRow(m_kanbanMeshHideBox);

    m_kanbanInteractBox = new QCheckBox(QStringLiteral("允许点击互动"), card);
    m_kanbanInteractBox->setToolTip(tooltipstyle::format(
        QStringLiteral("勾选后可以用鼠标拖动小人、点它触发动作；\n"
                       "取消勾选则只显示不响应，鼠标拖动会落到桌面")));
    connect(m_kanbanInteractBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setInteractionEnabled(on);
    });
    addCheckRow(m_kanbanInteractBox);

    // 横线划分：线上是「窗口/显示类」项，线下是动作、声音、穿透等「行为类」项。
    auto *displaySep = new QFrame(card);
    displaySep->setObjectName(QStringLiteral("SideCardSep"));
    displaySep->setFrameShape(QFrame::HLine);
    lay->addWidget(displaySep);

    m_kanbanMotionLoopBox = new QCheckBox(QStringLiteral("动作循环"), card);
    m_kanbanMotionLoopBox->setToolTip(tooltipstyle::format(
        QStringLiteral("默认开启：当前动作播完后自动切换到下一个，到尾后从头循环")));
    connect(m_kanbanMotionLoopBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setMotionLoopEnabled(on);
    });
    addCheckRow(m_kanbanMotionLoopBox);

    m_kanbanSoundBox = new QCheckBox(QStringLiteral("播放声音"), card);
    m_kanbanSoundBox->setToolTip(tooltipstyle::format(
        QStringLiteral("默认开启：切动作时播放模型自带的语音。\n"
                       "取消勾选后，即便模型带语音也不会出声。")));
    connect(m_kanbanSoundBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setSoundEnabled(on);
    });
    addCheckRow(m_kanbanSoundBox);

    m_kanbanDoubleClickBox = new QCheckBox(QStringLiteral("双击动画切换动作"), card);
    m_kanbanDoubleClickBox->setToolTip(tooltipstyle::format(
        QStringLiteral("默认开启：双击桌面上的动画人物，切换到下一个动作")));
    connect(m_kanbanDoubleClickBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setDoubleClickSwitchEnabled(on);
    });
    addCheckRow(m_kanbanDoubleClickBox);

    m_kanbanThroughBox = new QCheckBox(QStringLiteral("鼠标穿透"), card);
    m_kanbanThroughBox->setToolTip(tooltipstyle::format(
        QStringLiteral("开启后鼠标对窗口隐形，点击全部落到桌面。\n"
                       "注意：穿透期间小人收不到鼠标，右键菜单也叫不出来 —— "
                       "要关掉只能回这一页取消勾选。")));
    connect(m_kanbanThroughBox, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_kanbanSyncing)
            m_kanban->setMouseThrough(on);
    });
    addCheckRow(m_kanbanThroughBox);

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

    // id 直接用档位值：槽里拿到的就能交给控制器，不用再映射一次。
    m_kanbanGazeGroup = new QButtonGroup(this);
    m_kanbanGazeGroup->setExclusive(true);
    m_kanbanGazeGroup->addButton(m_kanbanGazeOff, kanban::KanbanRenderer::GazeOff);
    m_kanbanGazeGroup->addButton(m_kanbanGazeWeak, kanban::KanbanRenderer::GazeWeak);
    m_kanbanGazeGroup->addButton(m_kanbanGazeMedium, kanban::KanbanRenderer::GazeMedium);
    m_kanbanGazeGroup->addButton(m_kanbanGazeStrong, kanban::KanbanRenderer::GazeStrong);
    connect(m_kanbanGazeGroup, &QButtonGroup::idClicked, this, [this](int strength) {
        m_kanban->setGazeStrength(strength);
    });
    gazeRow->addStretch(1);
    lay->addLayout(gazeRow);

    auto *sep = new QFrame(card);
    sep->setObjectName(QStringLiteral("SideCardSep"));
    sep->setFrameShape(QFrame::HLine);
    lay->addWidget(sep);

    auto *trayTitle = new QLabel(QStringLiteral("系统与快捷键"), card);
    trayTitle->setObjectName(QStringLiteral("GroupTitle"));
    lay->addWidget(trayTitle);

    // 软件自启：与壁纸页的「开机自动启动」是同一个注册表项(appinfo)，两处复选框
    // 由 toggled 处理器互相同步，哪边勾都算数。
    m_kanbanAutostartBox = new QCheckBox(QStringLiteral("开机自动启动"), card);
    m_kanbanAutostartBox->setToolTip(tooltipstyle::format(
        QStringLiteral("开机后自动运行本软件：壁纸与看板娘按上次退出时的状态恢复")));
    m_kanbanAutostartBox->setChecked(appinfo::autostartEnabled());
    connect(m_kanbanAutostartBox, &QCheckBox::toggled, this, [this](bool on) {
        if (m_kanbanSyncing)
            return;
        appinfo::setAutostart(on);
        if (m_autostartBox && m_autostartBox->isChecked() != on) {
            QSignalBlocker blocker(m_autostartBox);
            m_autostartBox->setChecked(on);
        }
    });
    addCheckRow(m_kanbanAutostartBox);

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

    // 显示/隐藏看板娘的全局快捷键：注册在系统层，主窗口没有焦点(游戏全屏、托盘
    // 模式)也生效。存 PortableText；清空即停用；注册不上(组合不带 Ctrl/Alt/Win、
    // 或被别的程序占用)时回弹成停用并写日志说明原因。
    auto *hotkeyRow = new QHBoxLayout();
    hotkeyRow->setSpacing(14);
    hotkeyRow->addWidget(new QLabel(QStringLiteral("显示/隐藏看板娘"), card));
    hotkeyRow->addSpacing(4);
    m_kanbanHotkeyEdit = new QKeySequenceEdit(card);
    m_kanbanHotkeyEdit->setToolTip(tooltipstyle::format(
        QStringLiteral("在键盘上按下想用的组合键(需含 Ctrl/Alt/Win)，全局生效。\n"
                       "默认 Ctrl+Alt+K；清空输入框即停用快捷键")));
    m_kanbanHotkeyEdit->setFixedWidth(160);
    connect(m_kanbanHotkeyEdit, &QKeySequenceEdit::keySequenceChanged, this,
            [this](const QKeySequence &seq) {
        if (m_kanbanSyncing)
            return;
        auto *config = &AppConfig::instance();
        const auto key = QString::fromLatin1(ConfigKeys::Kanban::ToggleHotkey);
        if (!m_kanbanHotkey || !m_kanbanHotkey->applySequence(seq)) {
            // 注册失败：编辑框弹回空(停用)，配置同步清掉，别留下「看着设了其实没生效」的值。
            config->setValue(key, QString());
            m_kanbanSyncing = true;
            m_kanbanHotkeyEdit->setKeySequence(QKeySequence());
            m_kanbanSyncing = false;
            setKanbanLog(QStringLiteral("快捷键没有生效：需要 Ctrl/Alt/Win 加字母、数字或 F 键，"
                                       "且未被其他程序占用。"), true);
            return;
        }
        config->setValue(key, seq.toString(QKeySequence::PortableText));
        setKanbanLog(seq.isEmpty()
                         ? QStringLiteral("快捷键已停用。")
                         : QStringLiteral("快捷键已设为 %1，立即生效。")
                               .arg(seq.toString(QKeySequence::NativeText)),
                     false);
    });
    hotkeyRow->addWidget(m_kanbanHotkeyEdit);
    hotkeyRow->addStretch(1);
    lay->addLayout(hotkeyRow);

    return card;
}
void MainWindow::setupKanbanAndTray()
{
    m_kanban = std::make_unique<kanban::KanbanController>(this);
    kanban::KanbanController &kan = *m_kanban;

    connect(&kan, &kanban::KanbanController::measuredFpsChanged,
            this, &MainWindow::updateKanbanStatus);

    connect(&kan, &kanban::KanbanController::settingsChanged,
            this, &MainWindow::updateKanbanControls);
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
            [this](const QString &) { updateKanbanControls(); });
    connect(&kan, &kanban::KanbanController::currentModelChanged, this,
            [this](const QString &name) {
                updateKanbanControls();
                // 唯一例外：**没装载成功**时留一条 —— 那时只有这句话说明「看到的是占位形象」。
                if (name.isEmpty())
                    setKanbanLog(QStringLiteral("未装载模型，使用内置占位形象。"), false);
            });
    // 右键菜单「打开设置」：把主窗口捞回来并停在看板娘页。
    connect(&kan, &kanban::KanbanController::openSettingsRequested, this, [this] {
        showFromTray();
        m_nav->setCurrentRow(2);
    });

    refreshKanbanModels();
    updateKanbanControls();

    // 构造开头恢复视频壁纸时 playbackStateChanged 还没接上，聚合器里会是「没在跑」；
    // 托盘初始化依赖这个事实，先补一次。
    {
        VideoWallpaper &video = VideoWallpaper::instance();
        ApplicationRuntimeState::instance().setWallpaperState(
            video.isStarted(), video.isStarted() && !video.isPlaying());
    }

    // 全局快捷键：注册在系统层，WM_HOTKEY 由 nativeEventFilter 转成信号。设置页
    // 改键只调 applySequence，这里只在启动时装一次初始值。
    m_kanbanHotkey = new fbswin::GlobalHotkey(this);
    connect(m_kanbanHotkey, &fbswin::GlobalHotkey::activated, this, [this] {
        if (m_kanban)
            m_kanban->toggleVisible();
    });
    const QString storedHotkey = AppConfig::instance().value(
        QString::fromLatin1(ConfigKeys::Kanban::ToggleHotkey),
        QStringLiteral("Ctrl+Alt+K")).toString();
    const QKeySequence hotkeySeq(storedHotkey);
    m_kanbanSyncing = true;
    if (m_kanbanHotkeyEdit)
        m_kanbanHotkeyEdit->setKeySequence(hotkeySeq);
    m_kanbanSyncing = false;
    if (!m_kanbanHotkey->applySequence(hotkeySeq)) {
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("全局快捷键 %1 注册失败(被占用或组合不合法)，已停用")
                           .arg(storedHotkey),
                       QStringLiteral("Kanban"));
    }

    m_tray = new SystemTrayController(this);
    m_tray->setKanbanController(m_kanban.get());
    const bool trayReady = m_tray->initialize();
    if (trayReady) {
        connect(m_tray, &SystemTrayController::showMainWindowRequested, this,
                &MainWindow::showFromTray);
        connect(m_tray, &SystemTrayController::quitRequested, this,
                &MainWindow::onTrayQuitRequested);
        // 托盘在时进程归托盘管：主窗口是最后一个可见窗口，沿用 Qt 默认策略的话隐藏它会
        // 顺手把进程结束掉。
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

    // 自动拉起判据是 `kanban/enabled`：publishState() 实时维护、stop()/enterError() 会清、
    // shutdownForExit() **不碰**，故恰好等于「上次退出时在不在跑」；无此键则默认不启动。
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

// 重扫模型目录并重建网格，尽量保住用户当前选中的那一项。
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
        // UserRole   = .model3.json 绝对路径，点击时直接拿它切模型
        // UserRole+1 = 预览图缓存键(ModelInfo::thumbKey，相对 data/models 的
        //              路径换 '#'，与生成进程一致)，可能为空(空则不出预览图)
        // UserRole+2 = 该格子的图是不是真预览图(占位图时为 false)，用于 tooltip
        item->setData(Qt::UserRole, model.modelJsonPath);
        item->setData(Qt::UserRole + 1, model.thumbKey);
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


void MainWindow::showKanbanModelMenu(const QPoint &viewportPos)
{
    if (!m_kanbanModelGrid)
        return;
    QListWidgetItem *item = m_kanbanModelGrid->itemAt(viewportPos);
    if (!item)
        return;

    QMenu menu;
    QAction *deleteAction = menu.addAction(QStringLiteral("删除模型"));

    // 用 exec() 的返回值判断点了哪一项：这样「菜单已关」与「动作执行」在时间上分开。
    QAction *chosen = menu.exec(m_kanbanModelGrid->viewport()->mapToGlobal(viewportPos));
    if (chosen == deleteAction)
        deleteKanbanModel(item);
}

// 删除一个模型：整个文件夹移入回收站，缓存预览图一并清掉。
void MainWindow::deleteKanbanModel(QListWidgetItem *item)
{
    if (!item || !m_kanban)
        return;

    // 先取出要用的东西：下面的 refreshKanbanModels() 会 clear() 整个网格，item 随即失效，
    // 之后再碰就是 use-after-free。
    const QString modelName = item->text();
    const QString jsonPath = item->data(Qt::UserRole).toString();
    const QString thumbKey = item->data(Qt::UserRole + 1).toString();
    if (jsonPath.isEmpty())
        return;

    // 模型文件夹 = .model3.json 所在目录，由它推出来，不另外存一份。
    const QString dirPath = QFileInfo(jsonPath).absolutePath();

    // **不弹确认框，点菜单项即删**：前提是删除**走回收站**(见 fbswin::moveToRecycleBin)，
    // 撤销路径在回收站里。代价是没有第二次机会，故无论成败都要写界面提示 + 落盘日志。
    QString error;
    if (!fbswin::moveToRecycleBin(dirPath, &error)) {
        // 失败到此为止，**绝不退化成永久删除**。措辞不能说「模型文件未被改动」：重试仍
        // 失败时最可能恰恰是「文件已全进回收站，只剩空目录删不掉」(见 shellfileops.cpp)。
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("删除模型「%1」失败: %2 (目录 %3)")
                           .arg(modelName, error, QDir::toNativeSeparators(dirPath)),
                       QStringLiteral("Kanban"));
        setKanbanLog(QStringLiteral("删除模型「%1」失败：%2").arg(modelName, error), true);
        QMessageBox::warning(this, QStringLiteral("删除失败"),
                             QStringLiteral("没能删掉「%1」。\n\n%2\n\n"
                                            "请点「↻ 刷新」确认它是否还在 —— "
                                            "失败时可能已经删掉了其中一部分。")
                                 .arg(modelName, error));
        return;
    }

    // 缓存图可再生，直接永久删掉，不占回收站。键可能为空(该模型本来就没出过图)，
    // remove() 对空键原样返回 false，正合适。
    const bool thumbRemoved = kanban::ModelThumbCache::remove(thumbKey);

    const bool wasCurrent = (m_kanban->modelPath() == jsonPath);

    // 删的是当前模型时要换成「它的下一个」，故**必须在刷新前**记下它的下标：刷新后它
    // 就不在列表里，按路径找不到时 nextValidAfter() 会算错。
    int removedIndex = -1;
    if (wasCurrent) {
        const QVector<kanban::ModelInfo> before = m_kanban->validModelList();
        for (int i = 0; i < before.size(); ++i) {
            if (before.at(i).modelJsonPath == jsonPath) {
                removedIndex = i;
                break;
            }
        }
    }

    refreshKanbanModels(); // 重扫目录 + 重建网格（此刻 item 已失效，别再用）

    const QVector<kanban::ModelInfo> left = m_kanban->validModelList();
    if (wasCurrent && !left.isEmpty()) {
        const int next = kanban::KanbanModelManager::successorIndexAfterRemoval(
            removedIndex, left.size());
        m_kanban->setModelPath(left.at(next).modelJsonPath);
    }

    updateKanbanControls();

    QString log = QStringLiteral("已删除模型「%1」%2，已移入回收站。剩余可用模型 %3 个。")
                      .arg(modelName,
                           thumbRemoved ? QStringLiteral("及预览图") : QString(),
                           QString::number(left.size()));
    if (left.isEmpty())
        log += QStringLiteral(" 放进新模型后点「↻ 刷新」即可。");
    setKanbanLog(log, left.isEmpty());

    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("已删除模型「%1」: 目录 %2 已移入回收站，"
                                  "预览图缓存 %3，剩余可用模型 %4 个")
                       .arg(modelName, QDir::toNativeSeparators(dirPath),
                            thumbRemoved ? QStringLiteral("已清除")
                                         : QStringLiteral("原本就没有"),
                            QString::number(left.size())),
                   QStringLiteral("Kanban"));
}


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

// 单个模型出图后即时贴图：不必整面墙重贴。
void MainWindow::applyKanbanModelThumb(const QString &thumbKey)
{
    if (!m_kanbanModelGrid || thumbKey.isEmpty())
        return;
    const QSize iconSize = m_kanbanModelGrid->iconSize();
    for (int i = 0; i < m_kanbanModelGrid->count(); ++i) {
        QListWidgetItem *item = m_kanbanModelGrid->item(i);
        if (!item || item->data(Qt::UserRole + 1).toString() != thumbKey)
            continue;
        applyKanbanThumbIcon(item, iconSize);
        item->setToolTip(tooltipstyle::format(
            QStringLiteral("点击把看板娘换成「%1」").arg(item->text())));
        return;
    }
}

// 发现缺图就起一个生成进程：整条链路只有「本程序再起一个自己」，无线程、无共享 GL 上下文
// —— 原因见 src/kanban/ModelThumbJob.h。
void MainWindow::ensureKanbanModelThumbs(bool force)
{
    if (!m_kanbanModelGrid)
        return;
    if (m_kanbanThumbJob)
        return;
    if (!m_kanban->live2dAvailable())
        return;

    // 全都在缓存里就什么都不做 —— 这是绝大多数启动的情形，也是「省资源」的关键。
    QStringList missing;
    for (int i = 0; i < m_kanbanModelGrid->count(); ++i) {
        const QListWidgetItem *item = m_kanbanModelGrid->item(i);
        if (!item)
            continue;
        const QString key = item->data(Qt::UserRole + 1).toString();
        if (key.isEmpty())
            continue;
        if (force || !kanban::ModelThumbCache::has(key))
            missing << key;
    }
    if (missing.isEmpty())
        return;

    // 顺手清掉上次被杀留下的 .tmp —— 用户知道这个目录在哪，别让他看到垃圾。
    kanban::ModelThumbCache::sweepTempFiles();

    auto *job = new QProcess(this);
    QStringList args;
    args << QStringLiteral("--render-model-thumbs");
    if (force)
        args << QStringLiteral("--force");
    // 用本程序自己的 exe：渲染代码绝对同版本。正式版是 requireAdministrator 清单，但父
    // 进程已提权，子进程继承令牌，不会弹 UAC。
    job->setProgram(QCoreApplication::applicationFilePath());
    job->setArguments(args);

    m_kanbanThumbJob = job;

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

        if (error != QProcess::FailedToStart)
            return;
        const QString reason = m_kanbanThumbJob ? m_kanbanThumbJob->errorString() : QString();
        videodiag::log(videodiag::Level::Warning,
                       QStringLiteral("预览图生成进程启动失败: %1").arg(reason),
                       QStringLiteral("Kanban"));
        onKanbanThumbFinished(-1);
    });

    // 边生成边贴：每 250ms 看一眼缓存目录，谁先落盘谁先亮。不读子进程的 stdout。
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

// 任务期间的轮询：已落盘的图先贴上去。判据是「修改时间与基线不同」而非「文件存在」——
// 强制重建时文件本来就在，只有修改时间能说明它刚被重写过。
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
            continue; // 还是任务开始前那一张
        }
        // erase 已返回下一项，不能再 ++it，否则会跳项甚至崩溃。
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

    // 收尾时再轮询一次：进程退出与上一次定时器触发之间落盘的那几张别漏掉。
    pollKanbanModelThumbs();

    // 剩下的就是没出图的。完成与否只看磁盘：文件一定写得出来，GUI 子系统进程的 stdout
    // 不一定。
    const int failed = int(m_kanbanThumbBaseline.size());
    const int ok = m_kanbanThumbTotal - failed;
    m_kanbanThumbBaseline.clear();
    m_kanbanThumbTotal = 0;

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
    if (!m_kanban || !m_kanbanModelInfo)
        return;

    const QString valueColor = kanbanValueColor(m_kanbanModelInfo);
    QString status;
    const auto appendStatus = [&](const QString &label, const QString &value) {
        if (!status.isEmpty())
            status += QStringLiteral(" · ");
        status += kanbanPairHtml(label, value, valueColor);
    };

    appendStatus(QStringLiteral("状态"), m_kanban->stateText());
    if (m_kanban->isRunning()) {
        appendStatus(QStringLiteral("后端"), m_kanban->backendText());
        appendStatus(QStringLiteral("实测"), QStringLiteral("%1 fps").arg(m_kanban->measuredFps()));
    } else if (!m_kanban->backendText().isEmpty()) {
        appendStatus(QStringLiteral("后端"), m_kanban->backendText());
    }

    const int motionTotal = m_kanban->playableMotionCount();
    if (motionTotal > 0) {
        appendStatus(QStringLiteral("当前动作"),
                     QStringLiteral("%1/%2")
                         .arg(m_kanban->currentMotionOrdinal())
                         .arg(motionTotal));
    }

    // 模型明细还没算过时只显示状态行，别拼出一个空行。
    m_kanbanModelInfo->setText(m_kanbanModelLine.isEmpty()
                                   ? status
                                   : status + QStringLiteral("<br>") + m_kanbanModelLine);
}

// 同步按钮、模型选中项与设置控件。
void MainWindow::updateKanbanControls()
{
    if (!m_kanban || !m_kanbanStartBtn)
        return;

    const bool running = m_kanban->isRunning();
    const bool paused = m_kanban->isPaused();
    const bool failed = m_kanban->state() == kanban::State::Error;
    // Stopping 是「正在收口」的中间态：窗口与渲染器正在拆，此时再点会在 stop() 里
    // 撞上非法转移。它是唯一该置灰的「在跑」状态，只存活一瞬。
    const bool stopping = m_kanban->state() == kanban::State::Stopping;

    // **双态开关**：「启动」/「取消」(运行中或失败待重试)。enabled 与 text 必须由同一个
    // 判据驱动 —— 各写一套会出现「文字变了、颜色变灰、点不动」。
    const bool canToggle = !stopping;
    m_kanbanStartBtn->setEnabled(canToggle);
    m_kanbanStartBtn->setText((running || failed) ? QStringLiteral("■ 取消")
                                                   : QStringLiteral("▶ 启动"));
    m_kanbanStartBtn->setProperty("data-active", (running || failed) ? 1 : 0);
    m_kanbanStartBtn->style()->unpolish(m_kanbanStartBtn);
    m_kanbanStartBtn->style()->polish(m_kanbanStartBtn);
    m_kanbanPauseBtn->setEnabled(running);
    m_kanbanPauseBtn->setText(paused ? QStringLiteral("⏸ 继续") : QStringLiteral("⏸ 暂停"));
    // 可播动作不足两个就置灰并说明原因 —— 不解释原因的灰按钮用户只会当成 bug。实测
    // 13 个模型里 8 个不到 2 个动作，灰掉是常态，更要说清楚。
    const int motionCount = m_kanban->playableMotionCount();
    const bool canPlayMotion = m_kanban->canPlayNextMotion();
    m_kanbanNextBtn->setEnabled(running && !paused && canPlayMotion);
    m_kanbanNextBtn->setToolTip(canPlayMotion
                                    ? QStringLiteral("当前模型有 %1 个动作，点击逐个切换")
                                          .arg(motionCount)
                                    : (motionCount == 0
                                           ? QStringLiteral("当前模型没有可播放的动作")
                                           : QStringLiteral("当前模型只有 1 个动作，没有可切换的对象")));
    // 没有表情就置灰，并把原因写进提示，用户才能自己换一个带表情的模型。
    const int exprCount = m_kanban->expressionCount();
    m_kanbanExprBtn->setEnabled(running && !paused && exprCount > 0);
    m_kanbanExprBtn->setToolTip(exprCount > 0
                                    ? QStringLiteral("当前模型有 %1 个表情，点击逐个切换")
                                          .arg(exprCount)
                                    : QStringLiteral("当前模型没有表情文件"));

    const QVector<kanban::ModelInfo> models = m_kanban->validModelList();
    const QString valueColor = kanbanValueColor(m_kanbanModelInfo);
    QString info;
    int selectedRow = -1;
    if (models.isEmpty()) {
        info = kanbanPairHtml(QStringLiteral("可用模型"), QStringLiteral("0 个"), valueColor);
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
            // 当前模型不在列表里时高亮第一个 —— 空着不高亮会让用户以为「一个都没有」。
            if (selectedRow < 0)
                selectedRow = 0;
            m_kanbanModelGrid->setCurrentRow(selectedRow);
        }
        m_kanbanSyncing = wasSyncing;

        const int index = (selectedRow >= 0 && selectedRow < models.size()) ? selectedRow : 0;
        const kanban::ModelInfo &sel = models.at(index);
        // 模型装载后以渲染器实际装载成功的动作数为准，包含 idle，
        // 与上方「当前动作 x/y」以及模型文件中的动作数保持一致。
        const int detailMotionCount =
            (running && sel.modelJsonPath == m_kanban->modelPath())
                ? m_kanban->playableMotionCount()
                : sel.motionCount;
        info = kanbanPairHtml(QStringLiteral("可用模型"),
                              QStringLiteral("%1 个").arg(models.size()), valueColor)
               + QStringLiteral(" · ")
               + kanbanPairHtml(QStringLiteral("当前"), sel.name, valueColor)
               + QStringLiteral(" · ")
               + kanbanPairHtml(QStringLiteral("贴图"), QString::number(sel.textureCount),
                                valueColor)
               + QStringLiteral(" · ")
               + kanbanPairHtml(QStringLiteral("动作"), QString::number(detailMotionCount),
                                valueColor)
               + QStringLiteral(" · ")
               + kanbanPairHtml(QStringLiteral("表情"), QString::number(sel.expressionCount),
                                valueColor);
    }
    if (!m_kanban->live2dAvailable())
        info += QStringLiteral("<br>")
                + QStringLiteral("本程序未编译 Live2D 后端，模型只扫描校验，画面用内置占位形象。")
                      .toHtmlEscaped();

    // 清单生效情况只有渲染器知道（装载时解析的），所以直接问它。没有清单时是空串，
    // 不留一段空位；有清单却关着开关时仍然显示 —— 用户得看得出「藏了但没生效」。
    const QString meshHideSummary = m_kanban->meshHideText();
    if (!meshHideSummary.isEmpty()) {
        info += QStringLiteral(" · ")
                + kanbanPairHtml(QStringLiteral("网格隐藏"), meshHideSummary, valueColor);
    }

    m_kanbanModelLine = info;
    updateKanbanStatus();

    m_kanbanSyncing = true;
    m_kanbanScale->setValue(m_kanban->scalePercent());
    m_kanbanOpacity->setValue(m_kanban->opacityPercent());
    m_kanbanFps->setValue(m_kanban->targetFps());
    m_kanbanTopBox->setChecked(m_kanban->alwaysOnTop());
    m_kanbanThroughBox->setChecked(m_kanban->mouseThrough());
    m_kanbanInteractBox->setChecked(m_kanban->interactionEnabled());
    m_kanbanMotionLoopBox->setChecked(m_kanban->motionLoopEnabled());
    m_kanbanSoundBox->setChecked(m_kanban->soundEnabled());
    m_kanbanDoubleClickBox->setChecked(m_kanban->doubleClickSwitchEnabled());
    m_kanbanMeshHideBox->setChecked(m_kanban->meshHideEnabled());
    m_kanbanMeshHideBox->setToolTip(tooltipstyle::format(meshHideTooltip(meshHideSummary)));

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
    // 托盘这项直接读配置：不归 KanbanController 管，不回填的话每次重绘都显示未勾选，
    // 用户以为设置丢了。
    auto &cfg = AppConfig::instance();
    m_trayMinimizeBox->setChecked(
        cfg.value(ConfigKeys::Tray::MinimizeToTrayOnClose, true).toBool());
    m_kanbanSyncing = false;

    if (m_tray)
        m_tray->updateRuntimeState();

    // 诊断探针：把「启动/取消」按钮的可用性与位置写进日志，让自动化验证能**不问像素**地
    // 拿到「此刻可不可点、在哪」。只在 YUMEIREN_DIAG=1 时落盘(Debug 级)。
    if (videodiag::diagEnabled()) {
        const QSize sz = m_kanbanStartBtn->size();
        // 相对整窗的坐标才是可点的：geometry() 给的是它在**直接父容器**里的位置(实测恒
        // 为 0,0)，mapTo(this) 逐级换算到主窗口客户区。
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

        // 视线四档的选中态与可点坐标。回填与点击走两条路，打印 checked 状态即可核对互斥。
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

        // 「按清单隐藏网格」的开关状态、清单摘要与可点坐标。自动化验证靠这一行拿到
        // 「现在勾没勾、清单认了几条、点哪里」，不必去数像素。
        {
            const QSize msz = m_kanbanMeshHideBox->size();
            const QPoint mc =
                m_kanbanMeshHideBox->mapTo(this, QPoint(msz.width() / 2, msz.height() / 2));
            const QPoint mg =
                m_kanbanMeshHideBox->mapToGlobal(QPoint(msz.width() / 2, msz.height() / 2));
            videodiag::log(videodiag::Level::Debug,
                           QStringLiteral("[KanbanPage] 网格隐藏 checked=%1 enabled=%2 "
                                          "摘要=%3 中心=%4,%5 屏幕=%6,%7 尺寸=%8x%9")
                               .arg(m_kanbanMeshHideBox->isChecked() ? 1 : 0)
                               .arg(m_kanbanMeshHideBox->isEnabled() ? 1 : 0)
                               .arg(meshHideSummary.isEmpty() ? QStringLiteral("(无清单)")
                                                              : meshHideSummary)
                               .arg(mc.x()).arg(mc.y())
                               .arg(mg.x()).arg(mg.y())
                               .arg(msz.width()).arg(msz.height()),
                           QStringLiteral("Kanban"));
        }
    }
}

// 看板娘页自己的日志行：主界面日志条只属于文件夹美化页，不借用它。
void MainWindow::setKanbanLog(const QString &text, bool isError)
{
    if (!m_kanbanLog)
        return;
    m_kanbanLog->setText(text);
    m_kanbanLog->setProperty("data-err", isError ? 1 : 0);
    m_kanbanLog->style()->unpolish(m_kanbanLog);
    m_kanbanLog->style()->polish(m_kanbanLog);
}

// 托盘图标左键单击/双击：窗口可能是 hide() 掉的，先 show 再解最小化。
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
    for (auto *t : m_kanbanTabs)
        t->setVisible(row == 2);
    m_statusBox->setVisible(row == 0);

    if (row == 0)
        selectHeaderTab(m_stack->currentIndex());
    else if (row == 1)
        selectWallTab(m_wallStack->currentIndex());
    else if (row == 2) {
        selectKanbanTab(m_kanbanStack ? m_kanbanStack->currentIndex() : 0);
        updateKanbanControls();

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
    // 底部那行若还是「就绪」提示，就跟着页签换成该页的 —— 「切到效果样式却仍写着
    // 图片页那句」就是这么来的。结果是「某次操作的反馈」时不换：用户刚点了应用，
    // 那句「效果样式已应用！」比一句泛泛的就绪提示有用。
    if (index == 0 || index == 1) {
        const QString hint = index == 0 ? imagePageHintText() : effectPageHintText();
        if (m_logShowsHint && m_logLabel->text() != hint)
            setLogHint(hint);
    }
    if (index == 0)
        updateImagePreview();
}

void MainWindow::selectWallTab(int index)
{
    if (index < 0 || index >= m_wallTabs.size())
        return;
    for (int i = 0; i < m_wallTabs.size(); ++i)
        m_wallTabs[i]->setChecked(i == index);
    m_wallStack->setCurrentIndex(index);
}

void MainWindow::selectKanbanTab(int index)
{
    if (index < 0 || index >= m_kanbanTabs.size())
        return;
    for (int i = 0; i < m_kanbanTabs.size(); ++i)
        m_kanbanTabs[i]->setChecked(i == index);
    if (m_kanbanStack)
        m_kanbanStack->setCurrentIndex(index);
}

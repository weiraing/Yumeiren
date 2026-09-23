#include "ui/MediaLibraryCard.h"

#include "ui/TooltipStyle.h"

#include <QDesktopServices>
#include <QDir>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

MediaLibraryCard::MediaLibraryCard(const QString &title, const QString &nameHeader,
                                   const QString &dir, const QString &helpText,
                                   const QString &noteText, QFrame *parent)
    : QFrame(parent)
    , m_dir(dir)
{
    setObjectName(QStringLiteral("PageCard"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(14, 12, 14, 14);
    lay->setSpacing(8);

    auto *t = new QLabel(title, this);
    t->setObjectName(QStringLiteral("GroupTitle"));
    lay->addWidget(t);

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);

    m_scanBtn = new QPushButton(QStringLiteral("↻ 刷新"), this);
    m_scanBtn->setObjectName(QStringLiteral("LibraryScanButton"));
    m_scanBtn->setMinimumWidth(72);
    m_scanBtn->setMinimumHeight(38);
    connect(m_scanBtn, &QPushButton::clicked, this, [this] { emit scanRequested(); });
    toolbar->addWidget(m_scanBtn);

    auto *openBtn = new QPushButton(QStringLiteral("打开目录"), this);
    openBtn->setMinimumHeight(38);
    connect(openBtn, &QPushButton::clicked, this, [this] {
        QDir().mkpath(m_dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_dir));
    });
    toolbar->addWidget(openBtn);

    m_deleteBtn = new QPushButton(QStringLiteral("✕ 删除"), this);
    m_deleteBtn->setObjectName(QStringLiteral("LibraryDeleteButton"));
    m_deleteBtn->setMinimumWidth(72);
    m_deleteBtn->setMinimumHeight(38);
    m_deleteBtn->setEnabled(false);
    m_deleteBtn->setToolTip(tooltipstyle::format(
        QStringLiteral("删除勾选的条目(可多选)。文件移入回收站，可在回收站还原。")));
    connect(m_deleteBtn, &QPushButton::clicked, this,
            [this] { emit deleteCheckedRequested(); });
    toolbar->addWidget(m_deleteBtn);
    toolbar->addStretch(1);

    auto *help = new QLabel(QStringLiteral("?"), this);
    help->setObjectName(QStringLiteral("HelpBadge"));
    help->setAlignment(Qt::AlignCenter);
    help->setFixedSize(22, 22);
    // noteText 并进 ? 的 tooltip(2026-09-23)：卡片底部原来有第二行常驻说明，现在
    // 只留一行三段式信息，说明内容全挪到这里 —— 所以传参时别跟 helpText 写重。
    help->setToolTip(tooltipstyle::format(
        noteText.isEmpty() ? helpText : helpText + QLatin1Char('\n') + noteText));
    toolbar->addWidget(help);
    lay->addLayout(toolbar);

    m_tree = new QTreeWidget(this);
    // 两个页面的库列表共用同一个对象名，从而共用同一组外观 QSS
    m_tree->setObjectName(QStringLiteral("LibraryTree"));
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({nameHeader, QStringLiteral("类型")});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    lay->addWidget(m_tree, 1);

    // 底部只留这一行：统计信息 · 状态 · 播放数据名(见 setInfo)。
    // ⚠️ 刻意 setWordWrap(false)：两个库的页脚高度因此完全一致，不会因为某页
    // 状态文案长一点就把卡片底边顶上去。文案放不下时会裁尾巴 —— 所以三段都要短。
    // (原来这里还有第二个 QLabel 常驻显示 noteText，已并进 ? 的 tooltip。)
    m_info = new QLabel(this);
    m_info->setObjectName(QStringLiteral("HintLabel"));
    m_info->setWordWrap(false);
    lay->addWidget(m_info);

    // 删除键跟随勾选集：勾上任意一行才可用
    connect(m_tree, &QTreeWidget::itemChanged, this, [this] {
        QTreeWidgetItemIterator checked(m_tree, QTreeWidgetItemIterator::Checked);
        m_deleteBtn->setEnabled(*checked != nullptr);
    });
}

QTreeWidgetItem *MediaLibraryCard::addRow(QTreeWidgetItem *parent, const QString &name,
                                          const QString &type, const QString &path,
                                          bool checkable, bool bold)
{
    auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
    item->setText(0, name);
    item->setText(1, type);
    item->setData(0, PathRole, path);
    // 刻意不挂 Qt::ToolTipRole：列表项鼠标一停就弹整条路径，扫列表时很吵(2026-09-23 用户要求)。
    // 名称列被省略号截断时也不再补 tooltip —— 想看全路径把列拉宽即可。
    // 注意 path 本身还要用(PathRole)，别把上面的 setData 一起删了。
    if (bold) {
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
    }
    if (checkable) {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);
    }
    return item;
}

QStringList MediaLibraryCard::checkedPaths() const
{
    QStringList paths;
    QTreeWidgetItemIterator it(m_tree, QTreeWidgetItemIterator::Checked);
    while (*it) {
        paths << (*it)->data(0, PathRole).toString();
        ++it;
    }
    return paths;
}

// 三段固定顺序：统计信息 · 状态 · 播放数据名。空段跳过。
// ⚠️ 分隔符必须是 QStringLiteral，**不能用 QLatin1String**：里面的 · 是非 ASCII，
// 窄字面量会被编成 UTF-8 两字节(C2 B7)，再被当 Latin-1 逐字节解 → 界面上显示 "Â·"。
void MediaLibraryCard::setInfo(const QString &stats, const QString &state, const QString &name)
{
    if (!m_info)
        return;
    QStringList parts;
    for (const QString &s : {stats, state, name}) {
        if (!s.isEmpty())
            parts << s;
    }
    m_info->setText(parts.join(QStringLiteral(" · ")));
}

void MediaLibraryCard::setLibraryEnabled(bool on)
{
    m_tree->setEnabled(on);
    m_scanBtn->setEnabled(on);
    if (!on)
        m_deleteBtn->setEnabled(false);
}

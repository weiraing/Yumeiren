#include "ui/LibraryCard.h"

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
    help->setToolTip(tooltipstyle::format(helpText));
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

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("HintLabel"));
    lay->addWidget(m_status);

    auto *note = new QLabel(noteText, this);
    lay->addWidget(note);

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
    item->setToolTip(0, QDir::toNativeSeparators(path));
    item->setToolTip(1, item->toolTip(0));
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

void MediaLibraryCard::setStatus(const QString &text)
{
    if (m_status)
        m_status->setText(text);
}

void MediaLibraryCard::setLibraryEnabled(bool on)
{
    m_tree->setEnabled(on);
    m_scanBtn->setEnabled(on);
    if (!on)
        m_deleteBtn->setEnabled(false);
}

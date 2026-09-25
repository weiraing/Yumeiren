/**
 * @file CardGridDelegate.h
 * @brief 图标网格的「卡片」单元格绘制 —— 上图标、下分隔线、底部居中文件名。
 */

#ifndef CARDGRIDDELEGATE_H
#define CARDGRIDDELEGATE_H

#include <QApplication>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>

/**
 * @brief 网格里的一张「卡片」：整块画底、上面居中出图、底部一条分隔线 + 文件名。
 *
 * 图片浏览网格与看板娘模型网格长相一样，原本各写了一份**几乎逐行相同**的 paint()；
 * 两处唯一的差别是底部文字区高度如何取（一个固定 28px，一个随字体行高自适应）。
 * 抽到这里，子类只要回答一件事：`footerHeight()`。
 *
 * 派生类若需要别的字号，重写 `initStyleOption()` 调基类后改 `font` 即可。
 */
class CardGridDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        // 图标与文字都自己画，所以先把基类的绘制项去掉，只借它画底/选中态。
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
        const int footer = footerHeight(opt.fontMetrics);
        const int dividerY = card.bottom() - footer;
        const QRect imageRect(card.left() + 4, card.top() + 4,
                              card.width() - 8, dividerY - card.top() - 8);
        icon.paint(painter, imageRect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        // 半透明灰而非具体颜色：两套主题下写死任何一色都会在另一套上显脏。
        painter->setPen(QColor(128, 128, 128, 65));
        painter->drawLine(card.left(), dividerY, card.right(), dividerY);

        const QRect textRect(card.left() + 4, dividerY + 1, card.width() - 8, footer - 1);
        const QString text = opt.fontMetrics.elidedText(name, Qt::ElideRight, textRect.width());
        style->drawItemText(painter, textRect, Qt::AlignCenter, opt.palette,
                            opt.state & QStyle::State_Enabled, text, QPalette::Text);
        painter->restore();
    }

protected:
    // 底部文字区高度(px)。默认随字体行高 —— 字号被放大时不至于把字切掉。
    virtual int footerHeight(const QFontMetrics &fm) const
    {
        return qMax(28, fm.height() + 8);
    }
};

#endif // CARDGRIDDELEGATE_H

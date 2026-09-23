#ifndef UISTYLE_H
#define UISTYLE_H

#include <QStyle>
#include <QWidget>

// 界面侧的小工具集。只放跨页面复用、又不值得为此开 .cpp 的东西。
namespace uistyle {

// QSS 选择器带动态属性时(QLabel#StatusChip[data-ok="1"] 这类)，改完属性值必须让
// Qt 重新求值样式，外观才会真的换 —— unpolish+polish 一对就是干这个的。
// 各页面上十来处同一段样板，收口到这里，顺带统一判空。
inline void restyleWidget(QWidget *widget)
{
    if (!widget)
        return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

} // namespace uistyle

#endif // UISTYLE_H

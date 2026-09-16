#ifndef TOOLTIPSTYLE_H
#define TOOLTIPSTYLE_H

#include <QString>

// 悬浮提示文本排版，与 style.qss / light.qss 里的 QToolTip 规则配套。
//
// 为什么需要它：Qt 的 QTipLabel 只在"一行宽度超过整块屏幕"时才自动换行
// (updateSize 里 setWordWrap 的条件是 width() > screenWidth)，所以一句较长
// 的中文提示会拉成上千像素宽的横条；再配上系统默认的黄色气泡，就是任务书
// 截图里那种"古老"观感。分工是：配色/圆角/描边交给 QSS，宽度交给这里 ——
// 在设置文本时按提示字体实测宽度插入硬换行，浮层就能保持卡片比例。
//
// 用硬换行而不是 setWordWrap(true) 的原因：QTipLabel::setText 会在
// QLabel::setText 之后显式执行 setWordWrap(width() > 16777215)，把换行标志
// 关掉，外部改的 wordWrap 与尺寸都会被它覆盖；而 sizeHint 是按 '\n' 逐行取
// 最宽者算出来的，硬换行因此能被 Qt 自身的布局逻辑稳定复现(含提示框已可见、
// 只切换文本的那条路径)。
namespace tooltipstyle {

// 提示浮层的最大可视宽度(逻辑像素，含 QSS 的 padding 与 1px 描边)。
inline constexpr int kMaxTipWidth = 380;

// 按 kMaxTipWidth 折行。原文里已有的换行全部保留，只对超宽的行插入新行；
// 优先在中文标点/右括号后断行，其次任意两个汉字之间，ASCII 连续段(路径、
// 文件名、"1080p30"、"GPU")内部绝不拆断。无需折行时原样返回。
QString format(const QString &text);

} // namespace tooltipstyle

#endif // TOOLTIPSTYLE_H

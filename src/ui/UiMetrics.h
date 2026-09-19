// 页面级尺寸的单一出处。
//
// 动态壁纸页与看板娘页是同一种左右布局：左边固定宽的控制卡，右边 addWidget(..., 1)
// 吃掉剩余宽度。两列宽度集中在一处，切页时左卡才不会横向跳。
#ifndef UIMETRICS_H
#define UIMETRICS_H

namespace uimetrics {

// 左右布局页面的左列宽度(逻辑像素)。调大它是从右列匀宽度过来，窗口总宽不变。
inline constexpr int kPageLeftColWidth = 300;

} // namespace uimetrics

#endif // UIMETRICS_H

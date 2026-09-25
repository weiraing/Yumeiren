/**
 * @file PixelWheelGrid.h
 * @brief 按固定像素滚动的图标网格 —— 替掉 Qt 默认的「一次滚两三格」。
 */
#ifndef PIXELWHEELGRID_H
#define PIXELWHEELGRID_H

#include <QListWidget>
#include <QScrollBar>
#include <QWheelEvent>

/**
 * @brief 滚轮按固定像素滚的网格。与 QListWidget 的唯一差别在 wheelEvent。
 *
 * 用在看板娘模型网格与图片浏览网格上 —— 两处的格子都够大，一屏只放得下两三格。
 * 只用在 GUI 线程(继承 QWidget 的亲和性)，无跨线程调用。
 */
class PixelWheelGrid final : public QListWidget
{
public:
    explicit PixelWheelGrid(QWidget *parent = nullptr) : QListWidget(parent) {}

    // 一次滚轮滚多少像素：半格。一屏两格 ⇒ 滚四下到底，既有明显位移又不跳。
    //
    // 为什么不能直接吃 Qt 的默认值：ScrollPerPixel + IconMode 下，一次滚轮的位移是
    // 「3 × singleStep，再被 pageStep 封顶」(鼠标默认一次滚 3 行 = 3 格)。而 Qt 按布局把
    // singleStep 算成「一格」量级 —— 实测模型墙 204px、图片浏览 102px，于是旧行为一次滚
    // 3 格：模型墙 3×204=612 被截成 416(整整一屏 2 格)、图片浏览 306(3 格)。格子大、一屏
    // 只放得下两三格时，稍微一滚就全甩过去。
    //
    // 不去 setSingleStep 较劲：那个值由 Qt 每次布局重算，跟着它跑等于把滚动量绑在它的
    // 内部算法上。这里直接按像素滚，步长只由格子尺寸决定。
    //
    // 现读 gridSize 而不是构造时定死：图片浏览网格的格子尺寸随视口宽度动态算(updateGalleryGrid)。
    static int stepFor(const QSize &gridSize) { return qMax(1, gridSize.height() / 2); }

    // 把一次滚轮换算成要滚的像素数。鼠标给的是 angleDelta(120 算一档)，触摸板给的是
    // pixelDelta(本身已是像素) —— 后者必须按原值滚，否则轻轻一滑就跳掉一整步。
    static int pixelsFor(const QWheelEvent *event, int step)
    {
        const QPoint pixels = event->pixelDelta();
        if (!pixels.isNull())
            return pixels.y();
        return qRound(event->angleDelta().y() * double(step) / QWheelEvent::DefaultDeltasPerStep);
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        QScrollBar *bar = verticalScrollBar();
        const int step = pixelsFor(event, stepFor(gridSize()));
        // 只在「真能滚」且方向是垂直时接管：内容不足一屏、纯水平档位都还给 Qt。
        if (step == 0 || !bar || bar->maximum() <= bar->minimum()) {
            QListWidget::wheelEvent(event);
            return;
        }
        const int before = bar->value();
        bar->setValue(before - step);
        if (bar->value() == before) {
            // 已经到顶/到底，这一下没滚成 —— 交回 Qt，让它按惯例把事件冒泡给外层滚动区，
            // 否则「网格滚到头」会连带把整页的滚动一起卡住。
            QListWidget::wheelEvent(event);
            return;
        }
        event->accept();
    }
};

#endif // PIXELWHEELGRID_H

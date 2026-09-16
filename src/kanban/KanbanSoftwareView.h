// 看板娘软件绘制视图：占位渲染器的承载控件。
//
// 为什么窗口不自己画：KanbanWindow 是「帧 + 原生样式」的所有者(置顶、穿透、
// TaskbarCreated)，而画面要能在两种后端之间切换 —— Live2D 必须活在
// QOpenGLWidget 的 GL 上下文里，占位动画则走 QPainter。两种宿主控件的基类不同
// (QWidget / QOpenGLWidget)，没法靠继承统一，所以窗口持有一个子视图，
// 交互事件用事件过滤器收在窗口一处处理，两个视图都零交互代码。
//
// 禁止事项(任务书 §5.1)：本类不得每帧生成 QImage/QPixmap，更不得 grab() 截图。
// 绘制直接落在窗口的 backing store 上，脏区由 update() 决定。
#ifndef KANBANSOFTWAREVIEW_H
#define KANBANSOFTWAREVIEW_H

#include <QWidget>

namespace kanban {

class KanbanRenderer;

class KanbanSoftwareView : public QWidget
{
    Q_OBJECT

public:
    explicit KanbanSoftwareView(QWidget *parent = nullptr);

    void setRenderer(KanbanRenderer *renderer);
    KanbanRenderer *renderer() const { return m_renderer; }

    // 控制器每帧调用：只标脏，不立即绘制。
    void requestFrame();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    KanbanRenderer *m_renderer = nullptr;
};

} // namespace kanban

#endif // KANBANSOFTWAREVIEW_H

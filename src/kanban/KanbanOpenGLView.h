// 看板娘 OpenGL 宿主视图：Live2D 后端的绘制面，仅 YUMEIREN_WITH_LIVE2D=ON 时编译。
//
// 为什么必须单独一个 QOpenGLWidget：Cubism 的纹理/着色器资源只能由持有当前上下文的
// 线程创建与释放，而 initializeGL / paintGL / shutdownGL 正好给出这三个时机。
// 本类不含任何 Cubism 头文件，只认 KanbanRenderer 抽象。
#ifndef KANBANOPENGLVIEW_H
#define KANBANOPENGLVIEW_H

#include <QOpenGLWidget>

#include "kanban/KanbanRenderer.h" // KanbanGlHost 基类需要完整定义

namespace kanban {

class KanbanRenderer;

class KanbanOpenGLView : public QOpenGLWidget, public KanbanGlHost
{
    Q_OBJECT

public:
    explicit KanbanOpenGLView(QWidget *parent = nullptr);
    ~KanbanOpenGLView() override;

    void setRenderer(KanbanRenderer *renderer);
    KanbanRenderer *renderer() const { return m_renderer; }

    // —— KanbanGlHost：渲染器在 paintGL 之外取用上下文的入口 ——
    bool glHostReady() const override;
    bool glIsCurrent() const override;
    bool glMakeCurrent() override;
    void glDoneCurrent() override;
    QSize glPixelSize() const override;
    quint64 glContextGeneration() const override;

signals:
    // GL 上下文已在当前线程就绪：控制器此时才允许 initialize + loadModel。
    void contextReady();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    KanbanRenderer *m_renderer = nullptr;
    // 首帧 paintGL 只记一次日志：这是「上下文建好了」到「画面真的出来了」的分界点。
    bool m_firstPaintDone = false;
    // 本视图承载的 GL 上下文世代号，在 initializeGL 里领一次。渲染器靠它认出
    // 「Cubism 的进程级着色器缓存属于上一个上下文、已作废」。
    quint64 m_contextGeneration = 0;
};

} // namespace kanban

#endif // KANBANOPENGLVIEW_H

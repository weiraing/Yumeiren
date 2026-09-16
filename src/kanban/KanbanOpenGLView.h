// 看板娘 OpenGL 宿主视图：Live2D 后端的绘制面。
//
// 仅在 YUMEIREN_WITH_LIVE2D=ON 时参与编译(见 CMakeLists.txt)：默认构建不引入
// Qt6::OpenGLWidgets 依赖，产物 DLL 需求与接入前完全一致。
//
// 为什么必须单独一个 QOpenGLWidget：Cubism 的 glDeleteTextures / 着色器对象等
// 资源只能在拥有当前上下文的线程上创建与释放(任务书 §4.3)。QOpenGLWidget 的
// initializeGL / paintGL / shutdownGL 正好给出这三个时机，于是 SDK 的
// initialize() 走 contextReady、shutdown() 走 shutdownGL，绝不在别处碰 GL。
//
// 本类不含任何 Cubism 头文件：它只认 KanbanRenderer 抽象，SDK 类型一律留在
// Live2DRenderer.cpp 里(任务书 §17 第 2 条)。
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

signals:
    // GL 上下文已在当前线程就绪：控制器此时才允许 initialize + loadModel。
    void contextReady();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    KanbanRenderer *m_renderer = nullptr;
    bool m_contextReadySent = false;
};

} // namespace kanban

#endif // KANBANOPENGLVIEW_H

// 看板娘窗口：无边框 + 逐像素透明的常驻小窗(任务书 §4.1 / §6)。
//
// 分工：本类只管「帧、原生窗口样式、用户意图」，画面由子视图承担 ——
//   · 占位渲染器 → KanbanSoftwareView(QWidget + QPainter)
//   · Live2D     → KanbanOpenGLView(QOpenGLWidget，需要活的 GL 上下文)
// 两者的基类不同，没法用继承统一，所以窗口按 renderer->usesOpenGL() 选型，
// 再用事件过滤器把鼠标交互收在一处：换后端不改交互，交互不改渲染。
//
// 三条硬约束：
//   · 不进任务栏、不抢焦点(Qt::Tool + WA_ShowWithoutActivating)：看板娘出现在
//     桌面上不该打断用户正在做的事；
//   · 关闭 = 隐藏：closeEvent 里 ignore 后发 hideRequested，销毁只由控制器
//     stop() 负责，于是反复启停不重建窗口(任务书 §13.1 对象数不增长)；
//   · 本类不计时：动画推进由控制器把统一时钟接到视图上，窗口不参与。
#ifndef KANBANWINDOW_H
#define KANBANWINDOW_H

#include <QPoint>
#include <QWidget>

namespace kanban {

class KanbanRenderer;

class KanbanWindow : public QWidget
{
    Q_OBJECT

public:
    explicit KanbanWindow(QWidget *parent = nullptr);
    ~KanbanWindow() override;

    // 按渲染器类型建/换子视图。GL 后端必须等上下文就绪才能 initialize()，
    // 所以这里不初始化渲染器，只负责把视图搭起来。
    void attachRenderer(KanbanRenderer *renderer);
    void detachRenderer();
    KanbanRenderer *renderer() const { return m_renderer; }
    QWidget *viewWidget() const { return m_view; }
    bool usesOpenGLHost() const { return m_glHost; }

    // 每帧标脏(控制器调用)。
    void requestFrame();

    void setAlwaysOnTop(bool onTop);
    bool alwaysOnTop() const { return m_alwaysOnTop; }

    // 鼠标穿透：WS_EX_TRANSPARENT，窗口对鼠标隐形(仍可被右键菜单关掉)。
    void setMouseThrough(bool through);
    bool mouseThrough() const { return m_mouseThrough; }

    // 窗口不透明度(0.2..1.0)。用 setWindowOpacity 而不是绘制时乘 alpha：
    // 前者由合成器做，后者每帧都要重算所有颜色。
    void setOpacityPercent(int percent);
    int opacityPercent() const { return m_opacityPercent; }

    // 首次显示落位：x/y 为 -1 表示「还没摆放过」→ 贴主屏可用区右下角。
    void placeFromConfig(int x, int y, int width, int height);

    void setPausedVisual(bool paused);
    void setModelDisplayName(const QString &name);

    // 关掉互动后仍可见可拖动，只是不再响应悬停/点击/滚轮(任务书 §6.4 的配置项)。
    void setInteractionEnabled(bool enabled);
    bool interactionEnabled() const { return m_interactionEnabled; }

signals:
    // —— 用户意图，一律交给控制器处置(窗口不判断「能不能暂停」) ——
    void pointerEntered();
    void pointerLeft();
    void hoveredAt(const QPointF &localPos);
    void clicked(const QPointF &localPos);
    void dragStarted();
    void dragFinished();          // 需要持久化位置
    void scaleStepped(int steps); // 滚轮：+1 放大 / -1 缩小
    void pauseResumeRequested();
    void playNextRequested();
    void nextExpressionRequested();
    void nextModelRequested();
    void hideRequested();
    void settingsRequested();
    void quitRequested();         // 「取消看板娘」
    // GL 后端：上下文已在渲染线程就绪，控制器此时才能 initialize()+loadModel()。
    void glContextReady();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void ensureView();
    // 让子视图把手里的渲染器指针放掉。视图是 deleteLater 的、比窗口多活一会儿，
    // 期间任何一次绘制都会用到这个指针 —— 见实现处的说明。
    void releaseViewRenderer();
    void applyTopmostStyle();
    void applyMouseThroughStyle();

    KanbanRenderer *m_renderer = nullptr;
    QWidget *m_view = nullptr;
    bool m_glHost = false;
    bool m_alwaysOnTop = true;
    bool m_mouseThrough = false;
    bool m_interactionEnabled = true;
    bool m_pausedVisual = false;
    int m_opacityPercent = 100;

    // 拖动与「点击还是拖动」判定
    bool m_pressSeen = false;
    bool m_dragging = false;
    QPoint m_dragOffset;
    QPoint m_pressGlobal;
    quintptr m_taskbarCreatedMsg = 0;
};

} // namespace kanban

#endif // KANBANWINDOW_H

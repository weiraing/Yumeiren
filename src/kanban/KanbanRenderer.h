// 看板娘渲染后端抽象(任务书 §4.1 KanbanRenderer)。
//
// 接口分两条绘制路径，两条都禁止「渲染→截图→QImage→QPixmap→QLabel」这种
// 每帧位图搬运(任务书 §5.1 明令禁止)：
//   · 软件路径：paint(QPainter*) —— 占位渲染器用，直接画进窗口自己的 paintEvent，
//     全程不生成中间位图；
//   · GPU 路径：render() —— Live2D/OpenGL 后端用，渲染进窗口自身的 GL 上下文，
//     由 swapBuffers 上屏；此时 paint() 不会被调用。
// 选哪条路径由后端在 supportsOpenGL() 里表态，窗口据此决定自己是不是
// QOpenGLWidget 宿主。
#ifndef KANBANRENDERER_H
#define KANBANRENDERER_H

#include <QPointF>
#include <QString>
#include <QSize>

class QPainter;

namespace kanban {

// GL 宿主能力：让渲染器能在「非 paintGL 时机」把 GL 上下文取回来。
//
// 为什么需要它：QOpenGLWidget 只在 initializeGL/resizeGL/paintGL 里替我们
// 当前化上下文，而模型装载、换模型这些动作发生在定时器/设置变更里，
// 此刻 glGenTextures 会静默失败。Qt 6 的 QOpenGLWidget 有公开 makeCurrent，
// 所以由视图实现这三个方法，渲染器用完即 doneCurrent，不长期持有上下文。
// 接口刻意不出现任何 Qt GL 类型：软件后端与非 GL 构建零成本。
struct KanbanGlHost
{
    virtual ~KanbanGlHost() = default;

    // 上下文是否已经创建(视图尚未首绘时为 false)。
    virtual bool glHostReady() const = 0;
    // 本宿主的上下文此刻是否已在当前线程当前化。paintGL 期间恒为 true，
    // 此时既不能重复 makeCurrent 更不能 doneCurrent，否则视图自己的绘制状态被拆。
    virtual bool glIsCurrent() const = 0;
    // 借上下文：成功返回 true。用完必须与 glDoneCurrent 成对。
    virtual bool glMakeCurrent() = 0;
    virtual void glDoneCurrent() = 0;
    // 绘制面尺寸，设备像素(=Cubism 渲染器/FBO 需要的真实像素)。
    virtual QSize glPixelSize() const = 0;
};

class KanbanRenderer
{
public:
    virtual ~KanbanRenderer() = default;

    // 后端标识，写日志与界面提示用(如 "Live2D Cubism" / "Placeholder")。
    // 刻意叫 backendName 而不是 name：Qt 对象树里 name() 是 objectName 访问器，
    // 渲染器将来可能同时是 QObject，撞名会留下难查的隐藏(hiding)坑。
    virtual QString backendName() const = 0;

    // 创建渲染所需资源。Live2D 后端在这里初始化 Cubism Framework 与 GL 上下文；
    // 失败返回 false，outError 给出人能看懂的原因，调用方切降级方案。
    virtual bool initialize(QString *outError) = 0;

    // 模型装载：fileSystemPath 指向 .model3.json。返回 false 表示模型不可用，
    // 但后端必须保持可用(可继续装载别的模型)，不得进入不可恢复状态。
    virtual bool loadModel(const QString &modelJsonPath, QString *outError) = 0;
    virtual void unloadModel() = 0;

    // 是否必须跑在 OpenGL 宿主里。true 时窗口以 QOpenGLWidget 作承载。
    virtual bool usesOpenGL() const = 0;

    // 尺寸变化(逻辑像素 + DPR)。
    virtual void resize(int width, int height, float devicePixelRatio) = 0;

    // 推进一帧动画：只更新参数，不绘制。deltaSeconds 来自统一动画时钟的真实间隔。
    virtual void update(float deltaSeconds) = 0;

    // GPU 路径的绘制入口(软件后端在此什么都不做)。
    virtual void render() = 0;

    // 软件路径的绘制入口；painter 已裁剪到窗口客户区，坐标系为逻辑像素。
    virtual void paint(QPainter *painter, const QSize &logicalSize) = 0;

    // GL 宿主(仅 Live2D 后端需要)。软件后端忽略即可。
    virtual void setGlHost(KanbanGlHost *host) { Q_UNUSED(host) }

    // —— 交互 ——
    // 鼠标移动(逻辑像素，窗口坐标)：Live2D 后端据此做视线/头部跟随。
    virtual void pointerMove(const QPointF &pos) = 0;
    // 点击(逻辑像素)：触发一次性动作。
    virtual void pointerClick(const QPointF &pos) = 0;
    // 播放下一个动作；返回动作名表示确实播了，返回 false 表示没有可播动作。
    virtual bool playNextMotion() = 0;

    virtual void pause() = 0;
    virtual void resume() = 0;
    // 释放全部后端资源；调用后允许再次 initialize()(允许用户重试)。
    virtual void shutdown() = 0;
};

} // namespace kanban

#endif // KANBANRENDERER_H

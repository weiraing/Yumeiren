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

    // GL 上下文的世代号：宿主每新建一次上下文递增一次，进程内单调、不复用。
    //
    // 为什么渲染器需要知道这件事：Cubism 有一份**进程级**的全局 GL 缓存
    // (CubismShader_OpenGLES2 单例，里面存的是着色器 program id)，而 program
    // 只在创建它的那个上下文里有效。看板娘「取消 → 启用」会换一个全新上下文，
    // 单例却还活着 —— 没有世代号，渲染器就分不清「同一个上下文又用了一次」和
    // 「换上下文了」，而后者必须把那份缓存丢掉重建，否则新上下文沿用死 id，
    // glUseProgram 静默失败，桌面上什么都没有。
    //
    // 刻意不用 QOpenGLContext* 当身份：对象释放后地址会被复用，复用一次就是
    // 「换了上下文却当成没换」，正是最难查的那种偶发故障。
    // 返回 0 表示「还没有 GL 上下文」——软件宿主与尚未首绘的 GL 视图都走
    // 这条默认实现，它们没有上下文，也就没有「缓存作废」可言。
    virtual quint64 glContextGeneration() const { return 0; }
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
    // pointerMove(逻辑像素，窗口坐标)：Live2D 后端据此做视线/头部跟随。
    //
    // 传进来的应当是**光标相对窗口的坐标**，而不是「鼠标是否压在窗口上」。看板娘
    // 是个小窗，用户大多数时候光标都在窗外 —— 若只喂窗口内坐标，「视线追踪」就
    // 退化成「鼠标划过的一瞬间转一下眼」。所以由控制器把全局光标换算成本窗口
    // 坐标（范围允许超出 [0,size)，这正是能把「在左边 / 在上方」表达清楚的原因），
    // 窗口不做任何判断。映射细节见 Live2DRenderer::pointerMove。
    virtual void pointerMove(const QPointF &pos) = 0;
    // 点击(逻辑像素)：触发一次性动作。
    virtual void pointerClick(const QPointF &pos) = 0;
    // 播放下一个动作；返回 true 表示确实播了，返回 false 表示没有可播动作。
    virtual bool playNextMotion() = 0;

    // —— 视线追踪开关 ——
    // 关掉后指针移动不再改变头/眼角度，模型回到正面。
    //
    // 为什么放在渲染器而不是窗口：能不能「看向某处」是后端能力(Live2D 靠
    // CubismLook 把归一化坐标映射到 ParamAngleX/EyeBallX 等参数，占位后端靠自己
    // 那几个字段)，而「什么时候该看哪里」才是交互策略。能力留在渲染器、策略留在
    // 控制器，两边不混。
    // 关掉时必须把角度**复位**而不是保留最后那个值，否则用户一关开关，模型就
    // 僵在一个歪头的姿势上。
    virtual void setGazeEnabled(bool enabled) { m_gazeEnabled = enabled; }
    bool gazeEnabled() const { return m_gazeEnabled; }

protected:
    // 默认实现只写这个字段，派生类在 pointerMove / update 里自行判断。
    bool m_gazeEnabled = true;

public:

    // playableMotionCount() 返回「可播动作」数 —— **不含 idle 组**。
    //
    // 为什么不算 idle：idle 是待机循环，本来就一直在播。把它算进来，
    // 「下一个」就永远有得播，而用户点下去看到的是同一段待机 —— 那正是
    // 这个入口要避免的「点了跟没点一样」。
    // 按模型文件决定的后端必须给真值：实测 13 个模型里有 8 个的可播动作不足 2 个。
    virtual int playableMotionCount() const { return 0; }

    // 「播放下一个动作」这个入口该不该可点：至少要有两个可播动作。
    //
    // 只有一个时，「下一个」就是原地重播同一段 —— 用户点了看不见任何变化，
    // 只会以为程序坏了，所以一个也算「不可播」。
    // 刻意收成基类里的一个派生判据，而不是让各处自己写 playableMotionCount() >= 2：
    // 右键菜单、设置页按钮、托盘三条入口各写一遍，迟早漏掉一处。
    bool canPlayNextMotion() const { return playableMotionCount() >= 2; }

    // —— 表情 ——
    // 与动作分开是刻意的：Cubism 里表情走 ExpressionMotionManager、动作走
    // MotionManager，两者互不抢占优先级，所以「切表情」不该打断正在播的动作。
    //
    // expressionCount() 返回 0 表示「本后端/本模型没有表情」。界面拿它把入口
    // 置灰 —— 一个点了没反应的菜单项，比一个灰掉的菜单项更让人怀疑程序坏了。
    // 两个后端都要给真值：占位后端也有几个写死的表情，降级路径的功能面
    // 不该因为后端不同而缩水。
    virtual int expressionCount() const { return 0; }
    // 切到下一个表情(循环)。返回 false 表示没有可切的表情，调用方静默返回即可。
    virtual bool playNextExpression() { return false; }

    virtual void pause() = 0;
    virtual void resume() = 0;
    // 释放全部后端资源；调用后允许再次 initialize()(允许用户重试)。
    virtual void shutdown() = 0;
};

} // namespace kanban

#endif // KANBANRENDERER_H

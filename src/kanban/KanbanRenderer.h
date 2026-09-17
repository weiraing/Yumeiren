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

// —— 视线强度档位到「实际幅度」的唯一映射表 ——
//
// 放在抽象层而不是各后端各写一份：两个后端必须给出**逐位一致**的手感，
// 否则用户在降级路径(占位渲染器)上调好了档位，切回 Live2D 会发现同一档
// 效果不一样。这类「两处常量要手工同步」的约定，写在一起才守得住。
//
// 两个系数的含义：
//   radiusFactor —— 作用半径 = 窗口尺寸 × 该系数。档位越高系数越小，
//                   即「转过同样角度所需的鼠标偏移更小」= 更敏感。
//                   这不是「幅度上限」：CubismLook 的最终幅度由参数范围
//                   (ParamAngleX ±30、EyeBall ±1)封顶，档位改的是**到达
//                   上限的快慢**，也就是「窗口附近动一动就有反应」的程度。
//   verticalScale —— 纵向收敛，生理上抬头幅度小于左右摆，观感更自然。
struct GazeTuning {
    float radiusFactor;
    float verticalScale;
};

// 索引即档位(GazeOff 也占一格，值无意义 —— 关闭时根本不查表)。
//
// 三档的半径倍数取 3.6 / 2.2 / 1.5：
//   · 中档 2.2 是此前实测满意的值，作为基准不动；
//   · 弱档 3.6 让「鼠标要移开挺远才看得出转头」，安静不打扰；
//   · 强档 1.5 在窗口外一小段距离就接近最大转角，存在感最强。
// 强档刻意**不**再往下压(试过 1.3)：那样在 230px 宽的窗口上，光标离中心
// 150px 就彻底饱和，屏幕上绝大多数位置看到的都是同一个「顶到极限」的姿势 ——
// 用户会觉得「强档 = 卡住了」，而不是「跟得紧」。留一点行程才有强弱之分。
inline constexpr GazeTuning kGazeTuning[4] = {
    {2.2f, 0.70f}, // GazeOff   —— 占位，不参与计算
    {3.6f, 0.55f}, // GazeWeak  —— 半径大得多，轻微跟随；纵向更收
    {2.2f, 0.70f}, // GazeMedium
    {1.5f, 0.85f}, // GazeStrong—— 追得紧，纵向几乎不压
};

// 作用半径下限(逻辑像素)。缩放调到 20% 时窗口只有几十像素，纯按倍数算出的
// 半径太小，光标轻微抖动就会让模型疯狂偏头 —— 用一个地板值兜住。
inline constexpr float kGazeMinRadiusPx = 160.0f;

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

    // —— 视线追踪强度 ——
    //
    // 四档枚举，「无」= 关闭，其余三档是同一套映射换不同的作用半径。
    //
    // 为什么是「强度」而不是「开关 + 单独调灵敏度滑块」：用户对这功能的诉求是
    // 「让它看着我」，他会描述成「有点弱 / 挺明显 / 太夸张了」—— 这是一条
    // 一维的档位，不是两个正交参数。做成四选一既能关掉，又把「多明显」收进
    // 一个说得清的刻度里，比两个控件更好选。
    //
    // 为什么档位语义放在渲染器而不是窗口：能不能「看向某处」是后端能力(Live2D
    // 靠 CubismLook 把归一化坐标映射到 ParamAngleX/EyeBallX 等参数，占位后端靠
    // 自己那几个字段)，而「什么时候该看哪里」才是交互策略。能力留在渲染器、
    // 策略留在控制器，两边不混。
    //
    // 档位→实际幅度的映射表定义在 KanbanGaze 命名空间(本头文件下方)，两个后端
    // 共用同一张表 —— 否则降级路径下手感会和 Live2D 路径分叉，用户会以为
    // 「升级后变迟钝了」。
    enum GazeStrength {
        GazeOff = 0,   // 无：不跟随，模型保持正面
        GazeWeak = 1,  // 弱
        GazeMedium = 2, // 中
        GazeStrong = 3, // 强
    };

    // 设置强度。传 GazeOff 时必须把角度**复位**而不是保留最后那个值，
    // 否则用户一关，模型就僵在一个歪头的姿势上(见两个后端的实现)。
    virtual void setGazeStrength(int strength);
    int gazeStrength() const { return m_gazeStrength; }

    // 「有没有开」是强度 > 0 的派生判据。刻意做成函数而不是让各处自己写
    // `strength != 0`：这条判断在控制器、两个后端、三处界面入口都要用，
    // 各写各的迟早漏一处。
    bool gazeEnabled() const { return m_gazeStrength != GazeOff; }

    // 档位名(「无/弱/中/强」)，界面与日志共用一处，避免两处译名不一致。
    static QString gazeStrengthName(int strength);
    // 把任意数值夹进合法档位。配置是从磁盘读的，可能是旧版遗留的布尔值或脏数据。
    static int clampGazeStrength(int strength);

protected:
    // 默认实现只写这个字段，派生类在 pointerMove / update 里自行判断。
    int m_gazeStrength = GazeMedium;

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

// 这三个在头文件里内联定义：它们只碰一个 int 和一个静态表，没有需要藏起来的
// 实现细节，放进 .cpp 反而要多一个 translation unit(且两份后端实现都要链接它)。
inline int KanbanRenderer::clampGazeStrength(int strength)
{
    if (strength < GazeOff)
        return GazeOff;
    if (strength > GazeStrong)
        return GazeStrong;
    return strength;
}

inline void KanbanRenderer::setGazeStrength(int strength)
{
    m_gazeStrength = clampGazeStrength(strength);
}

inline QString KanbanRenderer::gazeStrengthName(int strength)
{
    switch (clampGazeStrength(strength)) {
    case GazeWeak:
        return QStringLiteral("弱");
    case GazeMedium:
        return QStringLiteral("中");
    case GazeStrong:
        return QStringLiteral("强");
    case GazeOff:
    default:
        return QStringLiteral("无");
    }
}

} // namespace kanban

#endif // KANBANRENDERER_H

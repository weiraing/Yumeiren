// 看板娘渲染后端抽象。
//
// 两条绘制路径：软件路径 paint() 直接画进窗口 paintEvent；GPU 路径 render() 渲染进
// 窗口自身的 GL 上下文。选哪条由后端在 usesOpenGL() 里表态。
// 两条都禁止「渲染→截图→QImage→QPixmap→QLabel」这种每帧位图搬运。
#ifndef KANBANRENDERER_H
#define KANBANRENDERER_H

#include <QPointF>
#include <QString>
#include <QSize>

class QPainter;

namespace kanban {

// 视线强度档位到「实际幅度」的唯一映射表。两个后端共用，否则降级路径下手感会分叉。
// radiusFactor —— 作用半径 = 窗口尺寸 × 该系数，越小越敏感（改的是到达上限的快慢，
//                 不是幅度上限，上限由 CubismLook 的参数范围封顶）。
// verticalScale —— 纵向收敛，抬头幅度小于左右摆更自然。
struct GazeTuning {
    float radiusFactor;
    float verticalScale;
};

// 索引即档位(GazeOff 也占一格，值无意义 —— 关闭时根本不查表)。
inline constexpr GazeTuning kGazeTuning[4] = {
    {2.2f, 0.70f}, // GazeOff   —— 占位，不参与计算
    {3.6f, 0.55f}, // GazeWeak
    {2.2f, 0.70f}, // GazeMedium
    {1.5f, 0.85f}, // GazeStrong
};

// 作用半径下限(逻辑像素)：缩放很小时纯按倍数算出的半径太小，光标轻微抖动就会疯狂偏头。
inline constexpr float kGazeMinRadiusPx = 160.0f;

// 纹理限幅的地板值：再往下压省不出多少，却会让窗口稍微变大就得重解一遍素材。
inline constexpr int kTextureMaxDimFloor = 1024;

// 单边最多缩这么多倍 —— 与素材尺寸挂钩的质量底线。图集是零件打包，纯按窗口尺寸
// 等比缩会让整只角色糊掉。
inline constexpr int kTextureMaxShrink = 4;

// 由配置 kanban/textureDownscale 在读设置时写入。进程级而非渲染器成员：两个后端
// 与离屏探针问的是同一件事，不该各存一份再想办法同步。
inline bool &textureDownscaleFlag()
{
    static bool s_on = true;
    return s_on;
}

inline void setTextureDownscaleEnabled(bool on)
{
    textureDownscaleFlag() = on;
}

inline bool textureDownscaleEnabled()
{
    return textureDownscaleFlag();
}

// 窗口尺寸推出的「够用值」，恒为 2 的幂。只回答「窗口能显示多少」，
// 不回答「这张图里被用到的部分还剩多少」—— 后者要用下面那个重载。
inline int textureMaxDimFor(const QSize &pixelSize)
{
    if (!textureDownscaleEnabled() || pixelSize.isEmpty()) {
        return 0;
    }
    const int longest = qMax(pixelSize.width(), pixelSize.height());
    int maxDim = kTextureMaxDimFloor;
    while (maxDim < longest) {
        maxDim <<= 1;
    }
    return maxDim;
}

// 把「窗口够用值」与「素材自身尺寸」合起来，得到这张纹理实际该缩到多长边。
// 返回 0 = 原尺寸上传。source 只需读文件头即可，不必解码。
inline int textureMaxDimFor(int windowMaxDim, const QSize &source)
{
    if (windowMaxDim <= 0) {
        return 0; // 策略关掉 → 原尺寸
    }
    if (!source.isValid() || source.isEmpty()) {
        return windowMaxDim;
    }
    const int longest = qMax(source.width(), source.height());
    int maxDim = windowMaxDim;
    const int floorFromSource = longest / kTextureMaxShrink;
    if (floorFromSource > maxDim) {
        maxDim = floorFromSource;
    }
    if (maxDim > longest) {
        maxDim = longest; // 不放大
    }
    return maxDim;
}

// GL 宿主能力：让渲染器能在「非 paintGL 时机」把 GL 上下文取回来。
// 接口刻意不出现任何 Qt GL 类型：软件后端与非 GL 构建零成本。
struct KanbanGlHost
{
    virtual ~KanbanGlHost() = default;

    // 上下文是否已经创建(视图尚未首绘时为 false)。
    virtual bool glHostReady() const = 0;
    // 本宿主的上下文此刻是否已在当前线程当前化。paintGL 期间恒为 true，
    // 此时既不能重复 makeCurrent 更不能 doneCurrent。
    virtual bool glIsCurrent() const = 0;
    // 借上下文：成功返回 true。用完必须与 glDoneCurrent 成对。
    virtual bool glMakeCurrent() = 0;
    virtual void glDoneCurrent() = 0;
    // 绘制面尺寸，设备像素(=Cubism 渲染器/FBO 需要的真实像素)。
    virtual QSize glPixelSize() const = 0;

    // GL 上下文的世代号：宿主每新建一次上下文递增一次，进程内单调、不复用。
    // 渲染器靠它判断「换上下文了」并丢掉 Cubism 的进程级着色器缓存。
    // 返回 0 表示「还没有 GL 上下文」。
    virtual quint64 glContextGeneration() const { return 0; }
};

class KanbanRenderer
{
public:
    virtual ~KanbanRenderer() = default;

    // 后端标识，写日志与界面提示用(如 "Live2D Cubism" / "Placeholder")。
    // 刻意叫 backendName 而不是 name：渲染器将来可能同时是 QObject，撞名会隐藏基类访问器。
    virtual QString backendName() const = 0;

    // 创建渲染所需资源；失败返回 false，outError 给出人能看懂的原因，调用方切降级方案。
    virtual bool initialize(QString *outError) = 0;

    // 模型装载：modelJsonPath 指向 .model3.json。返回 false 表示模型不可用，
    // 但后端必须保持可用(可继续装载别的模型)，不得进入不可恢复状态。
    virtual bool loadModel(const QString &modelJsonPath, QString *outError) = 0;
    virtual void unloadModel() = 0;

    // 是否必须跑在 OpenGL 宿主里。true 时窗口以 QOpenGLWidget 作承载。
    virtual bool usesOpenGL() const = 0;

    virtual void resize(int width, int height, float devicePixelRatio) = 0;

    // 绘制面变大到「当前纹理已经不够细」时重建纹理，真的重建了返回 true。
    // 由帧定时器调用，不可在 resize() 内做（会重入 paintGL，见设计注记 §2.3）。
    virtual bool rebuildTexturesIfNeeded() { return false; }

    // 推进一帧动画：只更新参数，不绘制。
    virtual void update(float deltaSeconds) = 0;

    // GPU 路径的绘制入口(软件后端在此什么都不做)。
    virtual void render() = 0;

    // 软件路径的绘制入口；painter 已裁剪到窗口客户区，坐标系为逻辑像素。
    virtual void paint(QPainter *painter, const QSize &logicalSize) = 0;

    // GL 宿主(仅 Live2D 后端需要)。软件后端忽略即可。
    virtual void setGlHost(KanbanGlHost *host) { Q_UNUSED(host) }

    // —— 交互 ——
    // pos 是**光标相对窗口的坐标**(范围允许超出 [0,size))，不是「鼠标是否压在窗口上」。
    virtual void pointerMove(const QPointF &pos) = 0;
    virtual void pointerClick(const QPointF &pos) = 0;
    // 播放下一个动作；返回 true 表示确实播了，返回 false 表示没有可播动作。
    virtual bool playNextMotion() = 0;

    // 视线追踪强度：四档，「无」= 关闭。
    // 档位语义放这里(后端能力)，交互策略留控制器。理由见设计注记 §2.2。
    enum GazeStrength {
        GazeOff = 0,   // 无：不跟随，模型保持正面
        GazeWeak = 1,  // 弱
        GazeMedium = 2, // 中
        GazeStrong = 3, // 强
    };

    // 传 GazeOff 时必须把角度**复位**而不是保留最后那个值，
    // 否则用户一关，模型就僵在一个歪头的姿势上(见两个后端的实现)。
    virtual void setGazeStrength(int strength);
    int gazeStrength() const { return m_gazeStrength; }

    // 「有没有开」的派生判据。控制器、两个后端、三处界面入口都用它，别各写各的。
    bool gazeEnabled() const { return m_gazeStrength != GazeOff; }

    // 档位名(「无/弱/中/强」)，界面与日志共用一处，避免两处译名不一致。
    static QString gazeStrengthName(int strength);
    // 把任意数值夹进合法档位。配置是从磁盘读的，可能是脏数据。
    static int clampGazeStrength(int strength);

protected:
    // 默认实现只写这个字段，派生类在 pointerMove / update 里自行判断。
    int m_gazeStrength = GazeMedium;

public:

    // 「可播动作」数 —— **不含 idle 组**，理由见设计注记 §2.6。
    // 按模型文件决定的后端必须给真值：实测 13 个模型里有 8 个的可播动作不足 2 个。
    virtual int playableMotionCount() const { return 0; }

    // 这个入口该不该可点：至少要有两个可播动作(一个时就是原地重播，用户以为程序坏了)。
    bool canPlayNextMotion() const { return playableMotionCount() >= 2; }

    // —— 表情 ——
    // 与动作分开是刻意的：Cubism 里两者互不抢占优先级，「切表情」不该打断正在播的动作。
    // 返回 0 表示「本后端/本模型没有表情」，界面据此把入口置灰。
    virtual int expressionCount() const { return 0; }
    // 切到下一个表情(循环)。返回 false 表示没有可切的表情，调用方静默返回即可。
    virtual bool playNextExpression() { return false; }

    virtual void pause() = 0;
    virtual void resume() = 0;
    // 释放全部后端资源；调用后允许再次 initialize()(允许用户重试)。
    virtual void shutdown() = 0;
};

// 内联定义：只碰一个 int 和一个静态表，放进 .cpp 反而要多一个 translation unit。
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

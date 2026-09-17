// Live2D 后端的真实实现：Cubism Native SDK 5 R.5 + OpenGL。
//
// 只有 -D YUMEIREN_WITH_LIVE2D=ON 时才编译本文件，此时 Live2DRendererStub.cpp
// 整体退出构建。两份实现对同一个 Live2DRenderer.h 给出完全一致的行为契约，
// 上层(控制器/窗口/托盘/设置)因此一行都不用改。
//
// ── 三条必须守住的规矩 ────────────────────────────────────────────────
// 1. Cubism 类型不外泄：本文件不写进任何头文件，SDK 头只在这里出现，
//    否则 SDK 的许可与编译宏会传染整个工程(任务书 §17 第 2 条)。
// 2. GL 资源只在宿主上下文里创建/释放(§4.3)：所有 gl* 调用都包在 GlScope 内，
//    上下文由 KanbanOpenGLView 通过 KanbanGlHost 借出，用完立刻归还。
// 3. 装载/卸载成对(§13.2)：模型、纹理、渲染器、离屏管理器逐一对应释放，
//    重复 start/stop 不允许累积。刻意不调用 CubismFramework::Dispose()：
//    框架的 IdManager 与内存池是进程级单例，StartUp 幂等提前返回，
//    反复起停若配对 Dispose 反而会把 Option 指针解绑(它只存指针不拷贝)。
//
// ── 一处 SDK 事实值得记住 ────────────────────────────────────────────
// CubismRenderer_OpenGLES2 在首次 DoDrawModel 前用 wglGetProcAddress 抓约 40 个
// GL 入口，抓不到就静默不画(不报错)。所以 KanbanOpenGLView 明确请求 OpenGL 3.3，
// 且本文件在第一次进 GL 时调用一次 glewInit()，供框架其它文件读扩展开关。
#include <GL/glew.h> // 必须第一个：框架的 Windows GL 头以 GLEW 的原型为准，且本文件不碰任何 Qt GL 头

#include "kanban/Live2DRenderer.h"
#include "kanban/KanbanRenderer.h"
#include "core/Diagnostics.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStringList>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <malloc.h> // _aligned_malloc / _aligned_free
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "CubismDefaultParameterId.hpp"
#include "CubismFramework.hpp"
#include "CubismModelSettingJson.hpp"
#include "Effect/CubismBreath.hpp"
#include "Effect/CubismEyeBlink.hpp"
#include "Effect/CubismLook.hpp"
#include "ICubismAllocator.hpp"
#include "ICubismModelSetting.hpp"
#include "Id/CubismIdManager.hpp"
#include "Math/CubismMatrix44.hpp"
#include "Math/CubismModelMatrix.hpp"
#include "Math/CubismTargetPoint.hpp"
#include "Model/CubismUserModel.hpp"
#include "Motion/ACubismMotion.hpp"
#include "Motion/CubismBreathUpdater.hpp"
#include "Motion/CubismExpressionMotionManager.hpp"
#include "Motion/CubismExpressionUpdater.hpp"
#include "Motion/CubismEyeBlinkUpdater.hpp"
#include "Motion/CubismLookUpdater.hpp"
#include "Motion/CubismMotion.hpp"
#include "Motion/CubismMotionManager.hpp"
#include "Motion/CubismPhysicsUpdater.hpp"
#include "Motion/CubismPoseUpdater.hpp"
#include "Motion/CubismUpdateScheduler.hpp"
#include "Physics/CubismPhysics.hpp"
#include "Rendering/CubismRenderer.hpp"
#include "Rendering/OpenGL/CubismOffscreenManager_OpenGLES2.hpp"
#include "Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "Rendering/OpenGL/CubismShader_OpenGLES2.hpp"
#include "Type/csmMap.hpp"

using namespace Live2D::Cubism::Framework;
// 渲染层在 R.5 里单独一层命名空间：renderer、离屏管理、遮罩目标都在里头。
using namespace Live2D::Cubism::Framework::Rendering;
// 呼吸/看向要按标准参数名登记，这些常量在 DefaultParameterId 子命名空间里。
using namespace Live2D::Cubism::Framework::DefaultParameterId;

namespace kanban {

namespace {

constexpr char kModule[] = "Live2D";

// 与官方示例一致的优先级口径：数值越大越优先，低优先级请求会被拒绝。
constexpr int kPriorityNone = 0;
constexpr int kPriorityIdle = 1;
constexpr int kPriorityNormal = 2;
constexpr int kPriorityForce = 3;

QString logLine(const QString &text)
{
    return QStringLiteral("[Live2D] %1").arg(text);
}

void logInfo(const QString &text)
{
    videodiag::log(videodiag::Level::Info, logLine(text), QLatin1String(kModule));
}

void logWarn(const QString &text)
{
    videodiag::log(videodiag::Level::Warning, logLine(text), QLatin1String(kModule));
}

// 交互路径专用：点击/表情这类「用户看得见、日志里却什么都没有」的动作，
// 必须留下可查的痕迹。否则「点了没反应」只能靠猜是命中区没配、动作组没配，
// 还是压根没收到点击。
void logDebug(const QString &text)
{
    videodiag::log(videodiag::Level::Debug, logLine(text), QLatin1String(kModule));
}

// Cubism(Core 与 Framework 共用这一个回调)的日志出口。
// 刻意在 StartUp 之前就把指针交出去：SDK 在 StartUp 内部就会打印 Core 版本号，
// 那行日志正是「接的是哪个内核」的最硬证据，必须落到 videodiag 里。
void cubismLogBridge(const char *message)
{
    if (!message) {
        return;
    }
    const QString text = QString::fromUtf8(message).trimmed();
    if (text.isEmpty()) {
        return;
    }
    // SDK 把级别写进文本前缀，这里翻成 videodiag 的四级；默认只放行 warning 以上，
    // 诊断模式打开时放行 info(装载过程每一步都留痕)。
    videodiag::Level level = videodiag::Level::Debug;
    if (text.contains(QStringLiteral("[error]"), Qt::CaseInsensitive)) {
        level = videodiag::Level::Error;
    } else if (text.contains(QStringLiteral("[warning]"), Qt::CaseInsensitive)) {
        level = videodiag::Level::Warning;
    } else if (text.contains(QStringLiteral("[info]"), Qt::CaseInsensitive)) {
        level = videodiag::Level::Info;
    }
    videodiag::log(level, logLine(text), QLatin1String(kModule));
}

// ── 框架进程级资源 ────────────────────────────────────────────────────

// Cubism 要求的分配器。SDK 只用这三个动作，直接映射到 CRT 对齐分配最省事，
// 也避免去链示例工程那个自带内存池的 LAppAllocator(MinGW 下 CRT 不匹配)。
class KanbanCubismAllocator final : public ICubismAllocator
{
public:
    void *Allocate(const csmSizeType size) override
    {
        return std::malloc(static_cast<size_t>(size));
    }
    void Deallocate(void *memory) override
    {
        std::free(memory);
    }
    void *AllocateAligned(const csmSizeType size, const csmUint32 alignment) override
    {
        const size_t align = alignment > 0 ? alignment : alignof(std::max_align_t);
        return _aligned_malloc(static_cast<size_t>(size), align);
    }
    void DeallocateAligned(void *alignedMemory) override
    {
        _aligned_free(alignedMemory);
    }
};

// ⚠ CubismFramework::StartUp() 只保存这两个对象的「指针」，不拷贝内容；
// 而且一旦启动过就直接提前返回，第二个渲染器实例无法重新绑定。
// 所以它们必须是与进程同寿的文件级静态，绝不能塞进 Private 里跟着对象析构。
KanbanCubismAllocator g_allocator;
CubismFramework::Option g_option;

// ── 运行期读盘回调 ────────────────────────────────────────────────────
//
// Cubism 的 GL 着色器不在代码里，而在运行期逐个读盘：CubismShader_OpenGLES2::
// GenerateShaders() 会用常量路径("FrameworkShaders/VertShaderSrc.vert" 等)回调
// 宿主提供的 LoadFileFunction 去要字节。框架自己不读文件，官方示例填的是
// LAppPal::LoadFileAsBytes，这里给等价实现 —— 少了这两个回调，框架在
// GenerateShaders 里只会写一行 "File loader is not set."，然后拿着
// ShaderProgram=0 一路画下去：不崩、不报错、桌面上什么都没有。
//
// 路径解析口径与官方示例不同，是刻意的：SDK 传进来的永远是相对路径，官方示例
// 按「当前工作目录」解析 —— 从快捷方式、计划任务或别的程序拉起来时工作目录
// 未必是程序目录，那时就会全盘失效。这里按「程序目录 → 当前工作目录」顺序找，
// 部署时把 FrameworkShaders/ 放在 exe 旁边即可，与启动方式无关。
csmByte *loadCubismFileBytes(const std::string filePath, csmSizeInt *outSize)
{
    if (outSize) {
        *outSize = 0;
    }
    const QString relative = QString::fromUtf8(filePath.c_str());
    if (relative.isEmpty()) {
        return nullptr;
    }

    QStringList candidates;
    if (QFileInfo(relative).isAbsolute()) {
        candidates << relative;
    } else {
        candidates << QDir(QCoreApplication::applicationDirPath()).filePath(relative)
                   << QDir::current().filePath(relative);
    }

    for (const QString &path : candidates) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray data = file.readAll();
        file.close();
        if (data.isEmpty()) {
            continue;
        }
        // 释放函数由我们自己给(releaseCubismFileBytes)，所以分配方式随意，
        // 只要 new[]/delete[] 成对。框架会按 (指针, 长度) 构造 csmString，
        // 不需要我们补字符串终止符。
        const int size = data.size();
        csmByte *bytes = new csmByte[size];
        std::memcpy(bytes, data.constData(), static_cast<size_t>(size));
        if (outSize) {
            *outSize = static_cast<csmSizeInt>(size);
        }
        return bytes;
    }

    logWarn(QStringLiteral("读不到运行期资源：%1(程序目录 %2)")
                .arg(relative, QCoreApplication::applicationDirPath()));
    return nullptr;
}

void releaseCubismFileBytes(csmByte *byteData)
{
    delete[] byteData;
}

std::mt19937 &randomEngine()
{
    static std::mt19937 engine{std::random_device{}()};
    return engine;
}

int randomBelow(int bound)
{
    if (bound <= 1) {
        return 0;
    }
    std::uniform_int_distribution<int> dist(0, bound - 1);
    return dist(randomEngine());
}

// ============================================================================
// 进程级全局 GL 缓存：谁在用、什么时候必须丢
//
// Cubism 把着色器做成了**进程级单例**(CubismShader_OpenGLES2)，一次编译、
// 全局复用。这在「一个上下文活到进程结束」的移动端是合理的，但看板娘会
// 「取消 → 启用」：取消时窗口连同 GL 上下文一起销毁，启用时是一个全新上下文，
// 而单例还活着。program id 只属于创建它的上下文，于是新上下文里：
//   · GenerateShaders() 开头的 if (_shaderSets.GetSize() > 0) return; 直接短路，
//     压根不会重新编译；
//   · 后续每个绘制都沿用死 id，glUseProgram 静默失败。
// 症状极具迷惑性 —— 模型装载、纹理上传、30fps 的 DrawFrame 全部正常，
// 只有桌面上什么都没有(实测同一块区域的「不同颜色数」从 18021 掉到 1031)。
//
// 所以必须记住「这份缓存是给哪一代上下文建的」，换代就丢掉重来。
// 0 表示还没有缓存(进程刚起来，或已经被丢掉)。
// ============================================================================
quint64 g_shaderCacheGeneration = 0;

// 丢掉 Cubism 的进程级全局 GL 缓存，让下一次 CreateRenderer 在新上下文里
// 重新抓 GL 入口、重新编译全部着色器。
//
// 调用前提：g_shaderCacheGeneration != 0，也就是「单例一定已经存在」。
// 不满足时 GetInstance() 会当场新建单例并立刻编译几百个着色器，纯属浪费，
// 在没有当前上下文的时候甚至是一次崩溃。调用方 syncCubismShaderCache() 用
// 上面那个全局量把关，别绕过它直接调。
//
// 顺序不能反：
//   ① ReleaseInvalidShaderProgram() 只做 C++ 侧的 CSM_DELETE 与清空，全程不碰 GL。
//      它同时保证单例析构里的 ReleaseShaderProgram() 变成空循环 —— 那是唯一会发
//      glDeleteProgram 的地方，而此刻旧上下文多半已经不在了，对着死上下文发 GL
//      调用比漏一次释放严重得多。
//   ② StaticRelease() 复位 Windows GL 函数指针的抓取标志(s_isFirstInitializeGlFunctions
//      等)，并丢弃单例；下一次 CreateRenderer 会在新上下文里重新 wglGetProcAddress。
//      这一条同样重要：那些 PFNGL* 是 wglGetProcAddress 抓来的，在部分驱动上是
//      上下文相关的，不重抓就等于拿着旧上下文的入口画新上下文。
// 只调 ② 会在上下文已死时发出 glDeleteProgram；只调 ① 则单例仍在。两个都要。
void resetCubismGlobalGlState()
{
    CubismShader_OpenGLES2::GetInstance()->ReleaseInvalidShaderProgram();
    CubismRenderer::StaticRelease();
}

// 上下文换代检查：generation 是宿主当前上下文的世代号，0 表示还没有上下文。
//
// 放在「建渲染器之前」而不是「销毁上下文之时」，理由是只有这里能确定
// 「上下文真的换了」—— 渲染器每次启停都是新对象，没有跨启停的记忆；而旧上下文
// 是怎么没的(正常停止、启动失败、异常路径)在这里都不重要，换代就一定成立。
void syncCubismShaderCache(quint64 generation)
{
    if (generation == 0) {
        return;
    }
    if (g_shaderCacheGeneration != 0 && g_shaderCacheGeneration != generation) {
        logInfo(QStringLiteral("GL 上下文已换代(%1 → %2)，丢弃 Cubism 着色器缓存并重建")
                    .arg(g_shaderCacheGeneration)
                    .arg(generation));
        resetCubismGlobalGlState();
    }
    g_shaderCacheGeneration = generation;
}

// 把上下文的借用收敛成一个作用域：进入前保证 current，出去时只归还自己借的。
//
// paintGL 期间 Qt 已经把上下文当前化，此时既不必再 makeCurrent，
// 更绝对不能 doneCurrent —— 那会把视图自己的绘制状态从脚下抽走。
//
// 顺带把 glewInit() 也收在这里，理由是踩过一次的坑：
//   glew.h 把 glGenTextures / glBindTexture / glClear 这些名字统统重定向到
//   GLEW 自己的函数指针表(GLEW_GET_FUN)，而那张表在 glewInit() 之前全是空指针。
//   于是「第一次 GL 调用」如果发生在 glewInit() 之前，就是一次干净的
//   call 0x0 —— 没有异常、没有 Qt 错误框，只有进程凭空消失。
//   原先只在 render() 里初始化，而 loadModel() 里的纹理上传比它早，正好踩中。
//   放进 GlScope 之后，「借到上下文」与「函数表可用」变成同一件事，漏不掉了。
class GlScope
{
public:
    GlScope(KanbanGlHost *host, bool *glewReady)
        : m_host(host)
    {
        if (!m_host || !m_host->glHostReady()) {
            return;
        }
        if (!m_host->glIsCurrent()) {
            if (!m_host->glMakeCurrent()) {
                return;
            }
            m_owned = true;
        }
        // 走到这里上下文一定是当前化的。GLEW 的表是「上下文 + 线程」的产物，
        // 上下文易主后必须重来，所以这里只看传入的标志位。
        if (glewReady && !*glewReady) {
            if (glewInit() != GLEW_OK) {
                logWarn(QStringLiteral("glewInit 失败，GL 函数表不可用"));
                return;
            }
            *glewReady = true;
        }
        m_ok = true;
    }
    GlScope(const GlScope &) = delete;
    GlScope &operator=(const GlScope &) = delete;
    ~GlScope()
    {
        if (m_owned) {
            m_host->glDoneCurrent();
        }
    }
    bool ok() const { return m_ok; }

private:
    KanbanGlHost *m_host = nullptr;
    bool m_owned = false;
    bool m_ok = false;
};

bool readFile(const QString &path, QByteArray *out, QString *outError)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (outError) {
            *outError = QStringLiteral("读不到文件：%1(%2)")
                            .arg(QFileInfo(path).fileName(), file.errorString());
        }
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();
    if (data.isEmpty()) {
        if (outError) {
            *outError = QStringLiteral("文件是空的：%1").arg(QFileInfo(path).fileName());
        }
        return false;
    }
    *out = data;
    return true;
}

const csmByte *bytesOf(const QByteArray &array)
{
    return reinterpret_cast<const csmByte *>(array.constData());
}

csmSizeInt sizeOf(const QByteArray &array)
{
    return static_cast<csmSizeInt>(array.size());
}

} // namespace

// ============================================================================
// KanbanCubismModel：一个模型该有的全部状态
//
// 为什么自己派生 CubismUserModel 而不是直接用：SDK 只给了零件
// (moc/运动/物理/呼吸/看向/表情)，装配顺序、更新器注册表、纹理与运动缓存
// 都得有人负责，官方示例的 LAppModel 干的就是这件事。这里等价实现，
// 但把「CPU 侧装载」与「GL 侧上传」彻底劈开 —— 前者可以发生在任何线程时机，
// 后者只能在宿主上下文里。
// ============================================================================

class KanbanCubismModel final : public CubismUserModel
{
public:
    ~KanbanCubismModel() override
    {
        ReleaseGl();
        ReleaseCpu();
    }

    // 只读文件、建参数，不碰 GL。失败时模型不可用，调用方直接丢弃本对象。
    bool Setup(const QString &modelJsonPath, QString *outError);

    // 建渲染器并把纹理传上 GPU。必须在 GlScope 里调用。
    // contextGeneration 是宿主当前上下文的世代号(0 = 还没有上下文)，
    // 用来判断 Cubism 那份进程级着色器缓存是否已经随着旧上下文作废。
    bool EnsureGl(const QSize &pixelSize, quint64 contextGeneration, QString *outError);
    void ReleaseGl();

    void UpdateSelf(float deltaSeconds);
    void DrawFrame(const QSize &pixelSize);

    // —— 交互 ——
    void SetDragTarget(float x, float y);
    bool StartHitReaction(const QPointF &normalized);
    bool PlayNextMotion();
    bool StartIdleMotion();
    bool SetExpressionIndex(int index);
    // 顺序切下一张表情(用户点菜单用)；PlayRandomExpression 随机挑一张且避开
    // 当前这张(摸头反馈用)。两者都返回 false 表示「本模型没有表情」。
    bool PlayNextExpression();
    bool PlayRandomExpression();

    bool hasModel() const { return _model != nullptr; }
    void SetViewportSize(csmUint32 width, csmUint32 height) { SetRenderTargetSize(width, height); }
    QString textureName(int index) const;

    // 装载完就能问的四件事，门面层写日志/界面用，不必碰私有成员。
    int textureCount() const { return _textureImages.size(); }
    int motionGroupCount() const { return _motionGroups.size(); }
    int expressionCount() const { return _expressionNames.size(); }
    // 可播动作数(不含 idle 组)：就是预载成功、可以真的播出来的那些。
    int PlayableMotionCount() const { return _playableMotions.size(); }
    // 当前生效的表情名(还没切过则为空)，日志用。
    QString currentExpressionName() const
    {
        return _lastExpression >= 0 && _lastExpression < _expressionNames.size()
                   ? _expressionNames.at(_lastExpression)
                   : QString();
    }

    // 上下文已经没了(宿主正在析构)时的善后：纹理句柄随上下文一起作废，
    // 这时再发 glDeleteTextures 是未定义行为，所以只清账不发调用。
    // 注意渲染器对象本身仍要活在上下文里释放，这就是控制器必须坚持
    // 「renderer->shutdown() 早于窗口销毁」那条契约的原因。
    void InvalidateGl()
    {
        _glLive = false;
        _textureIds.clear();
    }

private:
    QString relativeToHome(const csmChar *relative) const;
    bool LoadSettingJson(const QString &jsonPath, QString *outError);
    void ReleaseCpu();
    bool DecodeTextures(QString *outError);
    // 预载一组的全部动作，返回**真正读进来的**组内序号。
    // 返回值不能省成 void：调用方要靠它区分「文件里写了 N 段」和「真能播 N 段」，
    // 后者才是「有没有下一个动作可播」的答案。
    QVector<int> PreloadMotionGroup(const QString &group);
    void FitProjection(const QSize &pixelSize, CubismMatrix44 *out);
    int motionCount(int group) const;
    bool startGroupMotion(int group, int index, int priority);

    // CubismFramework::GetIdManager()->GetId() 的简写：装载期要用几十次。
    static CubismIdHandle GetId(const csmChar *name);

    // 换算点击坐标要用到最近一次的投影修正量(命中测试得把它反过来除掉)。
    QSize _pixelSize;
    float _projScaleX = 1.0f;
    float _projScaleY = 1.0f;
    QStringList _motionKeys; // 与 _motions 的键同序，释放时逐个取用

    CubismModelSettingJson *_setting = nullptr;
    csmMap<csmString, ACubismMotion *> _motions;
    csmMap<csmString, ACubismMotion *> _expressions;
    csmVector<CubismIdHandle> _eyeBlinkIds;
    csmVector<CubismIdHandle> _lipSyncIds;
    QVector<QImage> _textureImages;   // CPU 侧解码结果，EnsureGl 才上传
    std::vector<GLuint> _textureIds;  // GPU 侧句柄，本对象负责释放
    QStringList _motionGroups;
    // 可播动作表：预载成功的非 idle 动作，(组下标, 组内序号) 按 model3.json 的
    // 声明顺序排好。PlayNextMotion 在这张表上顺序走一圈 —— 不是「按组随机」。
    //
    // 为什么记成一张扁平表而不是每次现算：只有预载成功的槽位才播得出来，
    // 而「有没有可播动作」要用来决定界面置灰。现算会算出「文件里有 N 段」，
    // 读盘失败的槽位也算进去，于是界面亮着、点下去却纹丝不动。
    QVector<QPair<int, int>> _playableMotions;
    int _motionCursor = 0;    // PlayNextMotion 在 _playableMotions 上的游标
    QStringList _expressionNames;
    QString _homeDir;
    bool _glLive = false;      // 渲染器与纹理确实活在某个上下文里
    int _idleGroup = -1;      // 名为 idle 的组，没有则退化为第 0 组
    int _nextExpression = 0;  // PlayNextExpression 的游标
    int _lastExpression = -1; // 当前生效的表情下标，用于避免「切了跟没切一样」
    csmBool _motionUpdated = false;
};

// —— 装载 ——

QString KanbanCubismModel::relativeToHome(const csmChar *relative) const
{
    if (!relative || !*relative) {
        return QString();
    }
    return QDir(_homeDir).filePath(QString::fromUtf8(relative));
}

QString KanbanCubismModel::textureName(int index) const
{
    if (!_setting || index < 0 || index >= _setting->GetTextureCount()) {
        return QString();
    }
    return QString::fromUtf8(_setting->GetTextureFileName(index));
}

bool KanbanCubismModel::Setup(const QString &modelJsonPath, QString *outError)
{
    _homeDir = QFileInfo(modelJsonPath).absolutePath();

    if (!LoadSettingJson(modelJsonPath, outError)) {
        return false;
    }

    // 1) 模型本体(.moc3)。失败时 _model 保持空，下面统一判定。
    if (const csmChar *mocFile = _setting->GetModelFileName(); mocFile && *mocFile) {
        QByteArray buffer;
        QString readError;
        const QString path = relativeToHome(mocFile);
        if (!readFile(path, &buffer, &readError)) {
            if (outError) {
                *outError = readError;
            }
            return false;
        }
        const auto mocVersion = GetMocVersionFromBuffer(bytesOf(buffer), sizeOf(buffer));
        LoadModel(bytesOf(buffer), sizeOf(buffer), false);
        if (!_model) {
            if (outError) {
                *outError = QStringLiteral("%1 无法解析(moc 版本 0x%2)，"
                                           "可能由比本 SDK 更新的编辑器导出")
                                .arg(QFileInfo(path).fileName())
                                .arg(static_cast<quint32>(mocVersion), 8, 16, QLatin1Char('0'));
            }
            return false;
        }
    } else {
        if (outError) {
            *outError = QStringLiteral("%1 里没有 FileReferences.Model").arg(QFileInfo(modelJsonPath).fileName());
        }
        return false;
    }

    // 2) 表情
    const csmInt32 expressionCount = _setting->GetExpressionCount();
    for (csmInt32 i = 0; i < expressionCount; ++i) {
        const csmString name = _setting->GetExpressionName(i);
        QByteArray buffer;
        if (!readFile(relativeToHome(_setting->GetExpressionFileName(i)), &buffer, nullptr)) {
            continue; // 表情缺一个不影响站得住，跳过即可
        }
        ACubismMotion *motion = LoadExpression(bytesOf(buffer), sizeOf(buffer), name.GetRawString());
        if (!motion) {
            continue;
        }
        if (_expressions.IsExist(name)) {
            ACubismMotion::Delete(_expressions[name]);
        }
        _expressions[name] = motion;
        _expressionNames << QString::fromUtf8(name.GetRawString());
    }
    if (!_expressionNames.isEmpty()) {
        _updateScheduler.AddUpdatableList(CSM_NEW CubismExpressionUpdater(*_expressionManager));
    }

    // 3) 物理摆动
    if (const csmChar *physicsFile = _setting->GetPhysicsFileName(); physicsFile && *physicsFile) {
        QByteArray buffer;
        if (readFile(relativeToHome(physicsFile), &buffer, nullptr)) {
            LoadPhysics(bytesOf(buffer), sizeOf(buffer));
            if (_physics) {
                _updateScheduler.AddUpdatableList(CSM_NEW CubismPhysicsUpdater(*_physics));
            }
        }
    }

    // 4) 姿态(-parts 二选一)
    if (const csmChar *poseFile = _setting->GetPoseFileName(); poseFile && *poseFile) {
        QByteArray buffer;
        if (readFile(relativeToHome(poseFile), &buffer, nullptr)) {
            LoadPose(bytesOf(buffer), sizeOf(buffer));
            if (_pose) {
                _updateScheduler.AddUpdatableList(CSM_NEW CubismPoseUpdater(*_pose));
            }
        }
    }

    // 5) 眨眼
    if (_setting->GetEyeBlinkParameterCount() > 0) {
        _eyeBlink = CubismEyeBlink::Create(_setting);
        if (_eyeBlink) {
            _updateScheduler.AddUpdatableList(CSM_NEW CubismEyeBlinkUpdater(_motionUpdated, *_eyeBlink));
        }
    }

    // 6) 呼吸：官方示例那组周期参数是照着标准参数表调出来的，直接沿用。
    _breath = CubismBreath::Create();
    {
        CubismIdHandle idBreath = CubismFramework::GetIdManager()->GetId(ParamBreath);
        csmVector<CubismBreath::BreathParameterData> breathParameters;
        breathParameters.PushBack(CubismBreath::BreathParameterData(GetId(ParamAngleX), 0.0f, 15.0f, 6.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(GetId(ParamAngleY), 0.0f, 8.0f, 3.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(GetId(ParamAngleZ), 0.0f, 10.0f, 5.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(GetId(ParamBodyAngleX), 0.0f, 4.0f, 15.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(idBreath, 0.5f, 0.5f, 3.2345f, 0.5f));
        _breath->SetParameters(breathParameters);
        _updateScheduler.AddUpdatableList(CSM_NEW CubismBreathUpdater(*_breath));
    }

    // 7) 用户数据(运动事件里按名取用，缺了不致命)
    if (const csmChar *userDataFile = _setting->GetUserDataFile(); userDataFile && *userDataFile) {
        QByteArray buffer;
        if (readFile(relativeToHome(userDataFile), &buffer, nullptr)) {
            LoadUserData(bytesOf(buffer), sizeOf(buffer));
        }
    }

    // 8) 让运动知道哪些参数归眨眼/口型接管，避免运动轨迹把它们按回原位
    for (csmInt32 i = 0; i < _setting->GetEyeBlinkParameterCount(); ++i) {
        _eyeBlinkIds.PushBack(_setting->GetEyeBlinkParameterId(i));
    }
    for (csmInt32 i = 0; i < _setting->GetLipSyncParameterCount(); ++i) {
        _lipSyncIds.PushBack(_setting->GetLipSyncParameterId(i));
    }

    // 9) 看向(头/眼跟随鼠标)：把 2D 拖动量映射回旋转参数
    _look = CubismLook::Create();
    {
        csmVector<CubismLook::LookParameterData> lookParameters;
        lookParameters.PushBack(CubismLook::LookParameterData(GetId(ParamAngleX), 30.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(GetId(ParamAngleY), 0.0f, 30.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(GetId(ParamAngleZ), 0.0f, 0.0f, -30.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(GetId(ParamBodyAngleX), 10.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(GetId(ParamEyeBallX), 1.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(GetId(ParamEyeBallY), 0.0f, 1.0f));
        _look->SetParameters(lookParameters);
        _updateScheduler.AddUpdatableList(CSM_NEW CubismLookUpdater(*_look, *_dragManager));
    }

    // 注册表填完了才排序：更新器之间是有先后依赖的(看向要在运动之后)。
    _updateScheduler.SortUpdatableList();

    // 10) 布局(model3.json 的 Layout 可以改宽度和锚点)
    csmMap<csmString, csmFloat32> layout;
    _setting->GetLayoutMap(layout);
    _modelMatrix->SetupFromLayout(layout);
    _model->SaveParameters();

    // 11) 纹理解码(还没上 GPU)：这一步就能发现丢图、坏图
    if (!DecodeTextures(outError)) {
        return false;
    }

    // 12) 运动全部预载：看板娘会频繁触发动作，边点边读盘会有明显卡顿
    const csmInt32 groupCount = _setting->GetMotionGroupCount();
    QVector<QVector<int>> loadedPerGroup; // 与 _motionGroups 同序
    for (csmInt32 i = 0; i < groupCount; ++i) {
        const QString group = QString::fromUtf8(_setting->GetMotionGroupName(i));
        _motionGroups << group;
        if (group.compare(QStringLiteral("idle"), Qt::CaseInsensitive) == 0) {
            _idleGroup = _motionGroups.size() - 1;
        }
        loadedPerGroup.append(PreloadMotionGroup(group));
    }
    if (_idleGroup < 0 && !_motionGroups.isEmpty()) {
        _idleGroup = 0;
    }
    // 可播动作表：把非 idle 组里预载成功的动作按声明顺序摊平。
    //
    // 摊平放在这里、而不是并进上面的循环：_idleGroup 的兜底(没有 idle 组就退化为
    // 第 0 组)要到循环结束才知道，边预载边摊平会把第 0 组当成可播动作收进去。
    //
    // 必须跳过 idle 组 —— 它一直在播，算进来「下一个」就永远有得播，
    // 而用户点下去看到的是同一段待机(见 KanbanRenderer::playableMotionCount)。
    for (int g = 0; g < _motionGroups.size(); ++g) {
        if (g == _idleGroup) {
            continue;
        }
        for (const int index : loadedPerGroup.at(g)) {
            _playableMotions.append(qMakePair(g, index));
        }
    }
    _motionManager->StopAllMotions();

    _updating = false;
    _initialized = true;
    return true;
}

bool KanbanCubismModel::LoadSettingJson(const QString &jsonPath, QString *outError)
{
    QByteArray buffer;
    QString readError;
    if (!readFile(jsonPath, &buffer, &readError)) {
        if (outError) {
            *outError = readError;
        }
        return false;
    }
    _setting = CSM_NEW CubismModelSettingJson(bytesOf(buffer), sizeOf(buffer));
    if (!_setting->IsValid()) {
        if (outError) {
            *outError = QStringLiteral("%1 不是合法的 model3.json").arg(QFileInfo(jsonPath).fileName());
        }
        CSM_DELETE(_setting);
        _setting = nullptr;
        return false;
    }
    return true;
}

CubismIdHandle KanbanCubismModel::GetId(const csmChar *name)
{
    return CubismFramework::GetIdManager()->GetId(name);
}

// —— GPU 侧 ——

bool KanbanCubismModel::EnsureGl(const QSize &pixelSize, quint64 contextGeneration,
                                 QString *outError)
{
    if (!_model) {
        if (outError) {
            *outError = QStringLiteral("模型未装载，无法建立 GL 资源");
        }
        return false;
    }
    ReleaseGl(); // 重建前先彻底清干净，纹理与 VBO 才不会有累积

    // 上传完就把 CPU 侧那份丢掉(一张 2048² 是 16MB，留着不划算)，代价是
    // 「上下文重建」或「宿主重交」之后必须重新解码才能再建 GL 资源。
    // 少了这一步，第二次 EnsureGl 会上传 0 张纹理：_textures 全为 0，
    // DrawMeshOpenGL 对每个绘制对象直接 return —— 模型整个消失，且
    // 不报错、不崩溃，正是最难查的那种「桌面上什么都没有」。
    if (!_setting || _textureImages.size() != int(_setting->GetTextureCount())) {
        if (!DecodeTextures(outError)) {
            return false;
        }
    }

    // 换代检查必须在 CreateRenderer() 之前：那一步内部会
    // CubismShader_OpenGLES2::GetInstance() 把着色器单例取出来，晚了就来不及了。
    // 「取消看板娘 → 再启用只剩一个空白窗口」的根因就落在这里。
    syncCubismShaderCache(contextGeneration);

    CreateRenderer(static_cast<csmUint32>(pixelSize.width()),
                   static_cast<csmUint32>(pixelSize.height()), 1);
    CubismRenderer_OpenGLES2 *renderer = GetRenderer<CubismRenderer_OpenGLES2>();
    if (!renderer) {
        if (outError) {
            *outError = QStringLiteral("Cubism 渲染器创建失败(GL 上下文未就绪?)");
        }
        return false;
    }

    for (int i = 0; i < _textureImages.size(); ++i) {
        const QImage &image = _textureImages.at(i);
        if (image.isNull()) {
            continue; // 空名字纹理位：model3.json 允许留空槽
        }
        GLuint textureId = 0;
        glGenTextures(1, &textureId);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
        // mipmap 不是可选项：Cubism 每次绘制前会把 GL_TEXTURE_MIN_FILTER 强制设成
        // GL_LINEAR_MIPMAP_LINEAR(见 CubismShader_OpenGLES2::SetupTexture)，纹理
        // 只要缺 mipmap 层，对采样器就是「不完整」，采样一律返回 (0,0,0,1)。
        // 表现是模型变成一堆纯黑方块，而 glGetError 全程为 0 —— 官方示例与
        // 参考实现都在 glTexImage2D 之后紧跟 glGenerateMipmap，正是这个原因。
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // 顶点着色器已把纹理坐标的 y 翻转过，边缘若按 REPEAT 会取到对侧像素，
        // 于是模型四周出现一圈来自另一端的杂色，这里按 CLAMP 收边。
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        renderer->BindTexture(static_cast<csmUint32>(i), textureId);
        _textureIds.push_back(textureId);
    }
    // 上传前已在 CPU 侧乘过 alpha，这里必须如实告知，否则二次相乘会让边缘发暗。
    renderer->IsPremultipliedAlpha(true);
    _pixelSize = pixelSize;
    _glLive = true;
    // 位图已经交给显卡，CPU 侧这份就不再需要；留着等于白占十几 MB。
    _textureImages.clear();
    _textureImages.squeeze();
    logInfo(QStringLiteral("纹理上传完成：%1 张，绘制面 %2x%3")
                .arg(static_cast<int>(_textureIds.size()))
                .arg(pixelSize.width())
                .arg(pixelSize.height()));
    return true;
}

void KanbanCubismModel::ReleaseGl()
{
    if (!_glLive) {
        _textureIds.clear();
        return;
    }
    _glLive = false;
    if (!_textureIds.empty()) {
        glDeleteTextures(static_cast<GLsizei>(_textureIds.size()), _textureIds.data());
        _textureIds.clear();
    }
    DeleteRenderer(); // 基类实现自带空指针判定，重复调用安全
}

// 等比完整显示：官方示例的适配口径是「画布高度恒为 2 个单位」
// (见 CubismModelMatrix 构造里的 SetHeight(2.0f))，所以只要按短边除掉
// 视口纵横比，长边自然留白，既不形变也不裁切。
void KanbanCubismModel::FitProjection(const QSize &pixelSize, CubismMatrix44 *out)
{
    const float w = pixelSize.width() > 0 ? static_cast<float>(pixelSize.width()) : 1.0f;
    const float h = pixelSize.height() > 0 ? static_cast<float>(pixelSize.height()) : 1.0f;

    if (_model->GetCanvasWidth() > 1.0f && w < h) {
        _modelMatrix->SetWidth(2.0f); // 横向画布塞进竖窗口：以宽度为准
        _projScaleX = 1.0f;
        _projScaleY = w / h;
    } else {
        _projScaleX = h / w;
        _projScaleY = 1.0f;
    }
    out->LoadIdentity();
    out->Scale(_projScaleX, _projScaleY);
    out->MultiplyByMatrix(_modelMatrix);
}

void KanbanCubismModel::UpdateSelf(float deltaSeconds)
{
    if (!_model) {
        return;
    }
    _motionUpdated = false;

    _model->LoadParameters(); // 回到上一帧保存的状态，运动之间才能叠加
    if (_motionManager->IsFinished()) {
        StartIdleMotion();
    } else {
        _motionUpdated = _motionManager->UpdateMotion(_model, deltaSeconds);
    }
    _model->SaveParameters();

    // 眨眼/呼吸/物理/姿态/表情/看向都在这个调度器里按登记顺序跑，
    // 其中 CubismLookUpdater 会顺带推进 _dragManager，别再手动 Update 它。
    _updateScheduler.OnLateUpdate(_model, deltaSeconds);
    _model->Update();
}

void KanbanCubismModel::DrawFrame(const QSize &pixelSize)
{
    CubismRenderer_OpenGLES2 *renderer = GetRenderer<CubismRenderer_OpenGLES2>();
    if (!_model || !renderer) {
        return;
    }
    glViewport(0, 0, pixelSize.width(), pixelSize.height());
    // 清成 alpha=0：透明窗口下任何非零底色的都会变成立像周围一圈脏边。
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // 注意模板实参是 CubismRenderTarget_OpenGLES2(渲染目标本体)，
    // 不是同名相近的 CubismOffscreenRenderTarget_OpenGLES2 —— R.5 里管理器就是这么声明的。
    ICubismOffscreenManager<CubismRenderTarget_OpenGLES2> *offscreen =
        CubismOffscreenManager_OpenGLES2::GetInstance();
    offscreen->BeginFrameProcess();

    CubismMatrix44 projection;
    FitProjection(pixelSize, &projection);
    renderer->SetMvpMatrix(&projection);
    renderer->DrawModel();

    offscreen->EndFrameProcess();
    offscreen->ReleaseStaleRenderTextures();
}

// —— 交互 ——

void KanbanCubismModel::SetDragTarget(float x, float y)
{
    if (_dragManager) {
        _dragManager->Set(x, y); // Look 更新器每帧取这里的值
    }
}

bool KanbanCubismModel::StartHitReaction(const QPointF &normalized)
{
    if (!_model || !_setting || _setting->GetHitAreasCount() <= 0) {
        // 没有命中区不是错误(不少模型根本没配 HitAreas)，但必须能查出来 ——
        // 否则「点了没反应」只能靠猜。
        logDebug(QStringLiteral("点击无反应：本模型没有配置 HitAreas"));
        return false;
    }
    // IsHit 收到的是「投影之后、modelMatrix 之前」的坐标，故要除掉投影修正量。
    const float x = static_cast<float>(normalized.x() * 2.0 - 1.0) / _projScaleX;
    const float y = static_cast<float>(1.0 - normalized.y() * 2.0) / _projScaleY;

    const csmInt32 count = _setting->GetHitAreasCount();
    for (csmInt32 i = 0; i < count; ++i) {
        if (!IsHit(_setting->GetHitAreaId(i), x, y)) {
            continue;
        }
        const QString area = QString::fromUtf8(_setting->GetHitAreaName(i));

        // 头部命中优先给「表情」，身体命中给「动作」—— 与参考实现
        // (QtLive2dDesktop 的 LAppLive2DManager::OnTap)一致：摸头是表情回应，
        // 戳身体是动作回应。命名约定各家不一，所以两种写法都认。
        const bool headLike = area.contains(QLatin1String("Head"), Qt::CaseInsensitive)
                              || area.contains(QStringLiteral("头"))
                              || area.contains(QStringLiteral("臉"))
                              || area.contains(QStringLiteral("脸"));
        if (headLike && PlayRandomExpression()) {
            logDebug(QStringLiteral("命中「%1」→ 表情反馈").arg(area));
            return true;
        }

        // 动作惯例：命中区名与运动组同名，或带 Tap 前缀，两种都认。
        const QStringList candidates = {area, QStringLiteral("Tap%1").arg(area)};
        for (const QString &candidate : candidates) {
            for (int g = 0; g < _motionGroups.size(); ++g) {
                if (_motionGroups.at(g).compare(candidate, Qt::CaseInsensitive) == 0 &&
                    motionCount(g) > 0) {
                    const bool played =
                        startGroupMotion(g, randomBelow(motionCount(g)), kPriorityForce);
                    logDebug(QStringLiteral("命中「%1」→ 动作组「%2」%3")
                                 .arg(area, candidate,
                                      played ? QStringLiteral("已播放")
                                             : QStringLiteral("被更高优先级的动作挡住")));
                    return played;
                }
            }
        }

        // 命中区既没配表情也没配动作(小模型很常见)：最后给个表情兜底，
        // 否则用户点了角色却毫无反应，看起来像点击没生效。
        if (!headLike && PlayRandomExpression()) {
            logDebug(QStringLiteral("命中「%1」没有对应动作组 → 退化为表情反馈").arg(area));
            return true;
        }
        logDebug(QStringLiteral("命中「%1」但既无同名/Tap 动作组，也没有可用表情").arg(area));
    }
    logDebug(QStringLiteral("点击未落入任何命中区(归一化 %1, %2)")
                 .arg(normalized.x(), 0, 'f', 3)
                 .arg(normalized.y(), 0, 'f', 3));
    return false;
}

int KanbanCubismModel::motionCount(int group) const
{
    if (!_setting || group < 0 || group >= _motionGroups.size()) {
        return 0;
    }
    return _setting->GetMotionCount(_motionGroups.at(group).toUtf8().constData());
}

bool KanbanCubismModel::startGroupMotion(int group, int index, int priority)
{
    if (!_motionManager || group < 0 || group >= _motionGroups.size()) {
        return false;
    }
    const QString groupName = _motionGroups.at(group);
    if (index < 0 || index >= motionCount(group)) {
        return false;
    }

    // 优先级谈判：Force 先占位，其它优先级在当前动作播完前抢不到就放弃。
    if (priority == kPriorityForce) {
        _motionManager->SetReservePriority(priority);
    } else if (!_motionManager->ReserveMotion(priority)) {
        return false;
    }

    const QByteArray keyUtf8 = QStringLiteral("%1_%2").arg(groupName).arg(index).toUtf8();
    const csmString key(keyUtf8.constData());
    if (!_motions.IsExist(key)) {
        return false; // 预载时读盘失败的槽位，这里静默跳过
    }
    return _motionManager->StartMotionPriority(_motions[key], false, priority) !=
           InvalidMotionQueueEntryHandleValue;
}

bool KanbanCubismModel::PlayNextMotion()
{
    const int total = _playableMotions.size();
    if (total == 0) {
        return false;
    }
    // 在可播动作表上顺序走一圈，点一次换一段，不原地重播。
    //
    // 这里刻意不用「组间轮转 + 组内随机」的老做法：同一组被连着轮到时，
    // 随机可能抽中刚播过的那一段，用户看到的是「点了没反应」。既然界面已经
    // 保证「可播动作 ≥ 2 才可点」，就该让每次点击都真的换一段。
    //
    // 用 Force 优先级，不用 Normal：CubismMotionManager::ReserveMotion 除了比
    // _reservePriority，还比 _currentPriority，而 _currentPriority 要等队列空了
    // 才复位 —— 也就是说动作还在播时，同优先级的请求会被**直接拒绝**。
    // 老代码用 Normal，于是「播放中再点一次」必然失败，而失败又会掉进
    // 「没动作就换模型」的兜底：模型明明有动作，点快一点模型却被换走了。
    // 显式点击是用户的明确意图，不该被静默丢弃(点击模型触发的命中反馈也用 Force)。
    for (int attempt = 0; attempt < total; ++attempt) {
        const int slot = _motionCursor;
        const QPair<int, int> &motion = _playableMotions.at(slot);
        _motionCursor = (slot + 1) % total;
        if (startGroupMotion(motion.first, motion.second, kPriorityForce)) {
            logDebug(QStringLiteral("播放下一个动作「%1_%2」(第 %3/%4 个)")
                         .arg(_motionGroups.value(motion.first))
                         .arg(motion.second)
                         .arg(slot + 1)
                         .arg(total));
            return true;
        }
    }
    logDebug(QStringLiteral("播放下一个动作失败：%1 个可播动作全部被优先级挡下").arg(total));
    return false;
}

bool KanbanCubismModel::StartIdleMotion()
{
    const int count = motionCount(_idleGroup);
    if (count > 0) {
        return startGroupMotion(_idleGroup, randomBelow(count), kPriorityIdle);
    }
    // 没有 idle 组就随便挑一组顶上，站桩不动比动作重复更难看。
    for (int g = 0; g < _motionGroups.size(); ++g) {
        const int n = motionCount(g);
        if (n > 0 && startGroupMotion(g, randomBelow(n), kPriorityIdle)) {
            return true;
        }
    }
    return false;
}

bool KanbanCubismModel::SetExpressionIndex(int index)
{
    if (!_expressionManager || index < 0 || index >= _expressionNames.size()) {
        return false;
    }
    const QByteArray utf8 = _expressionNames.at(index).toUtf8();
    const csmString name(utf8.constData());
    if (!_expressions.IsExist(name)) {
        return false;
    }
    if (_expressionManager->StartMotion(_expressions[name], false) ==
        InvalidMotionQueueEntryHandleValue) {
        return false;
    }
    // 记账只在这一处：_lastExpression 是「当前生效的表情」的唯一真相，
    // 两个调用方(顺序切/随机切)都靠它避免「切了跟没切一样」。
    _lastExpression = index;
    return true;
}

bool KanbanCubismModel::PlayNextExpression()
{
    if (!_expressionManager || _expressionNames.isEmpty()) {
        return false;
    }
    const int total = _expressionNames.size();
    // 顺序轮转而不是随机：用户点「切换表情」是想把几个表情看一遍，
    // 随机抽样会连着撞同一个，看起来像没生效。
    for (int attempt = 0; attempt < total; ++attempt) {
        const int index = _nextExpression % total;
        _nextExpression = (index + 1) % total;
        // 只有一个表情的模型不跳过 —— 那一个就是它的全部，重播也算切了。
        if (total > 1 && index == _lastExpression) {
            continue;
        }
        if (SetExpressionIndex(index)) {
            return true;
        }
    }
    return false;
}

bool KanbanCubismModel::PlayRandomExpression()
{
    if (!_expressionManager || _expressionNames.isEmpty()) {
        return false;
    }
    const int total = _expressionNames.size();
    if (total == 1) {
        return SetExpressionIndex(0);
    }
    // 避开当前这张：摸头若有一半概率挑回同一张，用户会以为点击没生效。
    int index = _lastExpression;
    for (int attempt = 0; attempt < 8 && index == _lastExpression; ++attempt) {
        index = randomBelow(total);
    }
    if (index == _lastExpression) {
        index = (_lastExpression + 1) % total; // 连续撞车(概率极低)时的确定性兜底
    }
    if (!SetExpressionIndex(index)) {
        return false;
    }
    // 同步游标，免得用户接着点「切换表情」时又绕回刚随机挑中的这张。
    _nextExpression = (index + 1) % total;
    return true;
}

// —— 装载辅助 ——

bool KanbanCubismModel::DecodeTextures(QString *outError)
{
    const csmInt32 count = _setting->GetTextureCount();
    if (count <= 0) {
        if (outError) {
            *outError = QStringLiteral("model3.json 里没有 FileReferences.Textures");
        }
        return false;
    }
    _textureImages.clear();
    for (csmInt32 i = 0; i < count; ++i) {
        const QString path = relativeToHome(_setting->GetTextureFileName(i));
        if (path.isEmpty()) {
            _textureImages << QImage();
            continue;
        }
        QByteArray buffer;
        QString readError;
        if (!readFile(path, &buffer, &readError)) {
            if (outError) {
                *outError = readError;
            }
            return false;
        }
        QImage image = QImage::fromData(buffer, "PNG");
        if (image.isNull()) {
            if (outError) {
                *outError = QStringLiteral("%1 解码失败(不是 PNG 或已损坏)")
                                .arg(QFileInfo(path).fileName());
            }
            return false;
        }
        // 统一成「预乘 alpha 的 RGBA8」：GL 侧格式固定，边缘也不会有半透明的白边。
        _textureImages << image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    }
    return true;
}

QVector<int> KanbanCubismModel::PreloadMotionGroup(const QString &group)
{
    QVector<int> loaded; // 真读进来的组内序号
    const QByteArray groupUtf8 = group.toUtf8();
    const csmChar *groupName = groupUtf8.constData();
    const csmInt32 count = _setting->GetMotionCount(groupName);
    for (csmInt32 i = 0; i < count; ++i) {
        const QString key = QStringLiteral("%1_%2").arg(group).arg(i);
        const QByteArray keyUtf8 = key.toUtf8();
        QByteArray buffer;
        if (!readFile(relativeToHome(_setting->GetMotionFileName(groupName, i)), &buffer, nullptr)) {
            continue;
        }
        CubismMotion *motion = static_cast<CubismMotion *>(
            LoadMotion(bytesOf(buffer), sizeOf(buffer), keyUtf8.constData(),
                       nullptr, nullptr, _setting, groupName, i, _motionConsistency));
        if (!motion) {
            continue;
        }
        // 告诉运动数据哪些参数归眨眼/口型接管，否则轨迹会把它们按回原位。
        motion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);
        const csmString name(keyUtf8.constData());
        if (_motions.IsExist(name)) {
            ACubismMotion::Delete(_motions[name]);
        } else {
            _motionKeys << key;
        }
        _motions[name] = motion;
        loaded << int(i);
    }
    return loaded;
}

void KanbanCubismModel::ReleaseCpu()
{
    // csmMap 的 Clear() 只管自己的存储，不动它持有的对象，
    // 所以逐个 ACubismMotion::Delete 必须自己做，漏一次就是永久泄漏。
    for (const QString &key : _motionKeys) {
        const QByteArray utf8 = key.toUtf8();
        const csmString name(utf8.constData());
        if (_motions.IsExist(name)) {
            ACubismMotion::Delete(_motions[name]);
        }
    }
    _motionKeys.clear();
    _motions.Clear();

    for (const QString &name : _expressionNames) {
        const QByteArray utf8 = name.toUtf8();
        const csmString key(utf8.constData());
        if (_expressions.IsExist(key)) {
            ACubismMotion::Delete(_expressions[key]);
        }
    }
    _expressionNames.clear();
    _expressions.Clear();

    // 这几个裸指针基类析构也会删，先置空就是防止二次释放。
    if (_eyeBlink) {
        CubismEyeBlink::Delete(_eyeBlink);
        _eyeBlink = nullptr;
    }
    if (_breath) {
        CubismBreath::Delete(_breath);
        _breath = nullptr;
    }
    if (_look) {
        CubismLook::Delete(_look);
        _look = nullptr;
    }
    CSM_DELETE(_setting);
    _setting = nullptr;

    _textureImages.clear();
    _motionGroups.clear();
    _playableMotions.clear();
    _motionCursor = 0;
    _idleGroup = -1;
    _nextExpression = 0;
    _initialized = false;
}

// ============================================================================
// Live2DRenderer：控制器唯一认识的那个门面
//
// 这一层刻意只做三件事：把 Cubism 对象藏进 Private、把「上下文有没有就绪」
// 翻译成 GlScope、把 Qt 的坐标(逻辑像素、y 向下)翻译成 Cubism 的 NDC(y 向上)。
// 模型本身的事一概不问，全交给 KanbanCubismModel。
// ============================================================================

struct Live2DRenderer::Private
{
    KanbanCubismModel *model = nullptr;
    KanbanGlHost *host = nullptr;
    bool glBuilt = false;   // 渲染器与纹理是否已在当前上下文里建好
    bool glewReady = false; // glewInit() 只需成功一次，上下文易主则重来
    QSize pixelSize;        // glBuilt 为真时对应的绘制面尺寸
};

bool Live2DRenderer::sdkCompiledIn()
{
    return true;
}

QString Live2DRenderer::unavailableReason()
{
    // 能走到这份实现就说明 SDK 已经链进来了，没有「不可用」可言。
    return QStringLiteral("Cubism Native SDK 已接入，无需降级");
}

Live2DRenderer::Live2DRenderer()
    : m_d(std::make_unique<Private>())
{
}

Live2DRenderer::~Live2DRenderer()
{
    shutdown();
}

void Live2DRenderer::setGlHost(KanbanGlHost *host)
{
    // 同一个宿主再交一次是常态：控制器在 GL 就绪后会把视图重新 attach 一遍。
    // 那种情况下上下文没换，GL 对象与纹理都还有效 —— 一旦在这里无条件作废，
    // 刚上传好的纹理就被判成过期，下一帧重建时又拿不到 CPU 侧数据，模型直接消失。
    // 只有宿主真的换了(视图重建、后端切换、上下文销毁)才需要作废。
    if (host == m_d->host) {
        return;
    }
    if (m_d->model) {
        m_d->model->InvalidateGl();
    }
    m_d->host = host;
    m_d->glBuilt = false;
    m_d->glewReady = false;
    m_d->pixelSize = QSize();
}

bool Live2DRenderer::initialize(QString *outError)
{
    if (m_ready) {
        return true;
    }

    // 日志指针必须在 StartUp 之前交出去：框架在 StartUp 内部就会打印 Core 版本号，
    // 那一行是「接的到底是哪个内核」最硬的证据，落到 videodiag 里才有得查。
    g_option.LogFunction = &cubismLogBridge;
    g_option.LoggingLevel = CubismFramework::Option::LogLevel_Warning;
    // 读盘回调同样必须早于 StartUp：GenerateShaders() 在第一次 CreateRenderer
    // 时就会调它，那时再补已经晚了(着色器是「生成一次、全局缓存」的)。
    g_option.LoadFileFunction = &loadCubismFileBytes;
    g_option.ReleaseBytesFunction = &releaseCubismFileBytes;

    if (!CubismFramework::IsStarted() && !CubismFramework::StartUp(&g_allocator, &g_option)) {
        const QString reason = QStringLiteral("CubismFramework::StartUp 失败");
        if (outError) {
            *outError = reason;
        }
        logWarn(reason);
        return false;
    }
    if (!CubismFramework::IsInitialized()) {
        CubismFramework::Initialize();
    }

    m_ready = true;
    logInfo(QStringLiteral("框架就绪，Core 版本 0x%1")
                .arg(static_cast<quint32>(Live2D::Cubism::Core::csmGetVersion()), 8, 16, QLatin1Char('0')));
    return true;
}

bool Live2DRenderer::usesOpenGL() const
{
    return true;
}

bool Live2DRenderer::loadModel(const QString &modelJsonPath, QString *outError)
{
    if (!initialize(outError)) {
        return false;
    }
    unloadModel();

    auto *model = new KanbanCubismModel();
    if (!model->Setup(modelJsonPath, outError)) {
        // Setup 失败时还没碰过 GL，直接丢弃是安全的。
        delete model;
        return false;
    }

    m_d->model = model;
    m_modelPath = modelJsonPath;
    m_modelLoaded = true;
    logInfo(QStringLiteral("已装载 %1：纹理 %2 张 / 动作组 %3 个(可播动作 %4 个) / 表情 %5 个")
                .arg(QFileInfo(modelJsonPath).completeBaseName())
                .arg(model->textureCount())
                .arg(model->motionGroupCount())
                .arg(model->PlayableMotionCount())
                .arg(model->expressionCount()));

    // 控制器换模型时未必在 paintGL 里，此时没有上下文是常态；
    // 建不起来就交给第一次 render() 补，不把这一步当失败。
    if (m_d->host) {
        const GlScope scope(m_d->host, &m_d->glewReady);
        const QSize pixels = m_d->host->glPixelSize();
        if (scope.ok() && pixels.width() > 0 && pixels.height() > 0) {
            QString glError;
            if (model->EnsureGl(pixels, m_d->host->glContextGeneration(), &glError)) {
                m_d->glBuilt = true;
                m_d->pixelSize = pixels;
            } else {
                logWarn(QStringLiteral("纹理上传延后：%1").arg(glError));
            }
        } else if (!scope.ok()) {
            logWarn(QStringLiteral("装载时上下文不可用，纹理上传延后到首帧"));
        }
    }
    return true;
}

void Live2DRenderer::unloadModel()
{
    KanbanCubismModel *model = m_d->model;
    m_d->model = nullptr;
    m_d->glBuilt = false;
    m_d->pixelSize = QSize();
    m_modelLoaded = false;
    if (!model) {
        return;
    }

    // 释放与创建必须成对，且都要在上下文里；上下文已经没了就只能作废句柄，
    // 对着死上下文发 GL 调用比漏一次释放严重得多。
    const GlScope scope(m_d->host, &m_d->glewReady);
    if (scope.ok()) {
        model->ReleaseGl();
    } else {
        model->InvalidateGl();
    }
    delete model;
}

void Live2DRenderer::resize(int width, int height, float devicePixelRatio)
{
    m_width = width;
    m_height = height;
    m_dpr = devicePixelRatio;
}

void Live2DRenderer::update(float deltaSeconds)
{
    if (!m_modelLoaded || m_paused || !m_d->model) {
        return;
    }
    m_d->model->UpdateSelf(deltaSeconds);
}

void Live2DRenderer::paint(QPainter *painter, const QSize &logicalSize)
{
    // GPU 路径由 render() 负责，软件路径下本后端不存在。
    Q_UNUSED(painter)
    Q_UNUSED(logicalSize)
}

void Live2DRenderer::render()
{
    KanbanCubismModel *model = m_d->model;
    if (!model || !m_d->host) {
        return;
    }
    // GlScope 顺带保证 GLEW 函数表可用(见类注释)，不必在这里单独 glewInit。
    const GlScope scope(m_d->host, &m_d->glewReady);
    if (!scope.ok()) {
        return;
    }

    const QSize pixels = m_d->host->glPixelSize();
    if (pixels.width() <= 0 || pixels.height() <= 0) {
        return;
    }

    if (!m_d->glBuilt) {
        QString glError;
        if (!model->EnsureGl(pixels, m_d->host->glContextGeneration(), &glError)) {
            // 只报一次，否则 30fps 的时钟会把日志刷满同一句话。
            if (m_d->pixelSize != pixels) {
                logWarn(QStringLiteral("GL 资源建立失败：%1").arg(glError));
                m_d->pixelSize = pixels;
            }
            return;
        }
        m_d->glBuilt = true;
        m_d->pixelSize = pixels;
    } else if (m_d->pixelSize != pixels) {
        model->SetViewportSize(static_cast<csmUint32>(pixels.width()),
                               static_cast<csmUint32>(pixels.height()));
        m_d->pixelSize = pixels;
    }

    model->DrawFrame(pixels);
}

void Live2DRenderer::pointerMove(const QPointF &pos)
{
    if (!m_d->model) {
        return;
    }
    // 逻辑像素 → NDC：x 右为正、y 上为正，和 Qt 的 y 向下相反。
    const float w = m_width > 0 ? static_cast<float>(m_width) : 1.0f;
    const float h = m_height > 0 ? static_cast<float>(m_height) : 1.0f;
    m_d->model->SetDragTarget(pos.x() / w * 2.0f - 1.0f, 1.0f - pos.y() / h * 2.0f);
}

void Live2DRenderer::pointerClick(const QPointF &pos)
{
    if (!m_d->model) {
        return;
    }
    const float w = m_width > 0 ? static_cast<float>(m_width) : 1.0f;
    const float h = m_height > 0 ? static_cast<float>(m_height) : 1.0f;
    m_d->model->StartHitReaction(QPointF(pos.x() / w, pos.y() / h));
}

bool Live2DRenderer::playNextMotion()
{
    return m_d->model && m_d->model->PlayNextMotion();
}

int Live2DRenderer::playableMotionCount() const
{
    // 模型没装载时答 0：界面据此把「播放下一个动作」置灰，
    // 而不是让用户点了没反应。
    return m_d->model ? m_d->model->PlayableMotionCount() : 0;
}

int Live2DRenderer::expressionCount() const
{
    // 模型没装载时答 0：界面据此把入口置灰，而不是让用户点了没反应。
    return m_d->model ? m_d->model->expressionCount() : 0;
}

bool Live2DRenderer::playNextExpression()
{
    if (!m_d->model || !m_d->model->PlayNextExpression()) {
        return false;
    }
    logDebug(QStringLiteral("切换表情 → %1").arg(m_d->model->currentExpressionName()));
    return true;
}

void Live2DRenderer::pause()
{
    m_paused = true;
}

void Live2DRenderer::resume()
{
    m_paused = false;
}

void Live2DRenderer::shutdown()
{
    // 只放模型，不放框架：Option 与 IdManager 是进程级的，
    // CubismFramework::Dispose() 之后再 StartUp 并不会重新绑定 Option，
    // 反而让「关掉再打开看板娘」变成一条走不通的路。
    unloadModel();
    m_ready = false;
    m_modelLoaded = false;
    m_modelPath.clear();
}

} // namespace kanban

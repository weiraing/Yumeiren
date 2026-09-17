// Live2D 后端入口，不向控制器暴露 SDK 类型。
// GL 资源在宿主上下文内释放；CubismRuntime 保持进程级框架存活。
// 构建时选择 Cubism 或 Stub 实现，关闭后允许重新装载模型。
#ifndef LIVE2DRENDERER_H
#define LIVE2DRENDERER_H

#include "kanban/KanbanRenderer.h"

#include <memory>

namespace kanban {

class Live2DRenderer : public KanbanRenderer
{
public:
    Live2DRenderer();
    ~Live2DRenderer() override;

    QString backendName() const override { return QStringLiteral("Live2D"); }

    // 编译期事实：本 target 是否真的把 Cubism Framework/Core 链进来了。
    // 控制器据此决定是否尝试 Live2D，而不是每次都靠 initialize() 失败去发现。
    static bool sdkCompiledIn();
    // 供界面与日志回答「为什么看不到 Live2D 效果」。
    static QString unavailableReason();

    // 未接入时返回 false，outError 说明「未接入」而不是「模型坏了」。
    bool initialize(QString *outError) override;
    bool loadModel(const QString &modelJsonPath, QString *outError) override;
    void unloadModel() override;

    // 恒为 true：Live2D 一定要 GL 宿主，窗口据此选 QOpenGLWidget 承载。
    bool usesOpenGL() const override;

    void resize(int width, int height, float devicePixelRatio) override;
    void update(float deltaSeconds) override;
    void render() override;
    void paint(QPainter *painter, const QSize &logicalSize) override;

    // 视图把 GL 宿主交下来(见 KanbanGlHost)。两份实现都要给定义：
    // 未接入 SDK 的那一份什么都不做，接入的那一份存进 Private。
    void setGlHost(KanbanGlHost *host) override;

    void pointerMove(const QPointF &pos) override;
    // 视线追踪强度(无/弱/中/强)。切到「无」时把模型平滑地放回正面，
    // 在档位之间切换时立刻按新档位重算目标(见实现处的说明)。
    void setGazeStrength(int strength) override;

    // 装载前设置；预览图固定使用首个待机动作，正常运行保持随机。
    void setDeterministicIdle(bool on) { m_deterministicIdle = on; }

    // 视线相关参数的实时快照(值 + 取值范围)，诊断探针用。
    // 「鼠标动但模型不转头」有三种成因：参数名对不上、被运动每帧压回、
    // 模型参数范围本来就窄 —— 只有把值打出来才分得清是哪种。
    QString gazeDebugText() const;
    void pointerClick(const QPointF &pos) override;
    bool playNextMotion() override;
    // 可播动作数(不含 idle)。两份实现都要给定义：接入 SDK 的那份按模型文件算，
    // 未接入的那份恒为 0 —— 界面据此把「播放下一个动作」置灰。
    int playableMotionCount() const override;
    // 表情走 ExpressionMotionManager，与动作的 MotionManager 互不抢占优先级，
    // 所以「切表情」不会打断正在播的动作(见 KanbanRenderer 的说明)。
    int expressionCount() const override;
    bool playNextExpression() override;

    void pause() override;
    void resume() override;
    void shutdown() override;

private:
    struct Private; // 宿主状态与模型所有权

    bool m_ready = false;
    bool m_modelLoaded = false;
    bool m_paused = false;
    // 待机动作是否固定取第 0 个(见 setDeterministicIdle)。默认关 = 产品行为。
    bool m_deterministicIdle = false;
    int m_width = 320;
    int m_height = 480;
    float m_dpr = 1.0f;
    QString m_modelPath;
    std::unique_ptr<Private> m_d;
};

} // namespace kanban

#endif // LIVE2DRENDERER_H

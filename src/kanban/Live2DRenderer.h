// Live2D 后端入口，不向控制器暴露 SDK 类型。GL 资源在宿主上下文内释放，
// CubismRuntime 保持进程级框架存活；构建时选择 Cubism 或 Stub 实现。
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

    // 编译期事实：本 target 是否真把 Cubism 链进来了，控制器据此决定是否尝试 Live2D。
    static bool sdkCompiledIn();
    static QString unavailableReason();

    // 未接入时返回 false，outError 说明「未接入」而不是「模型坏了」。
    bool initialize(QString *outError) override;
    bool loadModel(const QString &modelJsonPath, QString *outError) override;
    void unloadModel() override;

    bool usesOpenGL() const override;

    void resize(int width, int height, float devicePixelRatio) override;
    bool rebuildTexturesIfNeeded() override;
    void update(float deltaSeconds) override;
    void render() override;
    void paint(QPainter *painter, const QSize &logicalSize) override;

    void setGlHost(KanbanGlHost *host) override;

    void pointerMove(const QPointF &pos) override;
    // 视线追踪强度(无/弱/中/强)。切到「无」时把模型平滑放回正面。
    void setGazeStrength(int strength) override;

    // 预览图固定用第 0 个待机动作，正常运行保持随机。
    void setDeterministicIdle(bool on) { m_deterministicIdle = on; }

    // 视线参数的实时快照，诊断探针用。「鼠标动但模型不转头」有三种成因(参数名对不上、
    // 被运动每帧压回、参数范围窄)，只有把值打出来才分得清。
    QString gazeDebugText() const;
    void pointerClick(const QPointF &pos) override;
    bool playNextMotion() override;
    // 可播动作数(不含 idle)；未接入的那份恒为 0，界面据此把入口置灰。
    int playableMotionCount() const override;
    // 表情走 ExpressionMotionManager，与 MotionManager 互不抢占优先级，切表情不会
    // 打断正在播的动作。
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
    // 待机动作是否固定取第 0 个。默认关 = 产品行为。
    bool m_deterministicIdle = false;
    int m_width = 320;
    int m_height = 480;
    float m_dpr = 1.0f;
    QString m_modelPath;
    std::unique_ptr<Private> m_d;
};

} // namespace kanban

#endif // LIVE2DRENDERER_H

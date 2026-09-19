// 占位渲染器：Live2D Cubism Native SDK 未接入时的看板娘画面。
//
// 为什么不用一张静态 PNG：需要的是可运行的动画循环、状态机、交互与
// 降级的联调载体，静态图验证不了时钟/暂停/缩放/透明度/穿透这条链路。这里用 QPainter
// 直接画一个会呼吸、会眨眼、会跟视线的角色，无位图搬运、无中间图像。
// SDK 接入后本类整体退役，控制器换掉 m_renderer 指向即可，其它模块零改动。
// SDK 接入后本类整体退役：KanbanController 换掉 m_renderer 指向即可，其它模块零改动。
#ifndef KANBANPLACEHOLDERRENDERER_H
#define KANBANPLACEHOLDERRENDERER_H

#include "kanban/KanbanRenderer.h"

#include <QImage>

namespace kanban {

class PlaceholderRenderer : public KanbanRenderer
{
public:
    QString backendName() const override { return QStringLiteral("Placeholder"); }

    // 软件绘制路径：帧在 paint() 里产出，窗口无需做成 OpenGL 宿主。
    bool usesOpenGL() const override { return false; }

    bool initialize(QString *outError) override;
    bool loadModel(const QString &modelJsonPath, QString *outError) override;
    void unloadModel() override;

    void resize(int width, int height, float devicePixelRatio) override;
    void update(float deltaSeconds) override;
    void render() override {} // 软件绘制路径：帧在 paint() 里产出
    void paint(QPainter *painter, const QSize &logicalSize) override;

    void pointerMove(const QPointF &pos) override;
    // 视线追踪强度(无/弱/中/强)。切到「无」时把视线平滑带回中心。
    void setGazeStrength(int strength) override;
    void pointerClick(const QPointF &pos) override;
    bool playNextMotion() override;
    // 3 个写死的动作，无 idle 组概念，所以 3 个全是「可播动作」—— 降级路径下入口
    // 不该变成死按钮。
    int playableMotionCount() const override;

    // 占位后端也有表情，降级路径下「切换表情」不该变成死按钮。
    int expressionCount() const override;
    bool playNextExpression() override;

    void pause() override { m_paused = true; }
    void resume() override { m_paused = false; }
    void shutdown() override;

private:
    // 一次性动作(眨眼/点头/小跳)：到点自动结束，不留悬挂状态。
    struct TimedMotion {
        QString name;
        float elapsed = 0.0f;
        float duration = 0.0f;
    };

    bool m_ready = false;
    bool m_paused = false;
    int m_width = 320;
    int m_height = 480;
    float m_dpr = 1.0f;
    QString m_modelName;
    QImage m_modelTexture;  // 模型纹理，用于占位渲染器显示不同模型

    float m_time = 0.0f;        // 累计动画时间(暂停时不涨)
    float m_blinkLeft = 0.0f;   // 0=睁眼 1=闭眼
    float m_nextBlinkIn = 2.0f;
    QPointF m_gaze;             // 视线偏移(-1..1)，由鼠标位置换算
    QPointF m_gazeTarget;
    // 最后收到的原始窗口坐标：换档位时要按新半径重算，只存归一化值不够
    // (那是按旧半径算的)。x < 0 表示还没喂过。与 Live2D 侧同名同义。
    QPointF m_lastPointer{-1.0, -1.0};
    TimedMotion m_motion;
    bool m_motionActive = false;
    int m_motionIndex = 0;     // 见 kMotions，索引即「下一个动作」的游标
    int m_expressionIndex = 0; // 见 kExpressions，索引即「当前表情」
};

} // namespace kanban

#endif // KANBANPLACEHOLDERRENDERER_H

// 占位渲染器：Live2D Cubism Native SDK 未接入时的看板娘画面。
//
// 为什么不是「先放一张 PNG 上去」：任务书 §5.3/§13.1 要的是可运行的动画循环、
// 状态机、交互与降级的联调载体，静态图验证不了时钟/暂停/缩放/透明度/穿透这条链路。
// 所以这里用 QPainter 直接画一个会呼吸、会眨眼、会跟视线的角色。
//
// 与任务书 §5.1 禁止项的关系：禁止的是「Live2D 渲染→截图→QImage→QPixmap→QLabel」
// 这种每帧位图搬运。本类在 paintEvent 里用 vector 绘制，无位图、无中间图像、无拷贝；
// 每帧只做常数次路径构造，且都落在复用窗口上(update() 走脏区重绘)。
//
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
    void pointerClick(const QPointF &pos) override;
    bool playNextMotion() override;

    // 占位后端也有表情(见 .cpp 的 kExpressions)：降级路径下右键菜单的
    // 「切换表情」不该变成死按钮，界面行为不因后端而不同。
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
    TimedMotion m_motion;
    bool m_motionActive = false;
    int m_expressionIndex = 0; // 见 kExpressions，索引即「当前表情」
};

} // namespace kanban

#endif // KANBANPLACEHOLDERRENDERER_H

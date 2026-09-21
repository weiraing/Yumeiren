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

class QDir;

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
    // 绘制面变大到「手上这张位图已经不够细」时按新上限重解一次，真的重解了返回 true。
    // 与 Live2D 后端同一条契约(见 KanbanRenderer.h)；没有它，窗口放大后这张位图只是
    // 被拉大画出去 —— 糊，而 Live2D 侧不糊，降级路径不该在这一点上缩水。
    bool rebuildTexturesIfNeeded() override;
    void update(float deltaSeconds) override;
    void render() override {} // 软件绘制路径：帧在 paint() 里产出
    void paint(QPainter *painter, const QSize &logicalSize) override;

    // 纹理状态的只读快照，仅供离屏诊断探针判读(与 motionDebugText 同类)。
    QString textureDebugText() const override;

    void pointerMove(const QPointF &pos) override;
    // 视线追踪强度(无/弱/中/强)。切到「无」时把视线平滑带回中心。
    void setGazeStrength(int strength) override;
    void pointerClick(const QPointF &pos) override;
    bool playNextMotion() override;
    // 3 个写死的动作，无 idle 组概念，所以 3 个全是「可播动作」—— 降级路径下入口
    // 不该变成死按钮。
    int playableMotionCount() const override;
    int currentMotionOrdinal() const override { return m_currentMotionOrdinal; }

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

    // 在模型目录里按候选名找纹理并按当前绘制面上限解码。找不到返回 false，且**不动**
    // 已有的 m_modelTexture(重解失败时保持原来那张，糊一点总比空着好)；
    // 成功时同时记下源文件与它自身的尺寸(供后续重算上限，不必再读盘)。
    bool decodeModelTexture(const QDir &modelDir);
    // 「按当前绘制面 + 素材尺寸，这张纹理该缩到多长边」，0 = 原尺寸。
    // 只做算术：源尺寸由调用方给，每帧调用不读盘。
    int effectiveTextureLimit(const QSize &source) const;

    bool m_ready = false;
    bool m_paused = false;
    int m_width = 320;
    int m_height = 480;
    float m_dpr = 1.0f;
    QString m_modelName;
    QImage m_modelTexture;  // 模型纹理，用于占位渲染器显示不同模型
    // 模型目录。候选纹理名(textures/texture_00.png 等)是相对**模型目录**的，
    // 重解时必须拿它去找 —— 别用纹理文件自己的目录(那是 textures/ 一层，找不到)。
    QString m_modelDir;
    // 解出 m_modelTexture 的那个源文件，以及它自身的长边尺寸。
    // 留着是为了重算上限时不必再读盘 —— 每帧都要问一次「现在该多细」。
    QString m_modelTexturePath;
    QSize m_modelSourceSize;
    // m_modelTexture 是按哪个上限解的(0 = 原尺寸)。重解的判据就是「新上限比它更细」。
    int m_modelTextureLimit = 0;
    // 已经失败过的上限：同一个上限不再重试(每帧重试就是每帧读盘解码)。
    // 与 m_modelTextureLimit 分开存，是为了让 textureDebugText() 说实话 ——
    // 失败时纹理其实还是旧的那张，报成「已用新上限」会把问题藏起来。
    int m_modelTextureFailedLimit = 0;

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
    bool m_motionIsPlayable = false;
    int m_motionIndex = 0;     // 见 kMotions，索引即「切换动作」的游标
    int m_currentMotionOrdinal = 0;
    int m_expressionIndex = 0; // 见 kExpressions，索引即「当前表情」
};

} // namespace kanban

#endif // KANBANPLACEHOLDERRENDERER_H

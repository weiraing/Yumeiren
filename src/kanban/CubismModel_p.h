#ifndef CUBISMMODEL_P_H
#define CUBISMMODEL_P_H

class QAudioOutput;

// 后端私有头；GLEW 必须早于其他 GL 头文件。
#include <GL/glew.h>

#include "kanban/CubismModel.h"
#include "kanban/CubismRuntime.h"  // stopMotionSound 的诊断日志
#include "kanban/KanbanRenderer.h" // 纹理上限策略(两个后端共用一份)
#include <QImage>
#include <QStringList>
#include <QVector>

#include <vector>

#include <QMediaPlayer>

#include <algorithm>

#include "CubismDefaultParameterId.hpp"
#include "CubismFramework.hpp"
#include "CubismModelSettingJson.hpp"
#include "Id/CubismIdManager.hpp"
#include "Math/CubismMatrix44.hpp"
#include "Math/CubismTargetPoint.hpp"
#include "Model/CubismUserModel.hpp"
#include "Motion/ACubismMotion.hpp"
#include "Type/csmMap.hpp"

namespace kanban::detail {

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::DefaultParameterId;

// 模型装配与状态；CPU 装载与 GL 上传分两步走。
class CubismModelImpl final : public CubismUserModel, public kanban::CubismModel
{
public:
    ~CubismModelImpl() override
    {
        releaseGl();
        releaseCpu();
    }

    bool setup(const QString &modelJsonPath, QString *outError) override;
    // 仅在宿主上下文当前化后创建或释放 GL 资源。
    bool ensureGl(const QSize &pixelSize, quint64 contextGeneration, QString *outError) override;
    void releaseGl() override;
    void update(float deltaSeconds) override;
    void draw(const QSize &pixelSize) override;
    void setDragTarget(float x, float y) override;
    void setDeterministicIdle(bool on) override { m_deterministicIdle = on; }
    void setMotionLoopEnabled(bool enabled) override { m_motionLoopEnabled = enabled; }
    void setSoundEnabled(bool enabled) override
    {
        m_soundEnabled = enabled;
        // 关掉时顺手掐断正在播的那句：否则用户勾掉之后，当前动作的语音还会放完。
        if (!enabled && m_voicePlayer) {
            m_voicePlayer->stop();
        }
    }
    void stopMotionSound() override
    {
        if (!m_voicePlayer) {
            return;
        }
        // 只在真在播时记一笔：这条日志是「暂停有没有掐到声音」的判据。
        const bool playing = m_voicePlayer->playbackState() == QMediaPlayer::PlayingState;
        m_voicePlayer->stop();
        if (playing) {
            cubismruntime::logInfo(QStringLiteral("动作语音已掐断"));
        }
    }
    bool startHitReaction(const QPointF &normalized) override;
    bool playNextMotion() override;
    bool playNextExpression() override;

    void setViewportSize(const QSize &pixelSize) override
    {
        SetRenderTargetSize(static_cast<csmUint32>(pixelSize.width()),
                            static_cast<csmUint32>(pixelSize.height()));
    }
    int textureCount() const override { return m_setting ? m_setting->GetTextureCount() : 0; }
    int motionGroupCount() const override { return m_motionGroups.size(); }
    int expressionCount() const override { return m_expressionNames.size(); }
    int playableMotionCount() const override { return m_playableMotions.size(); }
    int currentMotionOrdinal() const override { return m_currentMotionOrdinal; }

    // 输出参数值与范围。
    QString gazeDebugText() override
    {
        if (!_model) {
            return QStringLiteral("无模型");
        }
        struct Item { const csmChar *id; const char *label; };
        static const Item items[] = {
            {ParamAngleX, "ParamAngleX"},
            {ParamAngleY, "ParamAngleY"},
            {ParamBodyAngleX, "ParamBodyAngleX"},
            {ParamEyeBallX, "ParamEyeBallX"},
            {ParamEyeBallY, "ParamEyeBallY"},
        };
        QStringList parts;
        // 缺失参数返回虚拟索引(不是负数)，必须与实际参数总数比较。
        const csmInt32 total = _model->GetParameterCount();
        for (const Item &it : items) {
            const CubismIdHandle id = CubismFramework::GetIdManager()->GetId(it.id);
            const csmInt32 idx = _model->GetParameterIndex(id);
            if (idx >= total) {
                parts << QStringLiteral("%1=缺失").arg(QLatin1String(it.label));
                continue;
            }
            parts << QStringLiteral("%1=%2[%3~%4]")
                         .arg(QLatin1String(it.label))
                         .arg(_model->GetParameterValue(idx), 0, 'f', 2)
                         .arg(_model->GetParameterMinimumValue(idx), 0, 'f', 0)
                         .arg(_model->GetParameterMaximumValue(idx), 0, 'f', 0);
        }
        return parts.join(QStringLiteral(" "));
    }

    QString dragDebugText() const override
    {
        if (!_dragManager) {
            return QStringLiteral("无 dragManager");
        }
        return QStringLiteral("drag 平滑后=(%1,%2)")
            .arg(_dragManager->GetX(), 0, 'f', 3)
            .arg(_dragManager->GetY(), 0, 'f', 3);
    }

    QString motionDebugText() const override
    {
        if (!_model) {
            return QStringLiteral("无模型");
        }
        struct Item { const csmChar *id; const char *label; };
        static const Item params[] = {
            {ParamArmRA, "RA"}, {ParamArmRB, "RB"},
            {ParamArmLA, "LA"}, {ParamArmLB, "LB"},
        };
        static const Item parts[] = {
            {"Part01ArmRA001", "pRA"}, {"Part01ArmRB001", "pRB"},
            {"Part01ArmLA001", "pLA"}, {"Part01ArmLB001", "pLB"},
        };
        QStringList out;
        const csmInt32 paramTotal = _model->GetParameterCount();
        for (const Item &it : params) {
            const CubismIdHandle id = CubismFramework::GetIdManager()->GetId(it.id);
            const csmInt32 index = _model->GetParameterIndex(id);
            if (index < 0 || index >= paramTotal) {
                out << QStringLiteral("%1=缺失").arg(QLatin1String(it.label));
                continue;
            }
            out << QStringLiteral("%1=%2")
                       .arg(QLatin1String(it.label))
                       .arg(_model->GetParameterValue(index), 0, 'f', 3);
        }
        const csmInt32 partTotal = _model->GetPartCount();
        for (const Item &it : parts) {
            const CubismIdHandle id = CubismFramework::GetIdManager()->GetId(it.id);
            const csmInt32 index = _model->GetPartIndex(id);
            if (index < 0 || index >= partTotal) {
                out << QStringLiteral("%1=缺失").arg(QLatin1String(it.label));
                continue;
            }
            out << QStringLiteral("%1=%2")
                       .arg(QLatin1String(it.label))
                       .arg(_model->GetPartOpacity(index), 0, 'f', 3);
        }
        if (_pose) {
            out << QStringLiteral("pose=on");
        } else {
            out << QStringLiteral("pose=off");
        }
        return out.join(QLatin1Char(' '));
    }

    QString partOpacityText(const QStringList &ids) const override
    {
        if (!_model) {
            return QStringLiteral("无模型");
        }
        QStringList out;
        const csmInt32 total = _model->GetPartCount();
        for (const QString &id : ids) {
            const QByteArray utf8 = id.toUtf8();
            const csmInt32 index =
                _model->GetPartIndex(CubismFramework::GetIdManager()->GetId(utf8.constData()));
            if (index < 0 || index >= total) {
                out << QStringLiteral("%1=缺失").arg(id);
                continue;
            }
            out << QStringLiteral("%1=%2").arg(id).arg(_model->GetPartOpacity(index), 0, 'f', 3);
        }
        return out.join(QLatin1Char(' '));
    }

    QString drawableOpacityText(const QStringList &ids) const override
    {
        if (!_model) {
            return QStringLiteral("无模型");
        }
        QStringList out;
        const csmInt32 total = _model->GetDrawableCount();
        for (const QString &id : ids) {
            const QByteArray utf8 = id.toUtf8();
            const csmInt32 index = _model->GetDrawableIndex(
                CubismFramework::GetIdManager()->GetId(utf8.constData()));
            if (index < 0 || index >= total) {
                out << QStringLiteral("%1=缺失").arg(id);
                continue;
            }
            out << QStringLiteral("%1=%2")
                       .arg(id)
                       .arg(_model->GetDrawableOpacity(index), 0, 'f', 3);
        }
        return out.join(QLatin1Char(' '));
    }

    QString currentExpressionName() const override
    {
        return m_lastExpression >= 0 && m_lastExpression < m_expressionNames.size()
                   ? m_expressionNames.at(m_lastExpression)
                   : QString();
    }

    QString meshHideText() const override;
    void refreshMeshHide() override;

    // 上下文销毁后只清除句柄记录，不再发送 GL 调用；渲染器仍须由控制器在窗口销毁前 shutdown。
    void invalidateGl() override
    {
        m_glLive = false;
        m_textureIds.clear();
    }

private:
    bool startIdleMotion();
    bool setExpressionIndex(int index);
    bool playRandomExpression();
    void playMotionSound(const QString &group, int index);
    QString relativeToHome(const csmChar *relative) const;
    bool loadSettingJson(const QString &jsonPath, QString *outError);
    void releaseCpu();
    // 只判断每张纹理能否解码、不产出位图：坏图要在装载阶段就报出来，而全尺寸位图
    // 一旦为「校验」常驻就白白占掉几百 MB。
    bool validateTextures(QString *outError);
    // windowMaxDim = 窗口尺寸推出的纹理最长边上限(0 = 原尺寸)；实际上限还叠了一条与
    // 素材尺寸挂钩的质量底线，见 KanbanRenderer.h 的 textureMaxDimFor()。
    bool decodeTextures(int windowMaxDim, QString *outError);
    // 仅返回预载成功的动作序号。
    QVector<int> preloadMotionGroup(const QString &group);
    // 按网格隐藏：清单来自模型目录里的 *.hidden.json(查看器导出)，在 setup() 末尾解析成
    // drawable 下标；此后每帧 update() 之后按开关清 IsVisible 位。
    void loadMeshHideList();
    void applyMeshHide();
    void fitProjection(const QSize &pixelSize, CubismMatrix44 *out);
    int motionCount(int group) const;
    bool isIdleMotion(int group, int index) const;
    bool startGroupMotion(int group, int index, int priority);
    static CubismIdHandle parameterId(const csmChar *name);

    float m_projScaleX = 1.0f;
    float m_projScaleY = 1.0f;
    QStringList m_motionKeys; // 与 m_motions 的键同序，释放时使用
    CubismModelSettingJson *m_setting = nullptr;
    csmMap<csmString, ACubismMotion *> m_motions;
    csmMap<csmString, ACubismMotion *> m_expressions;
    csmVector<CubismIdHandle> m_eyeBlinkIds;
    csmVector<CubismIdHandle> m_lipSyncIds;
    QMediaPlayer *m_voicePlayer = nullptr;
    QVector<QImage> m_textureImages;
    // m_textureImages 是按哪个最长边上限解出来的(0 = 原尺寸)：上限变了必须重解，
    // 而 m_textureImages 上传后即清空，所以下一次上传一定重解。
    int m_textureMaxDim = 0;
    // 首次解码失败后记住原因：ensureGl 由动画时钟逐帧重试，不缓存就成了每帧读盘解码。
    QString m_decodeError;
    std::vector<GLuint> m_textureIds;
    QStringList m_motionGroups;
    // 预载成功的全部动作(含 idle)，按配置顺序保存组号与组内序号。
    QVector<QPair<int, int>> m_playableMotions;
    int m_motionCursor = 0;
    int m_currentMotionOrdinal = 0;
    QStringList m_expressionNames;
    QString m_homeDir;
    // 按网格隐藏：清单解析结果(已排序去重的 drawable 下标)。空 = 本模型没有清单文件。
    std::vector<csmInt32> m_meshHideDrawables;
    int m_meshHideFiles = 0;   // 读进来的清单文件份数
    int m_meshHideRules = 0;   // 清单声明的条目数(部件 + 网格)
    int m_meshHideMissing = 0; // 清单里没在模型中找到的 Id 条数
    bool m_glLive = false;
    int m_idleGroup = -1;
    bool m_deterministicIdle = false;
    bool m_motionLoopEnabled = true;
    bool m_soundEnabled = true;
    int m_nextExpression = 0;
    int m_lastExpression = -1;
    csmBool m_motionUpdated = false;
};

} // namespace kanban::detail

#endif // CUBISMMODEL_P_H

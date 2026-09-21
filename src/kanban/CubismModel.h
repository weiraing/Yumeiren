#ifndef CUBISMMODEL_H
#define CUBISMMODEL_H

#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>

#include <memory>

namespace kanban {

// 后端内部模型接口：隔离 SDK 类型，调用方负责提供当前 GL 上下文。
class CubismModel
{
public:
    virtual ~CubismModel() = default;

    virtual bool setup(const QString &modelPath, QString *outError) = 0;
    virtual bool ensureGl(const QSize &pixelSize, quint64 generation, QString *outError) = 0;
    virtual void releaseGl() = 0;
    // 上下文丢失时只作废句柄；正常关闭应在上下文销毁前释放模型。
    virtual void invalidateGl() = 0;
    virtual void setViewportSize(const QSize &pixelSize) = 0;
    virtual void update(float deltaSeconds) = 0;
    virtual void draw(const QSize &pixelSize) = 0;

    virtual void setDragTarget(float x, float y) = 0;
    virtual void setDeterministicIdle(bool enabled) = 0;
    virtual void setMotionLoopEnabled(bool enabled) = 0;
    // 动作语音开关。关掉后 playMotionSound 直接返回，模型自带语音也不播。
    virtual void setSoundEnabled(bool enabled) = 0;
    /// @brief 立即掐断当前正在播放的动作语音；播放器还没建时无副作用。
    /// @note 与 setSoundEnabled(false) 的区别：只打断这一句，不改后续动作发不发声 ——
    ///       暂停看板娘时用它，不能顺手把用户「播放声音」这个偏好改掉。
    virtual void stopMotionSound() = 0;
    virtual bool startHitReaction(const QPointF &normalized) = 0;
    virtual bool playNextMotion() = 0;
    virtual bool playNextExpression() = 0;

    virtual int textureCount() const = 0;
    virtual int motionGroupCount() const = 0;
    virtual int playableMotionCount() const = 0;
    // 最近一次手动/命中触发的可播动作序号(从 1 开始)；还没有触发过时为 0。
    virtual int currentMotionOrdinal() const = 0;
    virtual int expressionCount() const = 0;
    virtual QString currentExpressionName() const = 0;
    virtual QString gazeDebugText() = 0;
    virtual QString dragDebugText() const = 0;
    // 动作/部件诊断：双臂参数与 Pose 管理的部件透明度快照。探针用。
    virtual QString motionDebugText() const = 0;
    /// @brief 按部件 Id 取当前不透明度快照，探针断言 pose 是否生效用。
    /// @param ids 部件 Id 列表；查不到的 Id 标注为「缺失」而不是报错。
    /// @return 「Id=0.000」用空格连接的一行。
    /// @note 快照来自 Pose 每帧写过的值，所以读它等价于读「画面上到底显示了几层」。
    virtual QString partOpacityText(const QStringList &ids) const = 0;
    /// @brief 按 drawable Id 取当前不透明度快照，探针用来判断「这层贴图到底画没画」。
    /// @param ids drawable Id 列表；查不到的 Id 标注为「缺失」而不是报错。
    /// @return 「Id=0.000」用空格连接的一行。
    /// @note 与 partOpacityText 的区别：drawable 不透明度还乘上了模型自己的参数绑定
    ///       (美术把某层的不透明度挂到参数上)，所以它才是「最终画不画」的判据 ——
    ///       只看部件会得出「全都可见」的相反结论。
    virtual QString drawableOpacityText(const QStringList &ids) const = 0;
    /// @brief 网格隐藏清单的一句话摘要，界面状态行与诊断探针共用；本模型没有清单时返回空串。
    /// @note 清单 = 模型目录里的 `*.hidden.json`(由 tools/live2d-part-inspector 导出)，
    ///       内容是要隐藏的部件 Id 与网格 Id。清单只在装载时读一次，改文件要重新装载。
    virtual QString meshHideText() const = 0;
    /// @brief 把「按清单隐藏网格」这个开关**立刻**作用到画面上。
    /// @note 正常运行时每帧 update() 都会重写一遍，但暂停时时钟不走 —— 用户在暂停状态下
    ///       切开关，不主动刷一次就要等恢复动画才看得到，看起来像开关失灵。
    virtual void refreshMeshHide() = 0;
};

std::unique_ptr<CubismModel> createCubismModel();

} // namespace kanban

#endif // CUBISMMODEL_H

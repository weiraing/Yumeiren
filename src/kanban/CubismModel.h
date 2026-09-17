#ifndef CUBISMMODEL_H
#define CUBISMMODEL_H

#include <QPointF>
#include <QSize>
#include <QString>

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
    virtual bool startHitReaction(const QPointF &normalized) = 0;
    virtual bool playNextMotion() = 0;
    virtual bool playNextExpression() = 0;

    virtual int textureCount() const = 0;
    virtual int motionGroupCount() const = 0;
    virtual int playableMotionCount() const = 0;
    virtual int expressionCount() const = 0;
    virtual QString currentExpressionName() const = 0;
    virtual QString gazeDebugText() = 0;
    virtual QString dragDebugText() const = 0;
};

std::unique_ptr<CubismModel> createCubismModel();

} // namespace kanban

#endif // CUBISMMODEL_H

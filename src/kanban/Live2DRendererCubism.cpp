#include "kanban/Live2DRenderer.h"
#include "kanban/CubismModel.h"
#include "kanban/CubismRuntime.h"

#include <QFileInfo>

#include <algorithm>
#include <utility>

namespace kanban {

using cubismruntime::GlScope;
using cubismruntime::logDebug;
using cubismruntime::logInfo;
using cubismruntime::logWarn;

struct Live2DRenderer::Private
{
    std::unique_ptr<CubismModel> model;
    KanbanGlHost *host = nullptr;
    bool glBuilt = false;
    bool glewReady = false;
    QSize pixelSize;
    bool textureRebuildPending = false;
    float gazeX = 0.0f;
    float gazeY = 0.0f;
    QPointF lastPointer{-1.0, -1.0};
    bool gazeSeen = false;
};

bool Live2DRenderer::sdkCompiledIn()
{
    return true;
}

QString Live2DRenderer::unavailableReason()
{
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
    if (host == m_d->host) {
        return;
    }
    if (m_d->model) {
        // 换宿主即作废旧句柄，避免在别的上下文里误用。
        m_d->model->invalidateGl();
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

    if (!cubismruntime::initializeFramework(outError)) {
        return false;
    }

    m_ready = true;
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

    auto model = createCubismModel();
    if (!model->setup(modelJsonPath, outError)) {
        return false;
    }
    m_d->textureRebuildPending = false;

    model->setDeterministicIdle(m_deterministicIdle);
    model->setMotionLoopEnabled(m_motionLoopEnabled);
    model->setSoundEnabled(m_soundEnabled);
    m_modelPath = modelJsonPath;
    m_modelLoaded = true;
    logInfo(QStringLiteral("已装载 %1：纹理 %2 张 / 动作组 %3 个(可播动作 %4 个) / 表情 %5 个")
                .arg(QFileInfo(modelJsonPath).completeBaseName())
                .arg(model->textureCount())
                .arg(model->motionGroupCount())
                .arg(model->playableMotionCount())
                .arg(model->expressionCount()));

    // 控制器换模型时未必在 paintGL 里；建不起来就交给第一次 render() 补，不算失败。
    if (m_d->host) {
        const GlScope scope(m_d->host, &m_d->glewReady);
        const QSize pixels = m_d->host->glPixelSize();
        if (scope.ok() && pixels.width() > 0 && pixels.height() > 0) {
            QString glError;
            if (model->ensureGl(pixels, m_d->host->glContextGeneration(), &glError)) {
                m_d->glBuilt = true;
                m_d->pixelSize = pixels;
            } else {
                logWarn(QStringLiteral("纹理上传延后：%1").arg(glError));
            }
        } else if (!scope.ok()) {
            logWarn(QStringLiteral("装载时上下文不可用，纹理上传延后到首帧"));
        }
    }
    m_d->model = std::move(model);
    // 装载完立刻把状态落定一次。理由有两层：
    // ① pose 的互斥显隐与 *.hidden.json 都**只在 update() 里生效** —— applyMeshHide() 挂在
    //    update() 末尾，IsVisible 位是 Core 在 Update() 里从 opacities 重算的；装载后部件
    //    opacities 全是默认值 1，于是 pose 组里的两个部件、以及清单里该藏的网格**全可见**。
    // ② 真实程序的首帧 paintGL 由窗口系统触发，比帧 tick 的第一次 update() 更早(实测日志里
    //    首帧 paintGL 早于 activateKanban 起钟 0.6s)，所以那一帧画的就是①那个全开状态 ——
    //    用户看到的「隐藏内容闪一下」就是它。
    // 直接调模型的 update 而不是本类的：本类那份带 m_paused 判断，而暂停中装载也要落定。
    m_d->model->update(0.0f);
    return true;
}

void Live2DRenderer::unloadModel()
{
    auto model = std::move(m_d->model);
    m_d->glBuilt = false;
    m_d->pixelSize = QSize();
    m_modelLoaded = false;
    if (!model) {
        return;
    }

    // 正常释放必须取得宿主上下文；上下文丢失时仅作废句柄记录。
    const GlScope scope(m_d->host, &m_d->glewReady);
    if (scope.ok()) {
        model->releaseGl();
    } else {
        model->invalidateGl();
    }
    // 必须在 scope 归还上下文前完成模型析构。
    model.reset();
}

void Live2DRenderer::resize(int width, int height, float devicePixelRatio)
{
    m_width = width;
    m_height = height;
    m_dpr = devicePixelRatio;

    // 纹理上限只在「变大到需要更细的一档」时补：缩小不回退，拖缩放时不该每步重解素材。
    // 判据取宿主的 glPixelSize；真正的重建留给帧 tick。
    const QSize pixels = m_d->host ? m_d->host->glPixelSize() : QSize();
    if (m_modelLoaded && !pixels.isEmpty() && !m_d->pixelSize.isEmpty()
        && textureMaxDimFor(pixels) > textureMaxDimFor(m_d->pixelSize)) {
        m_d->textureRebuildPending = true;
    }
}

bool Live2DRenderer::rebuildTexturesIfNeeded()
{
    if (!m_d->textureRebuildPending) {
        return false;
    }
    m_d->textureRebuildPending = false;
    if (m_modelPath.isEmpty()) {
        return false;
    }
    QString err;
    if (loadModel(m_modelPath, &err)) {
        logInfo(QStringLiteral("绘制面变大到 %1x%2，纹理按新上限 %3 重建")
                    .arg(m_d->pixelSize.width())
                    .arg(m_d->pixelSize.height())
                    .arg(textureMaxDimFor(m_d->pixelSize)));
        return true;
    }
    // 失败不会退回原画面，只会空着；不逐帧重试(那是每帧读盘解码)。
    logWarn(QStringLiteral("纹理按新上限重建失败：%1").arg(err));
    return false;
}

void Live2DRenderer::update(float deltaSeconds)
{
    if (!m_modelLoaded || m_paused || !m_d->model) {
        return;
    }
    m_d->model->update(deltaSeconds);
}

void Live2DRenderer::paint(QPainter *painter, const QSize &logicalSize)
// GPU 路径由 render() 负责，本后端不存在软件绘制路径。
{
    Q_UNUSED(painter)
    Q_UNUSED(logicalSize)
}

void Live2DRenderer::render()
{
    CubismModel *model = m_d->model.get();
    if (!model || !m_d->host) {
        return;
    }
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
        if (!model->ensureGl(pixels, m_d->host->glContextGeneration(), &glError)) {
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
        model->setViewportSize(pixels);
        m_d->pixelSize = pixels;
    }

    model->draw(pixels);
}

void Live2DRenderer::pointerMove(const QPointF &pos)
{
    if (!m_d->model || !gazeEnabled()) {
        return;
    }

    // 光标转成 -1..1 方向，并扩大作用半径避免小窗口过早饱和。
    const float w = m_width > 0 ? static_cast<float>(m_width) : 1.0f;
    const float h = m_height > 0 ? static_cast<float>(m_height) : 1.0f;

    const GazeTuning &tuning = kGazeTuning[clampGazeStrength(m_gazeStrength)];
    const float radiusX = std::max(w * tuning.radiusFactor, kGazeMinRadiusPx);
    const float radiusY = std::max(h * tuning.radiusFactor, kGazeMinRadiusPx);

    const float dx = static_cast<float>(pos.x()) - w * 0.5f;
    const float dy = static_cast<float>(pos.y()) - h * 0.5f;

    const float nx = qBound(-1.0f, dx / radiusX, 1.0f);
    const float ny = qBound(-1.0f, -dy / radiusY * tuning.verticalScale, 1.0f);

    m_d->gazeX = nx;
    m_d->gazeY = ny;
    m_d->gazeSeen = true;
    m_d->lastPointer = pos;
    m_d->model->setDragTarget(nx, ny);
    // 只写目标方向，平滑过渡由模型更新器完成。
}

QString Live2DRenderer::gazeDebugText() const
{
    if (!m_d->model) {
        return QStringLiteral("无模型");
    }
    return QStringLiteral("%1 | %2 | 目标=(%3,%4)")
        .arg(m_d->model->gazeDebugText())
        .arg(m_d->model->dragDebugText())
        .arg(m_d->gazeX, 0, 'f', 3)
        .arg(m_d->gazeY, 0, 'f', 3);
}

QString Live2DRenderer::motionDebugText() const
{
    return m_d->model ? m_d->model->motionDebugText() : QStringLiteral("无模型");
}

QString Live2DRenderer::partOpacityText(const QStringList &ids) const
{
    return m_d->model ? m_d->model->partOpacityText(ids) : QStringLiteral("无模型");
}

QString Live2DRenderer::drawableOpacityText(const QStringList &ids) const
{
    return m_d->model ? m_d->model->drawableOpacityText(ids) : QStringLiteral("无模型");
}

QString Live2DRenderer::meshHideText() const
{
    // 与另外几个诊断快照不同：这里没有模型时返回**空串**而不是「无模型」——
    // 界面拿它决定「状态行要不要多一段」，不是拿它当诊断输出。
    return m_d->model ? m_d->model->meshHideText() : QString();
}

void Live2DRenderer::refreshMeshHide()
{
    if (m_d->model) {
        m_d->model->refreshMeshHide();
    }
}

void Live2DRenderer::setGazeStrength(int strength)
{
    const int next = clampGazeStrength(strength);
    if (m_gazeStrength == next) {
        return;
    }
    const bool wasOn = gazeEnabled();
    m_gazeStrength = next;
    const bool nowOn = gazeEnabled();

    if (!m_d->model) {
        return;
    }
    if (wasOn && !nowOn && m_d->gazeSeen) {
        m_d->gazeX = 0.0f;
        m_d->gazeY = 0.0f;
        m_d->model->setDragTarget(0.0f, 0.0f);
    } else if (nowOn && m_d->gazeSeen) {
        pointerMove(m_d->lastPointer);
    }
}

void Live2DRenderer::pointerClick(const QPointF &pos)
{
    if (!m_d->model) {
        return;
    }
    const float w = m_width > 0 ? static_cast<float>(m_width) : 1.0f;
    const float h = m_height > 0 ? static_cast<float>(m_height) : 1.0f;
    m_d->model->startHitReaction(QPointF(pos.x() / w, pos.y() / h));
}

bool Live2DRenderer::playNextMotion()
{
    return m_d->model && m_d->model->playNextMotion();
}

void Live2DRenderer::setMotionLoopEnabled(bool enabled)
{
    KanbanRenderer::setMotionLoopEnabled(enabled);
    if (m_d->model) {
        m_d->model->setMotionLoopEnabled(enabled);
    }
}

void Live2DRenderer::setSoundEnabled(bool enabled)
{
    KanbanRenderer::setSoundEnabled(enabled);
    if (m_d->model) {
        m_d->model->setSoundEnabled(enabled);
    }
}

int Live2DRenderer::playableMotionCount() const
{
    return m_d->model ? m_d->model->playableMotionCount() : 0;
}

int Live2DRenderer::currentMotionOrdinal() const
{
    return m_d->model ? m_d->model->currentMotionOrdinal() : 0;
}

int Live2DRenderer::expressionCount() const
{
    return m_d->model ? m_d->model->expressionCount() : 0;
}

bool Live2DRenderer::playNextExpression()
{
    if (!m_d->model || !m_d->model->playNextExpression()) {
        return false;
    }
    logDebug(QStringLiteral("切换表情 → %1").arg(m_d->model->currentExpressionName()));
    return true;
}

void Live2DRenderer::pause()
{
    m_paused = true;
    // 暂停要连声音一起停：时钟停住后动作不再推进，语音却会自己播完，听着像没暂停成功。
    if (m_d->model) {
        m_d->model->stopMotionSound();
    }
}

void Live2DRenderer::resume()
{
    m_paused = false;
}

void Live2DRenderer::shutdown()
{
    unloadModel();
    m_ready = false;
    m_modelLoaded = false;
    m_modelPath.clear();
}

} // namespace kanban

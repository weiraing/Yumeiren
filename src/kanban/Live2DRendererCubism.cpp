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

// 门面仅管理宿主状态、模型生命周期及坐标转换。
struct Live2DRenderer::Private
{
    std::unique_ptr<CubismModel> model;
    KanbanGlHost *host = nullptr;
    bool glBuilt = false;
    bool glewReady = false;
    QSize pixelSize;
    float gazeX = 0.0f;
    float gazeY = 0.0f;
    // 保留原始坐标，强度切换时按新半径重算目标。
    QPointF lastPointer{-1.0, -1.0};
    bool gazeSeen = false;
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
    // 同一宿主可能重复交付，此时保留已上传的 GL 资源。
    if (host == m_d->host) {
        return;
    }
    if (m_d->model) {
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
        // setup 失败时还没碰过 GL，直接丢弃是安全的。
        return false;
    }

    model->setDeterministicIdle(m_deterministicIdle);
    m_modelPath = modelJsonPath;
    m_modelLoaded = true;
    logInfo(QStringLiteral("已装载 %1：纹理 %2 张 / 动作组 %3 个(可播动作 %4 个) / 表情 %5 个")
                .arg(QFileInfo(modelJsonPath).completeBaseName())
                .arg(model->textureCount())
                .arg(model->motionGroupCount())
                .arg(model->playableMotionCount())
                .arg(model->expressionCount()));

    // 控制器换模型时未必在 paintGL 里，此时没有上下文是常态；
    // 建不起来就交给第一次 render() 补，不把这一步当失败。
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
}

void Live2DRenderer::update(float deltaSeconds)
{
    if (!m_modelLoaded || m_paused || !m_d->model) {
        return;
    }
    m_d->model->update(deltaSeconds);
}

void Live2DRenderer::paint(QPainter *painter, const QSize &logicalSize)
{
    // GPU 路径由 render() 负责，软件路径下本后端不存在。
    Q_UNUSED(painter)
    Q_UNUSED(logicalSize)
}

void Live2DRenderer::render()
{
    CubismModel *model = m_d->model.get();
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

    // 光标转换为 -1..1 方向；扩大作用半径，避免小窗口过早达到幅度上限。
    const float w = m_width > 0 ? static_cast<float>(m_width) : 1.0f;
    const float h = m_height > 0 ? static_cast<float>(m_height) : 1.0f;

    const GazeTuning &tuning = kGazeTuning[clampGazeStrength(m_gazeStrength)];
    // 半径设下限，避免缩小窗口后过于敏感。
    const float radiusX = std::max(w * tuning.radiusFactor, kGazeMinRadiusPx);
    const float radiusY = std::max(h * tuning.radiusFactor, kGazeMinRadiusPx);

    // pos 可以位于窗口外，负坐标同样有效。
    const float dx = static_cast<float>(pos.x()) - w * 0.5f;
    const float dy = static_cast<float>(pos.y()) - h * 0.5f;

    const float nx = qBound(-1.0f, dx / radiusX, 1.0f);
    // Qt 的 y 向下，Cubism 向上；再按档位收敛纵向幅度。
    const float ny = qBound(-1.0f, -dy / radiusY * tuning.verticalScale, 1.0f);

    m_d->gazeX = nx;
    m_d->gazeY = ny;
    m_d->gazeSeen = true;
    // 存下原始坐标：档位切换时要按新半径重算，见 Private::lastPointer 的说明。
    m_d->lastPointer = pos;
    // 这里只写目标方向，平滑过渡由模型更新器完成。
    m_d->model->setDragTarget(nx, ny);
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
        // 关闭时通过同一平滑通道回正。
        m_d->gazeX = 0.0f;
        m_d->gazeY = 0.0f;
        m_d->model->setDragTarget(0.0f, 0.0f);
    } else if (nowOn && m_d->gazeSeen) {
        // gazeSeen 已保证坐标有效；窗口左侧的负坐标也要按新档位重算。
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

int Live2DRenderer::playableMotionCount() const
{
    // 模型没装载时答 0：界面据此把「播放下一个动作」置灰，
    // 而不是让用户点了没反应。
    return m_d->model ? m_d->model->playableMotionCount() : 0;
}

int Live2DRenderer::expressionCount() const
{
    // 模型没装载时答 0：界面据此把入口置灰，而不是让用户点了没反应。
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
}

void Live2DRenderer::resume()
{
    m_paused = false;
}

void Live2DRenderer::shutdown()
{
    // 框架选项和 ID 管理器与进程同寿，关闭后允许再次装载模型。
    unloadModel();
    m_ready = false;
    m_modelLoaded = false;
    m_modelPath.clear();
}

} // namespace kanban

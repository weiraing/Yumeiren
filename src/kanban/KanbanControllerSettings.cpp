// 看板娘配置读写、旧配置迁移与窗口几何设置。
#include "kanban/KanbanController.h"

#include "config/AppConfig.h"
#include "config/ConfigKeys.h"
#include "core/Diagnostics.h"
#include "kanban/KanbanAnimationClock.h"
#include "kanban/KanbanRenderer.h"
#include "kanban/KanbanWindow.h"

namespace kanban {
namespace {
constexpr int kMinEdgePx = 120;
constexpr int kMaxEdgePx = 2400;
constexpr int kBaseWidthPx = 320;
constexpr int kBaseHeightPx = 480;
constexpr int kScaleStepPercent = 5;

QSize windowSizeForScale(int scalePercent)
{
    const double scale = scalePercent / 100.0;
    return QSize(qBound(kMinEdgePx, int(kBaseWidthPx * scale), kMaxEdgePx),
                 qBound(kMinEdgePx, int(kBaseHeightPx * scale), kMaxEdgePx));
}
} // namespace

void KanbanController::loadSettings()
{
    AppConfig &config = AppConfig::instance();
    m_scalePercent = config.value(QString::fromLatin1(ConfigKeys::Kanban::Scale), 100).toInt();
    m_opacityPercent = config.value(QString::fromLatin1(ConfigKeys::Kanban::Opacity), 100).toInt();
    m_targetFps = config.value(QString::fromLatin1(ConfigKeys::Kanban::TargetFps), 30).toInt();
    m_alwaysOnTop = config.value(QString::fromLatin1(ConfigKeys::Kanban::AlwaysOnTop), true).toBool();
    m_mouseThrough = config.value(QString::fromLatin1(ConfigKeys::Kanban::MouseThrough), false).toBool();
    m_interactionEnabled = config.value(QString::fromLatin1(ConfigKeys::Kanban::AllowInteraction), true).toBool();

    // 仅在新键缺失时迁移旧开关，避免覆盖用户在新版选择的档位。
    const QString strengthKey = QString::fromLatin1(ConfigKeys::Kanban::GazeStrength);
    if (config.contains(strengthKey)) {
        m_gazeStrength = KanbanRenderer::clampGazeStrength(config.value(strengthKey).toInt());
    } else {
        const bool legacyOn =
            config.value(QString::fromLatin1(ConfigKeys::Kanban::GazeTrackingLegacy), true).toBool();
        m_gazeStrength = legacyOn ? KanbanRenderer::GazeMedium : KanbanRenderer::GazeOff;
        config.setValue(strengthKey, m_gazeStrength);
        videodiag::log(videodiag::Level::Info,
                       QStringLiteral("[Kanban] 视线追踪配置迁移: 旧布尔=%1 → 档位=%2")
                           .arg(legacyOn ? QStringLiteral("开") : QStringLiteral("关"))
                           .arg(KanbanRenderer::gazeStrengthName(m_gazeStrength)),
                       QLatin1String("Kanban"));
    }

    m_modelPath = config.value(QString::fromLatin1(ConfigKeys::Kanban::ModelPath)).toString();
    m_clock->setTargetFps(m_targetFps);
    emit settingsChanged();
}

bool KanbanController::wasRunningLastTime() const
{
    // 正常退出保留此键，用户停止或启动失败时清除；首次安装默认不启动。
    return AppConfig::instance().value(QString::fromLatin1(ConfigKeys::Kanban::Enabled), false).toBool();
}

void KanbanController::handleScaleStepped(int steps)
{
    setScalePercent(qBound(20, m_scalePercent + steps * kScaleStepPercent, 300));
}

void KanbanController::setScalePercent(int percent)
{
    m_scalePercent = qBound(20, percent, 300);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::Scale), m_scalePercent);
    applyScaleToWindow();
    emit settingsChanged();
}

void KanbanController::placeWindowFromConfig()
{
    if (!m_window) {
        return;
    }
    AppConfig &config = AppConfig::instance();
    // 未保存位置时，由窗口选择主屏右下角。
    const int x = config.value(QString::fromLatin1(ConfigKeys::Kanban::PosX), -1).toInt();
    const int y = config.value(QString::fromLatin1(ConfigKeys::Kanban::PosY), -1).toInt();
    const QSize size = windowSizeForScale(m_scalePercent);
    m_window->placeFromConfig(x, y, size.width(), size.height());
    videodiag::log(videodiag::Level::Debug,
                   QStringLiteral("[Kanban] 窗口首次落位 %1x%2 @(%3,%4)")
                       .arg(m_window->width())
                       .arg(m_window->height())
                       .arg(m_window->x())
                       .arg(m_window->y()),
                   QLatin1String("Kanban"));
}

void KanbanController::applyScaleToWindow()
{
    if (!m_window) {
        return;
    }
    const QSize size = windowSizeForScale(m_scalePercent);
    // 保持底边中心不变，避免角色随缩放向屏幕外漂移。
    const QRect before = m_window->geometry();
    const QPoint bottomCenter(before.x() + before.width() / 2,
                              before.y() + before.height());
    m_window->resize(size);
    m_window->move(bottomCenter.x() - size.width() / 2, bottomCenter.y() - size.height());
    saveGeometry();
}

void KanbanController::setOpacityPercent(int percent)
{
    m_opacityPercent = qBound(20, percent, 100);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::Opacity), m_opacityPercent);
    if (m_window) {
        m_window->setOpacityPercent(m_opacityPercent);
    }
    emit settingsChanged();
}

void KanbanController::setTargetFps(int fps)
{
    m_targetFps = qBound(10, fps, 60);
    m_clock->setTargetFps(m_targetFps);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::TargetFps), m_targetFps);
    emit settingsChanged();
}

void KanbanController::setAlwaysOnTop(bool onTop)
{
    m_alwaysOnTop = onTop;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::AlwaysOnTop), onTop);
    if (m_window) {
        m_window->setAlwaysOnTop(onTop);
    }
}

void KanbanController::setMouseThrough(bool through)
{
    m_mouseThrough = through;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::MouseThrough), through);
    if (m_window) {
        m_window->setMouseThrough(through);
    }
}

void KanbanController::setInteractionEnabled(bool enabled)
{
    m_interactionEnabled = enabled;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::AllowInteraction), enabled);
    if (m_window) {
        m_window->setInteractionEnabled(enabled);
    }
}

void KanbanController::setGazeStrength(int strength)
{
    const int next = KanbanRenderer::clampGazeStrength(strength);
    if (m_gazeStrength == next) {
        return;
    }
    m_gazeStrength = next;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::GazeStrength), next);
    // 同步旧键，保证回退旧版本时仍保留开关状态。
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::GazeTrackingLegacy),
                                   next != KanbanRenderer::GazeOff);
    // 立即重算视线，暂停动画时切档也能更新目标。
    if (m_renderer) {
        m_renderer->setGazeStrength(next);
        if (next != KanbanRenderer::GazeOff) {
            feedGazeTarget();
        }
    }
    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("[Kanban] 视线追踪强度=%1(%2)")
                       .arg(KanbanRenderer::gazeStrengthName(next))
                       .arg(next),
                   QLatin1String("Kanban"));
}

bool KanbanController::setModelPath(const QString &modelJsonPath)
{
    const ModelInfo *info = m_models.byJsonPath(modelJsonPath);
    if (!info || !info->valid) {
        m_lastError = QStringLiteral("模型不可用");
        return false;
    }
    if (!m_renderer) {
        m_modelPath = modelJsonPath;
        AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::ModelPath), m_modelPath);
        return true;
    }
    QString error;
    if (!m_renderer->loadModel(modelJsonPath, &error)) {
        m_lastError = error;
        return false;
    }
    m_modelPath = modelJsonPath;
    // 挂起释放标记跟着装载结果复位：熄屏时会话仍是活跃的，用户在这期间换模型
    // 会把刚释放掉的东西又装回显存。不复位，心跳就因为标记还挂着而再也不释放。
    m_releasedForSuspend = false;
    m_currentModelName = info->name;
    m_lastError.clear();
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::ModelPath), m_modelPath);
    if (m_window) {
        m_window->setModelDisplayName(m_currentModelName);
        m_window->requestFrame();
    }
    emit currentModelChanged(m_currentModelName);
    return true;
}

void KanbanController::saveGeometry()
{
    if (!m_window) {
        return;
    }
    const QRect geometry = m_window->geometry();
    AppConfig &config = AppConfig::instance();
    config.setValue(QString::fromLatin1(ConfigKeys::Kanban::Width), geometry.width());
    config.setValue(QString::fromLatin1(ConfigKeys::Kanban::Height), geometry.height());
    config.setValue(QString::fromLatin1(ConfigKeys::Kanban::PosX), geometry.x());
    config.setValue(QString::fromLatin1(ConfigKeys::Kanban::PosY), geometry.y());
}

} // namespace kanban

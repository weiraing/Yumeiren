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
// 菜单最多透到 80%：再往上条目文字就开始糊，点了什么都看不清了。
// 对应窗口透明度的 80 上限，两个滑杆的"最透一档"是同一种手感。
constexpr int kMaxMenuTransparencyPercent = 80;

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
    // 透明度(0=不透明，80=最透)。旧键存的是「不透明度」，缺新键时反算迁移一次。
    const QString trKey = QString::fromLatin1(ConfigKeys::Kanban::Transparency);
    if (config.contains(trKey)) {
        m_transparencyPercent = qBound(0, config.value(trKey).toInt(), 80);
    } else if (config.contains(QString::fromLatin1(ConfigKeys::Kanban::OpacityLegacy))) {
        m_transparencyPercent = qBound(0, 100 - config.value(
            QString::fromLatin1(ConfigKeys::Kanban::OpacityLegacy)).toInt(), 80);
        config.setValue(trKey, m_transparencyPercent);
    } else {
        m_transparencyPercent = 0;
        config.setValue(trKey, 0);
    }
    m_menuBg = QColor(config.value(QString::fromLatin1(ConfigKeys::Kanban::MenuBgColor)).toString());

    // 菜单透明度(0=不透明，80=最透)。旧键存的是「不透明度」，缺新键时反算迁移一次 ——
    // 与上面窗口透明度同一个处理，用户看不出换过键。
    const QString menuTrKey = QString::fromLatin1(ConfigKeys::Kanban::MenuTransparency);
    if (config.contains(menuTrKey)) {
        m_menuTransparency =
            qBound(0, config.value(menuTrKey).toInt(), kMaxMenuTransparencyPercent);
    } else if (config.contains(QString::fromLatin1(ConfigKeys::Kanban::MenuOpacityLegacy))) {
        m_menuTransparency = qBound(0, 100 - config.value(
            QString::fromLatin1(ConfigKeys::Kanban::MenuOpacityLegacy)).toInt(),
            kMaxMenuTransparencyPercent);
        config.setValue(menuTrKey, m_menuTransparency);
    } else {
        m_menuTransparency = 0;
        config.setValue(menuTrKey, 0);
    }
    m_menuGlass =
        qBound(0, config.value(QString::fromLatin1(ConfigKeys::Kanban::MenuGlass), 0).toInt(), 100);
    m_targetFps = config.value(QString::fromLatin1(ConfigKeys::Kanban::TargetFps), 30).toInt();
    m_alwaysOnTop = config.value(QString::fromLatin1(ConfigKeys::Kanban::AlwaysOnTop), true).toBool();
    m_mouseThrough = config.value(QString::fromLatin1(ConfigKeys::Kanban::MouseThrough), false).toBool();
    m_interactionEnabled = config.value(QString::fromLatin1(ConfigKeys::Kanban::AllowInteraction), true).toBool();
    m_motionLoopEnabled =
        config.value(QString::fromLatin1(ConfigKeys::Kanban::MotionLoop), true).toBool();
    m_soundEnabled = config.value(QString::fromLatin1(ConfigKeys::Kanban::PlaySound), true).toBool();
    m_doubleClickSwitchEnabled =
        config.value(QString::fromLatin1(ConfigKeys::Kanban::DoubleClickSwitch), true).toBool();

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
    // 纹理上限策略必须在任何模型装载之前定下来，所以跟着配置一起在这里落。
    setTextureDownscaleEnabled(
        config.value(QString::fromLatin1(ConfigKeys::Kanban::TextureDownscale), true).toBool());
    // 网格隐藏清单同理，也是「装载前就得定下来」的进程级策略。默认**开**：用户把清单丢进
    // 模型目录就是想让它生效，不该再让他去找一遍开关。没有清单的模型完全不受影响。
    kanban::setMeshHideEnabled(
        config.value(QString::fromLatin1(ConfigKeys::Kanban::MeshHide), true).toBool());
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
    // 未保存位置时(配置为 -1)，由窗口选择主屏右下角。
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

void KanbanController::setTransparencyPercent(int percent)
{
    const int v = qBound(0, percent, 80);
    if (m_transparencyPercent == v) {
        return;
    }
    m_transparencyPercent = v;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::Transparency), v);
    if (m_window) {
        m_window->setTransparencyPercent(v);
    }
    emit settingsChanged();
}

// —— 右键菜单外观：三项任一变动都整体推给窗口并回写设置页 ——

void KanbanController::setMenuBgColor(const QColor &color)
{
    const QColor c = color.isValid() ? color : QColor();
    if (m_menuBg == c) {
        return;
    }
    m_menuBg = c;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::MenuBgColor),
                                   c.isValid() ? c.name(QColor::HexArgb) : QString());
    if (m_window) {
        m_window->setMenuAppearance(m_menuBg, m_menuTransparency, m_menuGlass);
    }
    emit settingsChanged();
}

void KanbanController::setMenuTransparency(int percent)
{
    const int v = qBound(0, percent, kMaxMenuTransparencyPercent);
    if (m_menuTransparency == v) {
        return;
    }
    m_menuTransparency = v;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::MenuTransparency), v);
    if (m_window) {
        m_window->setMenuAppearance(m_menuBg, m_menuTransparency, m_menuGlass);
    }
    emit settingsChanged();
}

void KanbanController::setMenuGlass(int level)
{
    const int v = qBound(0, level, 100);
    if (m_menuGlass == v) {
        return;
    }
    m_menuGlass = v;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::MenuGlass), m_menuGlass);
    if (m_window) {
        m_window->setMenuAppearance(m_menuBg, m_menuTransparency, m_menuGlass);
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
    emit settingsChanged();
}

void KanbanController::setMouseThrough(bool through)
{
    m_mouseThrough = through;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::MouseThrough), through);
    if (m_window) {
        m_window->setMouseThrough(through);
    }
    emit settingsChanged();
}

void KanbanController::setInteractionEnabled(bool enabled)
{
    m_interactionEnabled = enabled;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::AllowInteraction), enabled);
    if (m_window) {
        m_window->setInteractionEnabled(enabled);
    }
    emit settingsChanged();
}

void KanbanController::setMotionLoopEnabled(bool enabled)
{
    if (m_motionLoopEnabled == enabled) {
        return;
    }
    m_motionLoopEnabled = enabled;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::MotionLoop), enabled);
    if (m_renderer) {
        m_renderer->setMotionLoopEnabled(enabled);
    }
    emit settingsChanged();
}

void KanbanController::setSoundEnabled(bool enabled)
{
    if (m_soundEnabled == enabled) {
        return;
    }
    m_soundEnabled = enabled;
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::PlaySound), enabled);
    if (m_renderer) {
        m_renderer->setSoundEnabled(enabled);
    }
    emit settingsChanged();
}

void KanbanController::setDoubleClickSwitchEnabled(bool enabled)
{
    if (m_doubleClickSwitchEnabled == enabled) {
        return;
    }
    m_doubleClickSwitchEnabled = enabled;
    AppConfig::instance().setValue(
        QString::fromLatin1(ConfigKeys::Kanban::DoubleClickSwitch), enabled);
    emit settingsChanged();
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

bool KanbanController::meshHideEnabled() const
{
    // 单一来源就是那个进程级标志(与 textureDownscale 同一套)，控制器不再存一份镜像。
    return kanban::meshHideEnabled();
}

void KanbanController::setMeshHideEnabled(bool enabled)
{
    if (kanban::meshHideEnabled() == enabled) {
        return;
    }
    kanban::setMeshHideEnabled(enabled);
    AppConfig::instance().setValue(QString::fromLatin1(ConfigKeys::Kanban::MeshHide), enabled);
    // 暂停时动画时钟不走、update() 不会被调用 —— 不主动刷一次的话，取消勾选要等恢复动画
    // 才生效，看起来像开关失灵。刷新完再请求一帧，让窗口真重画。
    if (m_renderer) {
        m_renderer->refreshMeshHide();
    }
    if (m_window) {
        m_window->requestFrame();
    }
    emit settingsChanged();
}

QString KanbanController::meshHideText() const
{
    return m_renderer ? m_renderer->meshHideText() : QString();
}bool KanbanController::setModelPath(const QString &modelJsonPath)
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
    // 挂起释放标记跟着装载结果复位：熄屏时会话仍活跃，用户若在这期间换模型会把
    // 刚释放的东西又装回显存；不复位的话心跳会因标记还挂着而再也不释放。
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

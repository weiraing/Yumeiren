#include "AppConfig.h"

#include "ConfigKeys.h"
#include "core/Diagnostics.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

namespace {

constexpr int kCurrentConfigVersion = 1;
constexpr int kSaveDebounceMs = 500;

struct NumericRule {
    const char *key;
    QVariant defaultValue;
    int lo;
    int hi;
};

// 数值类配置的默认值与合法区间：缺失补默认，越界收敛到边界
const NumericRule kNumericRules[] = {
    {ConfigKeys::Ui::Theme, 0, 0, 2},
    {ConfigKeys::Image::Rotate, 0, -180, 180},
    {ConfigKeys::Image::Scale, 100, 10, 150},
    {ConfigKeys::Image::Brightness, 100, 20, 200},
    {ConfigKeys::Image::Contrast, 100, 50, 150},
    {ConfigKeys::Image::Blur, 0, 0, 20},
    {ConfigKeys::Image::Opacity, 255, 30, 255},
    {ConfigKeys::Image::PosType, 6, 0, 6},
    {ConfigKeys::Image::Mode, 0, 0, 1},
    {ConfigKeys::Effect::Type, 1, 0, 4},
    {ConfigKeys::Effect::LightAlpha, 200, 0, 255},
    {ConfigKeys::Effect::DarkAlpha, 120, 0, 255},
    {ConfigKeys::Video::Volume, 0, 0, 100},
    {ConfigKeys::Video::TargetFps, 24, 0, 240},
    {ConfigKeys::Video::ScreenMode, 0, 0, 2},
    {ConfigKeys::Video::PlayMode, 0, 0, 2},
    // 看板娘位置 -1 表示「尚未放置」(首次显示时按主屏右下角自动摆放)，下界必须留 -1，
    // 否则钳位会把「未放置」改写成 0，等于把窗口钉死在屏幕左上角。
    {ConfigKeys::Kanban::Scale, 100, 20, 300},
    {ConfigKeys::Kanban::Transparency, 0, 0, 80},
    {ConfigKeys::Kanban::Width, 320, 160, 2560},
    {ConfigKeys::Kanban::Height, 480, 160, 2560},
    {ConfigKeys::Kanban::PosX, -1, -1, 8192},
    {ConfigKeys::Kanban::PosY, -1, -1, 8192},
    {ConfigKeys::Kanban::TargetFps, 30, 10, 60},
    {ConfigKeys::Web::RefreshMode, 0, 0, 2},
    {ConfigKeys::Web::Volume, 0, 0, 100},
    {ConfigKeys::Web::Zoom, 100, 50, 200},
    {ConfigKeys::Web::FpsCap, 24, 0, 60},
};

const char *kBoolRules[] = {
    ConfigKeys::Image::FolderExt,
    ConfigKeys::Effect::ClearAddress,
    ConfigKeys::Effect::ClearBarBg,
    ConfigKeys::Effect::ClearWinUIBg,
    ConfigKeys::Effect::ShowLine,
    ConfigKeys::Video::WasPlaying,
    ConfigKeys::Video::PauseFullscreen,
    ConfigKeys::Video::PauseBattery,
    ConfigKeys::Video::FpsKeepSpeed,
    ConfigKeys::Video::Reclaim,
    ConfigKeys::Video::AffinityLimit,
    ConfigKeys::Video::Diag,
    ConfigKeys::Window::Maximized,
    ConfigKeys::Kanban::Enabled,
    ConfigKeys::Kanban::AlwaysOnTop,
    ConfigKeys::Kanban::MouseThrough,
    ConfigKeys::Kanban::AllowInteraction,
    ConfigKeys::Kanban::MotionLoop,
    ConfigKeys::Kanban::PlaySound,
    ConfigKeys::Kanban::DoubleClickSwitch,
    ConfigKeys::Kanban::TextureDownscale,
    ConfigKeys::Kanban::MeshHide,
    ConfigKeys::Tray::Enabled,
    ConfigKeys::Tray::MinimizeToTrayOnClose,
    ConfigKeys::Web::Enabled,
    ConfigKeys::Web::Interactive,
};

// 布尔项缺省值表：列在这里的默认开，其余默认关。用表是为了新增键时只看一处。
const char *kBoolDefaultTrue[] = {
    ConfigKeys::Effect::ClearAddress,
    ConfigKeys::Effect::ClearBarBg,
    ConfigKeys::Effect::ClearWinUIBg,
    ConfigKeys::Video::PauseFullscreen,
    ConfigKeys::Video::FpsKeepSpeed,
    ConfigKeys::Video::Reclaim,
    ConfigKeys::Video::AffinityLimit,
    ConfigKeys::Kanban::AlwaysOnTop,
    ConfigKeys::Kanban::AllowInteraction,
    ConfigKeys::Kanban::MotionLoop,
    // 语音默认开：这是本项目的既有行为，加这个键只是为了给用户一个关掉的开关，
    // 升级后不该突然变安静。
    ConfigKeys::Kanban::PlaySound,
    ConfigKeys::Kanban::DoubleClickSwitch,
    ConfigKeys::Kanban::TextureDownscale,
    // 网格隐藏默认开：用户把清单丢进模型目录就是想让它生效，不该再让他找一遍开关。
    // 没有清单的模型完全不受影响，所以默认开是安全的。
    ConfigKeys::Kanban::MeshHide,
    ConfigKeys::Tray::Enabled,
    // 有后台任务时点关闭应「隐藏而不是退出」，故默认开；读取端(closeEvent /
    // 界面勾选框)默认值同为 true。
    ConfigKeys::Tray::MinimizeToTrayOnClose,
    // 网页壁纸默认允许交互：选网页壁纸多半就是冲着可交互内容来的，默认关会让
    // 用户以为功能坏了。
    ConfigKeys::Web::Interactive,
};

bool boolDefaultFor(const char *key)
{
    for (const char *k : kBoolDefaultTrue) {
        if (strcmp(k, key) == 0)
            return true;
    }
    return false;
}

// INI 中一切值都是字符串: "true"/"1" 视为真, "false"/"0" 视为假
bool normalizeBool(const QVariant &v)
{
    const QString s = v.toString().trimmed().toLower();
    return s == "true" || s == "1";
}

bool isValidBool(const QVariant &v)
{
    if (v.type() == QVariant::Bool)
        return true;
    const QString s = v.toString().trimmed().toLower();
    return s == "true" || s == "false" || s == "1" || s == "0";
}

bool isValidColor(const QVariant &v)
{
    static const QRegularExpression re(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return re.match(v.toString()).hasMatch();
}

} // namespace

AppConfig &AppConfig::instance()
{
    static AppConfig c;
    return c;
}

AppConfig::AppConfig(QObject *parent)
    : QObject(parent)
{
    const QString path = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("config/.ini"));
    m_settings = new QSettings(path, QSettings::IniFormat, this);
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kSaveDebounceMs);
    connect(m_saveTimer, &QTimer::timeout, this, &AppConfig::save);
}

AppConfig::~AppConfig()
{
    if (m_loaded)
        save();
}

QString AppConfig::configFilePath() const
{
    return m_settings->fileName();
}

QString AppConfig::configDirectory() const
{
    return QFileInfo(configFilePath()).absolutePath();
}

bool AppConfig::load()
{
    if (m_loaded)
        return true;
    m_loaded = true;

    // 读取诊断: 文件是否存在/大小/键数(排查"读不到配置"类问题)
    {
        QFileInfo fi(configFilePath());
        videodiag::log(videodiag::Level::Info,
            QStringLiteral("配置读取: path=%1 exists=%2 size=%3 keys=%4 wasPlaying=%5")
                .arg(fi.absoluteFilePath()).arg(fi.exists())
                .arg(fi.size()).arg(m_settings->allKeys().size())
                .arg(m_settings->value(ConfigKeys::Video::WasPlaying, -1).toString()),
            QStringLiteral("Config"));
    }

    const QString dir = configDirectory();
    if (!QDir().mkpath(dir)) {
        videodiag::log(videodiag::Level::Error,
            QStringLiteral("配置目录创建失败: %1 (将以默认配置运行)").arg(dir),
            QStringLiteral("Config"));
    }

    ensureDefaultsAndFix();
    if (!m_settings->contains(ConfigKeys::Meta::ConfigVersion))
        m_settings->setValue(ConfigKeys::Meta::ConfigVersion, kCurrentConfigVersion);

    save();
    videodiag::log(videodiag::Level::Info,
        QStringLiteral("配置已加载: %1").arg(configFilePath()),
        QStringLiteral("Config"));
    return true;
}

void AppConfig::ensureDefaultsAndFix()
{
    int missing = 0;
    int fixed = 0;

    // 播放模式迁移(必须先于默认值补齐)：旧的"列表循环/随机"两个开关合并成三选一的
    // video/playMode，其余(含全新配置)落到单循环。旧键留在文件里不删，回滚旧版仍可读。
    if (!m_settings->contains(ConfigKeys::Video::PlayMode)) {
        int mode = 0;
        if (normalizeBool(m_settings->value(ConfigKeys::Video::RandomLegacy, false)))
            mode = 2;
        else if (normalizeBool(m_settings->value(ConfigKeys::Video::AutoLoopLegacy, true)))
            mode = 1;
        m_settings->setValue(ConfigKeys::Video::PlayMode, mode);
        ++missing;
    }

    for (const NumericRule &rule : kNumericRules) {
        if (!m_settings->contains(rule.key)) {
            m_settings->setValue(rule.key, rule.defaultValue);
            ++missing;
            continue;
        }
        bool ok = false;
        const int n = m_settings->value(rule.key).toInt(&ok);
        if (!ok) {
            m_settings->setValue(rule.key, rule.defaultValue);
            ++fixed;
        } else if (n < rule.lo || n > rule.hi) {
            m_settings->setValue(rule.key, qBound(rule.lo, n, rule.hi));
            ++fixed;
        }
    }
    for (const char *key : kBoolRules) {
        if (!m_settings->contains(key)) {
            m_settings->setValue(key, boolDefaultFor(key));
            ++missing;
        } else {
            // 只有存储值确实不是合法布尔写法时才改写并计数：INI 里一切皆字符串，
            // 无脑 setValue 会把本来合法的布尔项每启动标脏一次，日志上表现为
            // "修复非法 N 项"，掩盖真实修复。
            const QVariant raw = m_settings->value(key);
            if (!isValidBool(raw)) {
                m_settings->setValue(key, normalizeBool(raw));
                ++fixed;
            }
        }
    }
    const struct { const char *key; const char *def; } colors[] = {
        {ConfigKeys::Effect::LightColor, "#ffffff"},
        {ConfigKeys::Effect::DarkColor, "#000000"},
    };
    for (const auto &c : colors) {
        if (!m_settings->contains(c.key)) {
            m_settings->setValue(c.key, QString::fromLatin1(c.def));
            ++missing;
        } else if (!isValidColor(m_settings->value(c.key))) {
            m_settings->setValue(c.key, QString::fromLatin1(c.def));
            ++fixed;
        }
    }
    if (!m_settings->contains(ConfigKeys::Video::Playlist)) {
        m_settings->setValue(ConfigKeys::Video::Playlist, QStringList());
        ++missing;
    }
    const struct { const char *key; const char *def; } strings[] = {
        {ConfigKeys::Image::CustomPath, ""},
        {ConfigKeys::Effect::ShowLine, "false"},
        {ConfigKeys::Kanban::ModelPath, ""},
    };
    for (const auto &s : strings) {
        if (!m_settings->contains(s.key)) {
            m_settings->setValue(s.key, QString::fromLatin1(s.def));
            ++missing;
        }
    }

    if (missing + fixed > 0)
        videodiag::log(videodiag::Level::Info,
            QStringLiteral("配置校验: 补齐缺失 %1 项, 修复非法 %2 项").arg(missing).arg(fixed),
            QStringLiteral("Config"));
}

bool AppConfig::save()
{
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        videodiag::log(videodiag::Level::Error,
            QStringLiteral("配置保存失败: %1 status=%2")
                .arg(configFilePath()).arg(int(m_settings->status())),
            QStringLiteral("Config"));
        return false;
    }
    return true;
}

void AppConfig::reload()
{
    m_settings->sync();
    m_settings->sync();
    m_settings->sync();
    // QSettings 无显式 reread：sync 把外部改动拉回来，之后读取即为最新内容
    ensureDefaultsAndFix();
}

QVariant AppConfig::value(const QString &key, const QVariant &defaultValue) const
{
    return m_settings->value(key, defaultValue);
}

void AppConfig::setValue(const QString &key, const QVariant &value)
{
    if (m_settings->value(key) == value)
        return;
    m_settings->setValue(key, value);
    emit settingChanged(key, value);
    scheduleSave();
}

bool AppConfig::contains(const QString &key) const
{
    return m_settings->contains(key);
}

QStringList AppConfig::allKeys() const
{
    return m_settings->allKeys();
}

void AppConfig::remove(const QString &key)
{
    m_settings->remove(key);
    scheduleSave();
}

void AppConfig::scheduleSave()
{
    if (m_saveTimer && !m_saveTimer->isActive())
        m_saveTimer->start();
}

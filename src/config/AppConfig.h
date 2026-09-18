#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QObject>
#include <QSettings>
#include <QVariant>

class QTimer;

// 统一配置中心(全项目唯一的设置存取入口)。
// 存储位置: <软件运行目录>/config/.ini (INI 格式；config 是目录，.ini 是文件名)。
// 启动流程: 创建目录 → 若无文件则写入默认值 → 校验修复。
// 写入策略: setValue 更新内存并启动 500ms 单次延迟保存；退出时统一保存。
class AppConfig : public QObject
{
    Q_OBJECT

public:
    static AppConfig &instance();

    bool load();                       // 幂等：创建目录/文件、校验修复
    bool save();                       // sync + 状态检查; 失败记日志不清空配置
    void reload();

    QString configFilePath() const;
    QString configDirectory() const;

    QVariant value(const QString &key, const QVariant &defaultValue = {}) const;
    void setValue(const QString &key, const QVariant &value);
    bool contains(const QString &key) const;
    QStringList allKeys() const;
    void remove(const QString &key);

signals:
    void settingChanged(const QString &key, const QVariant &value);

private:
    explicit AppConfig(QObject *parent = nullptr);
    ~AppConfig() override;
    Q_DISABLE_COPY(AppConfig)

    void ensureDefaultsAndFix();       // 补齐缺失 + 修复非法值
    void scheduleSave();               // 高频写入的延迟保存(500ms 单次)

    QSettings *m_settings = nullptr;
    QTimer *m_saveTimer = nullptr;
    bool m_loaded = false;
};

#endif // APPCONFIG_H

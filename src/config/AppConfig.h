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
//
// ⚠️ 退出路径必须显式调 flush()，不要依赖析构：单例是函数内静态对象，析构排在
// QApplication 与全局日志对象之后(atexit 顺序)，那时 sync() 失败连日志都写不出去。
// 正常退出由 ApplicationShutdown 收口调用；析构里那次只是最后的兜底。
class AppConfig : public QObject
{
    Q_OBJECT

public:
    static AppConfig &instance();

    bool load();                       // 幂等：创建目录/文件、校验修复
    bool save();                       // sync + 状态检查; 失败记日志不清空配置

    // 立即落盘：停掉待触发的防抖定时器并同步写盘。幂等，可重复调用。
    // 用于「退出前」与「改完必须立刻生效」的关键路径 —— 500ms 防抖窗口内进程被
    // 强杀/崩溃时，窗口内的改动会静默丢失。
    bool flush();

    QString configFilePath() const;
    QString configDirectory() const;

    QVariant value(const QString &key, const QVariant &defaultValue = {}) const;
    void setValue(const QString &key, const QVariant &value);
    bool contains(const QString &key) const;
    QStringList allKeys() const;
    void remove(const QString &key);

signals:

private:
    explicit AppConfig(QObject *parent = nullptr);
    ~AppConfig() override;
    Q_DISABLE_COPY(AppConfig)

    void ensureDefaultsAndFix();       // 补齐缺失 + 修复非法值
    void scheduleSave();               // 高频写入的延迟保存(500ms 单次)

    QSettings *m_settings = nullptr;
    QTimer *m_saveTimer = nullptr;
    bool m_loaded = false;
    // 是否已经通过 flush() 正经落过盘。析构只看它决定要不要兜底再存一次：
    // 存过就不必在静态析构期碰 QSettings / 全局日志。
    bool m_flushDone = false;
};

#endif // APPCONFIG_H

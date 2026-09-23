#pragma once

#include <QFrame>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// 媒体库卡片：「视频壁纸」与「动态网页壁纸」右侧的统一结构 ——
//   标题 → 工具栏(扫描 / 打开目录 / ✕删除 + 帮助) → 双列树形列表 → 一行信息。
// 卡片只负责结构、勾选态与删除键的联动、打开目录；行填充与删除逻辑由各页通过
// addRow / checkedPaths + scanRequested / deleteCheckedRequested 信号接入 ——
// 两个库的"删除"语义相同(把勾选行的文件移入回收站)，但各自的落点不同。
// 继承 QFrame：PageCard 的背景/圆角边框样式只有 QFrame 才会绘制(纯 QWidget
// 子类不带 WA_StyledBackground 会忽略样式表背景，右侧就"没有框")。
class MediaLibraryCard : public QFrame
{
    Q_OBJECT
public:
    // 行上存绝对路径的角色；页面可再往其它角色里挂自己的扩展数据
    static constexpr int PathRole = Qt::UserRole + 2;

    // helpText 是右上角 ? 徽标的 tooltip。noteText 会**追加在它后面**：
    // 2026-09-23 之前 noteText 是卡片底部常驻的第二行(没有 objectName，用的是默认
    // 深色正文色)，现在底部只留一行三段式信息，常驻说明一律并进 tooltip。
    // ⚠️ 所以 noteText 里**别再重复 helpText 已写过的内容** —— 用户会看到两遍。
    explicit MediaLibraryCard(const QString &title, const QString &nameHeader,
                              const QString &dir, const QString &helpText,
                              const QString &noteText, QFrame *parent = nullptr);

    QTreeWidget *tree() const { return m_tree; }

    // 卡片底部**唯一**那一行信息，三段固定顺序：统计信息 · 状态 · 播放数据名。
    // 两个库都走这个入口，两页看起来才一模一样；空段自动跳过
    // (「正在刷新…」这类过渡态只填第一段)。
    void setInfo(const QString &stats, const QString &state, const QString &name);

    // 追加一行：type 为第二列文本，path 存进 PathRole（checkedPaths 靠它取路径；
    // 刻意不挂成悬停提示，见 .cpp 里的说明）
    QTreeWidgetItem *addRow(QTreeWidgetItem *parent, const QString &name,
                            const QString &type, const QString &path,
                            bool checkable, bool bold = false);

    // 所有勾选行的 PathRole 路径
    QStringList checkedPaths() const;

    // 运行时不可用等场景：锁定列表与扫描入口
    void setLibraryEnabled(bool on);

signals:
    void scanRequested();
    void deleteCheckedRequested();

private:
    QString m_dir;                 // 「打开目录」的目标
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_scanBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QLabel *m_info = nullptr;      // 底部唯一那一行：统计信息 · 状态 · 播放数据名
};

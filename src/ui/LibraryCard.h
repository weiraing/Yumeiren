#pragma once

#include <QFrame>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// 媒体库卡片：「视频壁纸」与「动态网页壁纸」右侧的统一结构 ——
//   标题 → 工具栏(扫描 / 打开目录 / ✕删除 + 帮助) → 双列树形列表 → 状态行 → 注释。
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

    explicit MediaLibraryCard(const QString &title, const QString &nameHeader,
                              const QString &dir, const QString &helpText,
                              const QString &noteText, QFrame *parent = nullptr);

    QTreeWidget *tree() const { return m_tree; }
    QLabel *statusLabel() const { return m_status; }
    void setStatus(const QString &text);

    // 追加一行：type 为第二列文本，path 存 PathRole 并作为悬停提示
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
    QLabel *m_status = nullptr;
};

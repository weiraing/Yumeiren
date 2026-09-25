// 看板娘模型目录扫描与校验。
//
// 只负责「发现 + 校验 + 元数据」，不碰渲染/窗口/文件复制。校验口径来自 .model3.json
// 的 FileReferences，路径一律相对该 json 所在目录解析。
// 硬要求：坏模型绝不能拖垮软件 —— 扫描阶段就标成 valid=false 并剔除，只在日志留痕。
#ifndef KANBANMODELMANAGER_H
#define KANBANMODELMANAGER_H

#include <QHash>
#include <QStringList>
#include <QVector>

namespace kanban {

struct ModelInfo {
    QString id;              // 稳定标识：模型目录名
    QString name;            // 展示名
    QString thumbKey;        // 预览图缓存键：模型目录相对模型根目录的路径，分隔符换成
                             // '#'，如 data/models/分类/分类/模型文件夹名 →
                             // 分类#分类#模型文件夹名。空串 = 算不出安全键，不出预览图。
    QString rootDir;         // 模型所在目录(绝对)
    QString modelJsonPath;   // *.model3.json(绝对)
    QString moc3Path;        // 解析出的 .moc3(绝对)
    QStringList texturePaths; // 解析出的贴图(绝对)。与 moc3Path 一起构成「这是哪个模型」
                             // 的身份 —— 同一个 moc3 换一套贴图是另一个模型(换装变体)。
    int textureCount = 0;
    int motionCount = 0;     // motion 文件数(含 idle)
    int expressionCount = 0;
    bool valid = false;      // 可装载
    QStringList problems;    // **致命**：缺 .moc3 / 缺贴图 / json 读不了。只有它非空才 valid=false
    QStringList warnings;    // 能装载但缺东西或被程序补过：物理/姿势文件缺失、贴图靠推断…
                             // 与 problems 分开是因为**判据必须与装载器逐条对齐**：装载器
                             // (CubismModelImpl::setup) 对物理、姿势、单个动作/表情都是
                             // 「读不到就跳过」，只有 moc3 与贴图是硬要求。把这些也当致命，
                             // 会把一批本来能用的模型挡在墙外（本机实测 109 个只缺一个
                             // 指不到实处的 Physics 字段）。
};

class KanbanModelManager
{
public:
    // 固定模型根目录 <程序目录>/data/models。刻意不放进 .cache：模型是用户资产，
    // 清缓存不该把它们带走。
    static QString defaultModelsRoot();

    // 仅扫描固定模型目录，返回有效模型数。「没有模型」不算错误，只记日志；其它问题
    // 逐个记在 ModelInfo.problems 里。
    //
    // ⚠️ 幂等：扫过一次之后直接返回上次的结果，不再重扫磁盘。启动路径有**两条**独立
    // 的触发点（setupKanbanAndTray 为了状态栏要个准确计数、KanbanController::start
    // 在列表为空时补扫），它们先后脚发生，各扫一遍就是白遍历整个模型树 —— 实测本机
    // 1392 个 json 会因此跑两轮（日志里每次启动两条「模型扫描完成」）。用户主动点
    // 「刷新」要用真重扫，传 force=true。
    int rescan(bool force = false);

    // 上次扫描是否已经跑过（用于判断 rescan() 会不会真的碰磁盘）。
    bool hasScanned() const { return m_scanned; }

    const QVector<ModelInfo> &models() const { return m_models; }
    // 只含 valid==true 的模型，界面与「下一个模型」都在这个集合上操作。
    QVector<const ModelInfo *> validModels() const;

    const ModelInfo *byJsonPath(const QString &path) const;
    int indexByJsonPath(const QString &path) const;
    // 环形取下一个有效模型；不足两个时返回 nullptr(调用方保持当前模型)。
    const ModelInfo *nextValidAfter(const QString &currentJsonPath) const;

    static int successorIndexAfterRemoval(int removedIndex, int remainingCount);

    // 单个 json 的校验(界面「刷新」与扫描共用)；填好 out 的 valid/problems。
    static bool validateModelJson(const QString &jsonPath, ModelInfo *out);

private:
    QVector<ModelInfo> m_models;
    // 是否已经真扫过盘。rescan() 用它把「启动期那两次先后脚的调用」合并成一次遍历。
    bool m_scanned = false;
    // 被同组「完整描述」取代的精简描述 json 路径 → 取代它的那份 json 路径(都是规范化绝对
    // 路径)。查表只在精确匹配失败时兜底 —— 配置里存的可能正是那份精简描述的路径(实测用户
    // 配置就是)，不兜底会走到「找不到当前模型 → 回退到第一个模型」，用户的选中项被悄悄换掉。
    QHash<QString, QString> m_shadowAlias;
};

} // namespace kanban

#endif // KANBANMODELMANAGER_H

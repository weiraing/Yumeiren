// 看板娘模型目录扫描与校验(任务书 §4.1 KanbanModelManager)。
//
// 只负责「发现 + 校验 + 元数据」，不碰渲染、不碰窗口、不复制文件。
// 校验口径来自 Cubism 的 .model3.json：FileReferences.Moc / Textures[] /
// Motions / Expressions / Physics，路径一律相对该 json 所在目录解析。
//
// 硬要求：一个坏模型绝不能拖垮软件 —— 扫描阶段就把缺文件的模型标成
// valid=false 并从可装载列表里剔除，只在日志里留痕。
#ifndef KANBANMODELMANAGER_H
#define KANBANMODELMANAGER_H

#include <QVector>

namespace kanban {

struct ModelInfo {
    QString id;              // 稳定标识：模型目录名(相对模型根目录)
    QString name;            // 展示名
    QString rootDir;         // 模型所在目录(绝对)
    QString modelJsonPath;   // *.model3.json(绝对)
    QString moc3Path;        // 解析出的 .moc3(绝对)
    int textureCount = 0;
    int motionCount = 0;     // motion 组数
    int expressionCount = 0;
    bool valid = false;      // 可装载
    QStringList problems;    // 不可装载的原因(缺哪个文件/字段)
};

class KanbanModelManager
{
public:
    // 固定模型根目录：<程序目录>/data/models，不受工作目录影响。
    // 刻意不放进 .cache：模型是用户资产，清缓存不该把它们带走。
    static QString defaultModelsRoot();

    // 仅扫描固定模型目录，返回有效模型数。
    // 「没有模型」不算错误，只记日志；扫描中途的问题逐个记在 ModelInfo.problems 里。
    int rescan();

    const QVector<ModelInfo> &models() const { return m_models; }
    // 只含 valid==true 的模型，界面与「下一个模型」都在这个集合上操作。
    QVector<const ModelInfo *> validModels() const;

    const ModelInfo *byJsonPath(const QString &path) const;
    int indexByJsonPath(const QString &path) const;
    // 环形取下一个有效模型；不足两个时返回 nullptr(调用方保持当前模型)。
    const ModelInfo *nextValidAfter(const QString &currentJsonPath) const;

    // 删掉下标 removedIndex 的模型之后，应当切到**剩下的**第几个(0 基)。
    //
    // 不能拿 nextValidAfter() 顶替：那个函数假定当前模型仍在列表里，删完再按路径去
    // 找会找不到，而它「找不到就从 0 开始」的口径会给出第 2 个，不是第 1 个。
    // 正确口径是：删掉第 i 个之后原来的第 i+1 个补到第 i 位，所以「下一个」在新列表
    // 里仍然在下标 i；它本来就是最后一个(或下标无效)时绕回第一个。
    //
    // remainingCount = 删除**之后**剩下的数量。剩下 0 个时返回 0(调用方自行判空)。
    static int successorIndexAfterRemoval(int removedIndex, int remainingCount);

    // 单个 json 的校验(界面「刷新」与扫描共用)；填好 out 的 valid/problems。
    static bool validateModelJson(const QString &jsonPath, ModelInfo *out);

private:
    QVector<ModelInfo> m_models;
};

} // namespace kanban

#endif // KANBANMODELMANAGER_H

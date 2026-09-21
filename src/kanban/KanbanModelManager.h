// 看板娘模型目录扫描与校验。
//
// 只负责「发现 + 校验 + 元数据」，不碰渲染/窗口/文件复制。校验口径来自 .model3.json
// 的 FileReferences，路径一律相对该 json 所在目录解析。
// 硬要求：坏模型绝不能拖垮软件 —— 扫描阶段就标成 valid=false 并剔除，只在日志留痕。
#ifndef KANBANMODELMANAGER_H
#define KANBANMODELMANAGER_H

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
    int textureCount = 0;
    int motionCount = 0;     // motion 文件数(含 idle)
    int expressionCount = 0;
    bool valid = false;      // 可装载
    QStringList problems;    // 不可装载的原因(缺哪个文件/字段)
};

class KanbanModelManager
{
public:
    // 固定模型根目录 <程序目录>/data/models。刻意不放进 .cache：模型是用户资产，
    // 清缓存不该把它们带走。
    static QString defaultModelsRoot();

    // 仅扫描固定模型目录，返回有效模型数。「没有模型」不算错误，只记日志；其它问题
    // 逐个记在 ModelInfo.problems 里。
    int rescan();

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
};

} // namespace kanban

#endif // KANBANMODELMANAGER_H

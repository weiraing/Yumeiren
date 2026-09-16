#include "kanban/KanbanModelManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "videodiag.h"

namespace kanban {

namespace {

constexpr int kMaxDepth = 4; // 模型目录最多向下找 4 层，避免误指向盘根时全树遍历

// 分隔符统一为平台本地形式。等价于 QDir::fromSeparators()，但那个 API 要 Qt 6.9+，
// 本仓库锁定的 Qt 版本没有，只能自己写；行为按 Qt 文档对齐(只替换 '/'，不动 "//" 前缀)。
QString toNative(const QString &path)
{
    if (QDir::separator() == QLatin1Char('/'))
        return path;
    QString out = path;
    out.replace(QLatin1Char('/'), QDir::separator());
    return out;
}

QString normalized(const QString &path)
{
    return toNative(QDir(path).absolutePath());
}

// cubism 的 FileReferences 里路径用 '/'，Windows 上 Qt 两种分隔符都吃，
// 但比较与拼绝对路径前先统一，避免 "textures/a.png" 与 "textures\\a.png" 判成两个。
QString resolve(const QString &baseDir, const QString &relative)
{
    return QDir(baseDir).absoluteFilePath(toNative(relative));
}

void collectModelJsons(const QDir &dir, int depth, QStringList *out)
{
    if (!dir.exists() || depth > kMaxDepth)
        return;
    const QStringList hits = dir.entryList(QStringList() << QStringLiteral("*.model3.json"),
                                          QDir::Files, QDir::Name);
    for (const QString &f : hits)
        out->append(dir.absoluteFilePath(f));
    const QStringList subs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &s : subs)
        collectModelJsons(QDir(dir.absoluteFilePath(s)), depth + 1, out);
}

} // namespace

QString KanbanModelManager::defaultModelsRoot()
{
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("data/kanban/models"));
}

bool KanbanModelManager::validateModelJson(const QString &jsonPath, ModelInfo *out)
{
    if (!out)
        return false;
    out->problems.clear();
    out->valid = false;
    out->textureCount = out->motionCount = out->expressionCount = 0;
    out->modelJsonPath = toNative(QFileInfo(jsonPath).absoluteFilePath());
    out->rootDir = QFileInfo(out->modelJsonPath).absolutePath();
    out->moc3Path.clear();

    QFile file(out->modelJsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        out->problems << QStringLiteral("无法读取 %1").arg(QFileInfo(jsonPath).fileName());
        return false;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        out->problems << QStringLiteral("JSON 解析失败: %1").arg(err.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    const QJsonObject refs = root.value(QStringLiteral("FileReferences")).toObject();
    if (refs.isEmpty()) {
        out->problems << QStringLiteral("缺少 FileReferences 字段");
        return false;
    }

    // .moc3 是 Cubism 3/4 模型的必需件；.moc(旧 Cubism 2)不支持，明确报出来。
    const QString moc = refs.value(QStringLiteral("Moc")).toString();
    if (moc.isEmpty()) {
        // Cubism 2 的 .model.json 才用 "moc" 字段名，这里顺手把它认出来，
        // 给用户一个可理解的报错，而不是含糊的「缺文件」。
        const QString legacy = root.value(QStringLiteral("moc")).toString();
        out->problems << (legacy.isEmpty()
                              ? QStringLiteral("FileReferences.Moc 为空")
                              : QStringLiteral("这是 Cubism2 旧模型(%1)，本模块只支持 Cubism3+ 的 .model3.json")
                                    .arg(legacy));
    } else {
        out->moc3Path = resolve(out->rootDir, moc);
        if (!QFileInfo::exists(out->moc3Path))
            out->problems << QStringLiteral("缺少模型文件 %1").arg(moc);
    }

    const QJsonArray textures = refs.value(QStringLiteral("Textures")).toArray();
    if (textures.isEmpty()) {
        out->problems << QStringLiteral("缺少贴图列表 FileReferences.Textures");
    } else {
        for (const QJsonValue &v : textures) {
            const QString rel = v.toString();
            if (rel.isEmpty() || !QFileInfo::exists(resolve(out->rootDir, rel)))
                out->problems << QStringLiteral("缺少贴图 %1").arg(rel);
            else
                ++out->textureCount;
        }
    }

    const QJsonObject motions = refs.value(QStringLiteral("Motions")).toObject();
    out->motionCount = motions.count();
    for (auto it = motions.begin(); it != motions.end(); ++it) {
        const QJsonArray list = it.value().toArray();
        for (const QJsonValue &v : list) {
            const QString rel = v.toObject().value(QStringLiteral("File")).toString();
            if (!rel.isEmpty() && !QFileInfo::exists(resolve(out->rootDir, rel)))
                out->problems << QStringLiteral("缺少动作文件 %1").arg(rel);
        }
    }
    out->expressionCount = refs.value(QStringLiteral("Expressions")).toArray().count();

    const QString physics = refs.value(QStringLiteral("Physics")).toString();
    if (!physics.isEmpty() && !QFileInfo::exists(resolve(out->rootDir, physics)))
        out->problems << QStringLiteral("缺少物理文件 %1").arg(physics);
    const QString pose = refs.value(QStringLiteral("Pose")).toString();
    if (!pose.isEmpty() && !QFileInfo::exists(resolve(out->rootDir, pose)))
        out->problems << QStringLiteral("缺少姿势文件 %1").arg(pose);

    out->valid = out->problems.isEmpty();
    return out->valid;
}

int KanbanModelManager::rescan(const QString &extraDir)
{
    m_models.clear();
    QStringList searched;
    const QString root = defaultModelsRoot();
    searched << root;
    if (!extraDir.isEmpty() && toNative(extraDir) != normalized(root))
        searched << extraDir;

    QStringList files;
    for (const QString &dir : searched) {
        QDir d(dir);
        if (!d.exists()) {
            videodiag::log(videodiag::Level::Info,
                           QStringLiteral("模型目录不存在，跳过: %1").arg(dir),
                           QStringLiteral("KanbanModel"));
            continue;
        }
        collectModelJsons(d, 1, &files);
    }

    QSet<QString> seen;
    int validCount = 0;
    for (const QString &f : files) {
        const QString key = normalized(f);
        if (seen.contains(key))
            continue;
        seen.insert(key);
        ModelInfo info;
        validateModelJson(f, &info);
        // 展示名：优先用模型目录名，其次用文件名去掉 .model3.json。
        info.name = QDir(info.rootDir).dirName();
        if (info.name.isEmpty())
            info.name = QFileInfo(f).completeBaseName();
        info.id = info.name;
        if (!info.valid) {
            videodiag::log(videodiag::Level::Warning,
                           QStringLiteral("模型不可用: %1 -> %2")
                               .arg(info.name, info.problems.join(QStringLiteral("; "))),
                           QStringLiteral("KanbanModel"));
        } else {
            ++validCount;
        }
        m_models.append(info);
    }

    videodiag::log(videodiag::Level::Info,
                   QStringLiteral("模型扫描完成: 共 %1 个，可用 %2 个(扫描目录 %3)")
                       .arg(m_models.size())
                       .arg(validCount)
                       .arg(searched.join(QStringLiteral(" | "))),
                   QStringLiteral("KanbanModel"));
    return validCount;
}

QVector<const ModelInfo *> KanbanModelManager::validModels() const
{
    QVector<const ModelInfo *> out;
    for (const ModelInfo &m : m_models) {
        if (m.valid)
            out.append(&m);
    }
    return out;
}

const ModelInfo *KanbanModelManager::byJsonPath(const QString &path) const
{
    const int i = indexByJsonPath(path);
    return i >= 0 ? &m_models.at(i) : nullptr;
}

int KanbanModelManager::indexByJsonPath(const QString &path) const
{
    const QString key = toNative(QFileInfo(path).absoluteFilePath()).toLower();
    for (int i = 0; i < m_models.size(); ++i) {
        if (m_models.at(i).modelJsonPath.toLower() == key)
            return i;
    }
    return -1;
}

const ModelInfo *KanbanModelManager::nextValidAfter(const QString &currentJsonPath) const
{
    const QVector<const ModelInfo *> list = validModels();
    if (list.size() < 2)
        return nullptr;
    int pos = 0;
    for (int i = 0; i < list.size(); ++i) {
        if (list.at(i)->modelJsonPath ==
            toNative(QFileInfo(currentJsonPath).absoluteFilePath())) {
            pos = i;
            break;
        }
    }
    return list.at((pos + 1) % list.size());
}

} // namespace kanban

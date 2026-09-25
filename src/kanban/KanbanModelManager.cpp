#include "kanban/KanbanModelManager.h"
#include "kanban/ModelJsonRepair.h"

#include "core/Diagnostics.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace kanban {

namespace {

constexpr int kMaxDepth = 4; // 模型目录最多向下找 4 层，避免误指向盘根时全树遍历

// 分隔符统一为平台本地形式。等价于 QDir::fromSeparators()，但那个 API 要 Qt 6.9+，
// 本仓库锁定的 Qt 版本没有；行为按 Qt 文档对齐(只替换 '/'，不动 "//" 前缀)。
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

// cubism 的 FileReferences 里路径用 '/'，统一后再比较与拼绝对路径，避免
// "textures/a.png" 与 "textures\\a.png" 判成两个。
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

// 预览图缓存键：模型目录相对模型根目录的路径，分隔符统一换成 '#'。模型直接躺在
// 根目录下的多层子目录时，叶子名会重名，必须带路径才对得上号。模型 json 直接躺在
// 根目录(相对路径为 ".")或跑到根目录之外(".." 开头)时没有安全键，返回空串。
QString thumbKeyFor(const QString &rootDir, const QString &modelsRoot)
{
    const QString rel =
        QDir(modelsRoot).relativeFilePath(QDir(rootDir).absolutePath());
    if (rel.isEmpty() || rel == QLatin1String(".") ||
        rel.startsWith(QLatin1String("../")))
        return QString();
    QString key = rel;
    key.replace(QLatin1Char('/'), QLatin1Char('#'));
    key.replace(QLatin1Char('\\'), QLatin1Char('#'));
    return key;
}

} // namespace

// 路径查表的键。别名表(m_shadowAlias)的键与值都必须用它 —— 跟 indexByJsonPath 用同一套
// 规范化，否则「存进去的键」和「查表时算出来的键」会差一个大小写/分隔符而永远查不到。
QString lookupKey(const QString &path)
{
    return toNative(QFileInfo(path).absoluteFilePath()).toLower();
}

// 「这是哪个模型」的身份：.moc3 路径 + 贴图集合。
//
// 贴图**必须**进这个键：同一个 .moc3 换一套贴图是**另一个模型**（实测本机 6 组换装
// 变体，如 47__l2d_202.u 的 texture_00 / texture_01 是同一套动作的两个外观），
// 只看 moc3 会把它们合并、藏掉真模型。只按目录分组同样不行 —— 实测有目录放 6 个
// 各自独立 moc3 的模型（LOVE³ 的 akira 系列）。
//
// 贴图先排序：json 里写贴图的顺序不影响它渲染成什么样。
static QString modelIdentity(const ModelInfo &info)
{
    QStringList textures;
    textures.reserve(info.texturePaths.size());
    for (const QString &t : info.texturePaths)
        textures << normalized(t);
    textures.sort();
    return normalized(info.moc3Path) + QLatin1Char('\n') + textures.join(QLatin1Char('\n'));
}

QString KanbanModelManager::defaultModelsRoot()
{
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("data/models"));
}

bool KanbanModelManager::validateModelJson(const QString &jsonPath, ModelInfo *out)
{
    if (!out)
        return false;
    out->problems.clear();
    out->warnings.clear();
    out->valid = false;
    out->textureCount = out->motionCount = out->expressionCount = 0;
    out->texturePaths.clear();
    out->modelJsonPath = toNative(QFileInfo(jsonPath).absoluteFilePath());
    out->rootDir = QFileInfo(out->modelJsonPath).absolutePath();
    out->moc3Path.clear();

    // 读盘 + 修补排版 + 补贴图列表，全走共用入口（与装载器口径一致，见 prepareSettingBytes）。
    const PreparedSetting prep = prepareSettingBytes(out->modelJsonPath);
    if (!prep.ok) {
        out->problems << prep.readError;
        return false;
    }
    const JsonRepair &repair = prep.repair;
    const TextureCompletion &tex = prep.tex;
    if (repair.touched) {
        applog::log(applog::Level::Info,
                    QStringLiteral("模型 json 已修补: %1 -> %2")
                        .arg(QFileInfo(jsonPath).fileName(), repair.notes.join(QStringLiteral("; "))),
                    QStringLiteral("KanbanModel"));
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(prep.bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // 报错里带上「已经试过哪些修补」：光一句「解析失败」，用户既不知道是编码问题
        // 还是语法问题（两者的修法完全不同），也不知道程序已经替他做过什么。
        QString reason = QStringLiteral("JSON 解析失败: %1").arg(err.errorString());
        if (!repair.notes.isEmpty())
            reason += QStringLiteral("（已尝试: %1）").arg(repair.notes.join(QStringLiteral("; ")));
        out->problems << reason;
        return false;
    }
    if (tex.inferred)
        out->warnings << tex.note;
    const QJsonObject root = doc.object();
    const QJsonObject refs = root.value(QStringLiteral("FileReferences")).toObject();
    if (refs.isEmpty()) {
        out->problems << QStringLiteral("缺少 FileReferences 字段");
        return false;
    }

    // .moc3 是 Cubism 3/4 模型的必需件；.moc(旧 Cubism 2)不支持，明确报出来。
    const QString moc = refs.value(QStringLiteral("Moc")).toString();
    if (moc.isEmpty()) {
        // Cubism 2 的 .model.json 才用 "moc" 字段名，顺手认出来给一个可理解的报错，
        // 而不是含糊的「缺文件」。
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

    // 贴图是**硬要求**：装载器的 validateTextures() 对「一张都没有」与「声明的解不出来」
    // 都直接判装载失败，所以这里也必须致命，不能宽容。
    if (tex.textures.isEmpty()) {
        out->problems << (tex.note.isEmpty()
                              ? QStringLiteral("缺少贴图列表 FileReferences.Textures")
                              : tex.note);
    } else {
        for (const QString &rel : tex.textures) {
            if (!QFileInfo::exists(resolve(out->rootDir, rel)))
                out->problems << QStringLiteral("缺少贴图 %1").arg(rel);
            else {
                ++out->textureCount;
                out->texturePaths << resolve(out->rootDir, rel);
            }
        }
    }

    const QJsonObject motions = refs.value(QStringLiteral("Motions")).toObject();
    for (auto it = motions.begin(); it != motions.end(); ++it) {
        const QJsonArray list = it.value().toArray();
        out->motionCount += int(list.size());
        for (const QJsonValue &v : list) {
            const QString rel = v.toObject().value(QStringLiteral("File")).toString();
            if (!rel.isEmpty() && !QFileInfo::exists(resolve(out->rootDir, rel)))
                out->warnings << QStringLiteral("缺少动作文件 %1").arg(rel);
        }
    }
    out->expressionCount = refs.value(QStringLiteral("Expressions")).toArray().count();

    // 物理与姿势**不是**必需件：装载器读不到就跳过，模型照样能显示与播动作（只是少了
    // 头发/衣摆的物理，或少了部件显隐的淡入淡出）。素材里这类坏引用极常见 —— 本机实测
    // 109 个模型写着 `"Physics": ".physics3.json"`（连文件名都没有），
    // 若在这里判死，它们就整批上不了墙，而实际上一个都装得动。
    const QString physics = refs.value(QStringLiteral("Physics")).toString();
    if (!physics.isEmpty() && !QFileInfo::exists(resolve(out->rootDir, physics)))
        out->warnings << QStringLiteral("缺少物理文件 %1（已跳过物理）").arg(physics);
    const QString pose = refs.value(QStringLiteral("Pose")).toString();
    if (!pose.isEmpty() && !QFileInfo::exists(resolve(out->rootDir, pose)))
        out->warnings << QStringLiteral("缺少姿势文件 %1（已跳过姿势）").arg(pose);

    out->valid = out->problems.isEmpty();
    return out->valid;
}

int KanbanModelManager::rescan(bool force)
{
    // 已经扫过且不是强制重扫：直接返回上次结果。启动路径那两次先后脚的调用因此合并成
    // 一次遍历（见头文件说明）。返回值必须是**上次的可用数**，不能返回 0 —— 调用方拿它
    // 判断「有没有模型」。
    if (m_scanned && !force) {
        int valid = 0;
        for (const ModelInfo &m : m_models) {
            if (m.valid)
                ++valid;
        }
        return valid;
    }

    m_models.clear();
    m_shadowAlias.clear();
    m_scanned = true;
    const QString root = defaultModelsRoot();

    QStringList files;
    const QDir modelDir(root);
    if (modelDir.exists()) {
        collectModelJsons(modelDir, 1, &files);
    } else {
        applog::log(applog::Level::Info,
                       QStringLiteral("模型目录不存在，跳过: %1").arg(root),
                       QStringLiteral("KanbanModel"));
    }

    QSet<QString> seen;
    int validCount = 0;
    int warnedCount = 0;
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
        info.thumbKey = thumbKeyFor(info.rootDir, root);
        if (!info.valid) {
            applog::log(applog::Level::Warning,
                           QStringLiteral("模型不可用: %1 -> %2")
                               .arg(info.name, info.problems.join(QStringLiteral("; "))),
                           QStringLiteral("KanbanModel"));
        } else {
            ++validCount;
            // 能装但缺件/被程序补过，也要留痕：这类模型在墙上看不出异样，只有日志能
            // 解释「为什么这个模型没有头发物理」或者「贴图是程序猜的」。
            //
            // ⚠️ 逐条明细只能走 **Debug**。全库实测有 127 个这样的模型（109 个写着
            // `"Physics": ".physics3.json"` 空文件名 + 18 个贴图靠推断），每扫一轮
            // 就是 127 行 Info —— 而扫描每运行一次要做两轮，日志 1MB 一滚，几趟就翻页。
            // Debug 默认不落盘（见 applog 的分级说明），要查明细时开诊断模式即可；
            // 默认日志里只留下面那行汇总。
            if (!info.warnings.isEmpty()) {
                ++warnedCount;
                applog::log(applog::Level::Debug,
                               QStringLiteral("模型可用但有缺件: %1 -> %2")
                                   .arg(info.name, info.warnings.join(QStringLiteral("; "))),
                               QStringLiteral("KanbanModel"));
            }
        }
        m_models.append(info);
    }

    if (warnedCount > 0) {
        applog::log(applog::Level::Info,
                       QStringLiteral("模型可用但有缺件: %1 个（贴图靠推断 / 缺物理或姿势文件；"
                                      "逐条明细需开诊断模式 YUMEIREN_DIAG=1）")
                           .arg(warnedCount),
                       QStringLiteral("KanbanModel"));
    }

    // 同一个模型的两份描述只留一份。
    //
    // 素材包里常见这种搭配：一份是工具生成的**精简描述**（FileReferences 里只有
    // Moc / Textures，没有动作、表情，也没有 Groups），一份是原始完整描述。两份都合法时
    // 精简那份会在墙上多出一张同名卡，点开还没动作 —— 用户看到的「这个模型解析不出动作」
    // 就是这么来的。同组只留信息量最大的（动作 + 表情条数）。
    //
    // 两条边界必须守住：
    //   ① **只在合法项里挑**：精简描述存在的意义正是让原始描述坏掉的模型还能显示
    //      （实测有 20 个模型是这种情况），这时它是唯一能用的那份，丢掉它模型就从墙上消失。
    //   ② 分组键见 modelIdentity()：必须带贴图集合，否则会合并掉换装变体。
    QHash<QString, int> bestOf;          // 组键 -> m_models 下标
    QHash<int, int> winnerOf;            // 被隐藏项下标 -> 取代它的项下标
    for (int i = 0; i < m_models.size(); ++i) {
        if (!m_models.at(i).valid)
            continue;
        const QString key = modelIdentity(m_models.at(i));
        const auto found = bestOf.constFind(key);
        if (found == bestOf.constEnd()) {
            bestOf.insert(key, i);
            continue;
        }
        const ModelInfo &candidate = m_models.at(i);
        const ModelInfo &best = m_models.at(found.value());
        const int candidateScore = candidate.motionCount + candidate.expressionCount;
        const int bestScore = best.motionCount + best.expressionCount;
        // 并列时留先扫到的（扫描顺序由目录名排序决定，稳定可复现）。
        const int winner = candidateScore > bestScore ? i : found.value();
        const int loser = candidateScore > bestScore ? found.value() : i;
        bestOf[key] = winner;
        winnerOf.insert(loser, winner);
    }

    QVector<ModelInfo> kept;
    kept.reserve(m_models.size());
    for (int i = 0; i < m_models.size(); ++i) {
        const ModelInfo &info = m_models.at(i);
        const auto loserIt = winnerOf.constFind(i);
        if (loserIt == winnerOf.constEnd()) {
            kept.append(info);
            continue;
        }
        // 别名兜底：配置里存的可能正是被取代那份的路径（实测用户配置就是），不兜底会走到
        // 「找不到当前模型 → 回退到第一个模型」，用户的选中项被悄悄换掉。
        m_shadowAlias.insert(lookupKey(info.modelJsonPath),
                             m_models.at(loserIt.value()).modelJsonPath.toLower());
        --validCount;
        applog::log(applog::Level::Info,
                    QStringLiteral("同一模型的精简描述被完整描述取代，已隐藏: %1 (%2 -> %3)")
                        .arg(info.name, QFileInfo(info.modelJsonPath).fileName(),
                             QFileInfo(m_models.at(loserIt.value()).modelJsonPath).fileName()),
                    QStringLiteral("KanbanModel"));
    }
    m_models = kept;

    applog::log(applog::Level::Info,
                   QStringLiteral("模型扫描完成: 共 %1 个，可用 %2 个(扫描目录 %3)")
                       .arg(m_models.size())
                       .arg(validCount)
                       .arg(root),
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
    const QString key = lookupKey(path);
    for (int i = 0; i < m_models.size(); ++i) {
        if (m_models.at(i).modelJsonPath.toLower() == key)
            return i;
    }
    // 兜底：这份描述被同组的完整描述取代了（见 rescan）。不兜底会走到「找不到当前模型
    // → 回退到第一个模型」，用户的选中项被悄悄换成别的模型。
    const auto alias = m_shadowAlias.constFind(key);
    if (alias == m_shadowAlias.constEnd())
        return -1;
    for (int i = 0; i < m_models.size(); ++i) {
        if (m_models.at(i).modelJsonPath.toLower() == alias.value())
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

int KanbanModelManager::successorIndexAfterRemoval(int removedIndex, int remainingCount)
{
    if (remainingCount <= 0)
        return 0; // 调用方负责判空，这里只保证不越界
    // 删掉第 i 个后原来的第 i+1 个补到第 i 位，故 i 在新列表范围内时指的就是「下一个」；
    // i 越界说明删的是最后一个，绕回第一个。
    if (removedIndex >= 0 && removedIndex < remainingCount)
        return removedIndex;
    return 0;
}

} // namespace kanban

#include "kanban/CubismModel_p.h"
#include "kanban/ImageDecode.h"
#include "kanban/ModelJsonRepair.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QAudioOutput>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <cctype>
#include <limits>

#include "kanban/CubismRuntime.h"
#include "Effect/CubismBreath.hpp"
#include "Effect/CubismEyeBlink.hpp"
#include "Effect/CubismLook.hpp"
#include "Math/CubismModelMatrix.hpp"
#include "Motion/CubismBreathUpdater.hpp"
#include "Motion/CubismExpressionMotionManager.hpp"
#include "Motion/CubismExpressionUpdater.hpp"
#include "Motion/CubismEyeBlinkUpdater.hpp"
#include "Motion/CubismLookUpdater.hpp"
#include "Motion/CubismMotion.hpp"
#include "Motion/CubismMotionManager.hpp"
#include "Motion/CubismPhysicsUpdater.hpp"
#include "Motion/CubismPoseUpdater.hpp"
#include "Physics/CubismPhysics.hpp"

namespace kanban::detail {

namespace {

bool readFile(const QString &path, QByteArray *out, QString *outError)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (outError) {
            *outError = QStringLiteral("读不到文件：%1(%2)")
                            .arg(QFileInfo(path).fileName(), file.errorString());
        }
        return false;
    }
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (outError) {
            *outError = QStringLiteral("读取失败：%1(%2)")
                            .arg(QFileInfo(path).fileName(), file.errorString());
        }
        return false;
    }
    if (data.isEmpty()) {
        if (outError) {
            *outError = QStringLiteral("文件是空的：%1").arg(QFileInfo(path).fileName());
        }
        return false;
    }
    if (data.size() > (std::numeric_limits<csmSizeInt>::max)()) {
        if (outError) {
            *outError = QStringLiteral("文件超过 SDK 大小限制：%1").arg(QFileInfo(path).fileName());
        }
        return false;
    }
    *out = data;
    return true;
}

const csmByte *bytesOf(const QByteArray &array)
{
    return reinterpret_cast<const csmByte *>(array.constData());
}

csmSizeInt sizeOf(const QByteArray &array)
{
    return static_cast<csmSizeInt>(array.size());
}

// CubismJson 的数字解析只认换行/逗号做终点，压缩 JSON 里 12} 会直接判非法。
// 这里保持紧凑输出，仅对「数字后紧跟 }/]」补换行；用引号状态避免误改字符串内容。
void terminateJsonNumbers(QByteArray &buffer)
{
    bool inString = false;
    for (int i = 1; i < buffer.size(); ++i) {
        const char ch = buffer.at(i);
        if (inString) {
            if (ch == '\\') {
                ++i;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if ((ch == '}' || ch == ']')
            && (std::isdigit(static_cast<unsigned char>(buffer.at(i - 1)))
                || buffer.at(i - 1) == '.')) {
            buffer.insert(i, '\n');
            ++i;
        }
    }
}

// Pose 的部件显隐淡化时长由 pose3.json 的 FadeInTime 决定，缺省 0.5s(见 CubismPose.cpp 的
// DefaultFadeInSeconds)。而 _fadeTimeSeconds 是私有成员、没有 setter，唯一入口就是
// CubismPose::Create() 读这个字段 —— 要改时长，只能改喂给 LoadPose() 的字节流。
//
// 为什么非改不可：Haru 这类模型每条手臂有**两套贴图**(Part01ArmRA001 / RB001)，动作里用
// 「与部件同名的参数」决定当前显示哪一套。SDK 默认 0.5s 的淡化意味着切换期间两套手臂同时
// 半透明可见 —— 实测过渡中点 pRA=0.699 / pRB=0.501，整套过渡约 29 帧 @60fps，肉眼看就是
// 「上一个动作的手臂重影」。
//
// **现取值 0 = 硬切**（2026-09-20 用户看过三档对照后选定）：DoFade 在 _fadeTimeSeconds == 0
// 时直接 newOpacity = 1.0f，同组其余件算出来是 0 —— 部件显隐在**一帧之内**完成，没有任何
// 叠加帧，重影彻底消失。代价是 Haru 两套手臂贴图位置差得很远（身侧 vs 胸前），硬切能看到
// 手臂「瞬移」而不是「快速移过去」；这是用户明确选择换取的（宁可瞬移，不要重影）。
//
// 实测三档(60fps)：0.5s → 中间值 29 帧；0.12s → 7 帧；**0 → 0 帧**。
// 想改回渐变就调这一个常量（0.12 是当时试过、叠影几乎看不出的折中值），**别去动别处**。
//
// 只改我们自己读进来的 buffer，third_party/ 一行不动。
constexpr double kPoseFadeSeconds = 0.0;

void applyPoseFadeSeconds(QByteArray &buffer)
{
    if (buffer.isEmpty()) {
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(buffer, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // 解析不了就原样交回去：pose3.json 是用户素材，宁可退回 SDK 的默认值，
        // 也不能因为我们改不动它就让模型装不上。
        return;
    }
    QJsonObject root = doc.object();
    root.insert(QStringLiteral("FadeInTime"), kPoseFadeSeconds);
    buffer = QJsonDocument(root).toJson(QJsonDocument::Compact);
    terminateJsonNumbers(buffer);
}

// 物理文件引用的参数在 moc3 里可能根本不存在 —— 全库 660 个带物理的模型里有 9 个：
//   3__l2d_31.u     -> PARAM_SWING_HEADWEAR1/2/3, PARAM_SWING_RIBBON, PARAM_SWING_SKIRT
//   l2d00.u         -> PARAM_CHESIWA_X, PARAM_HUDIEJIE_P_1, PARAM_LACE_X, PARAM_XIUSHI …
//   l2d2__l2d_77.u  -> PARAM_HAT_X, PARAM_PiFeng_X, PARAM_Qun_X …
//   sharedassets30  -> ParamTitiLLR, ParamBodyFB, ParamHead …
// 素材作者改了参数名却忘了同步物理文件，是纯粹的素材缺陷。
//
// ⚠️ 但 SDK 对这种情况**不做任何检查**：CubismPhysics::Evaluate 里
//     currentOutputs[i].DestinationParameterIndex = model->GetParameterIndex(...)
// 拿到 -1 之后直接用它下标：
//     &parameterValues[-1] / parameterMinimumValues[-1] / parameterMaximumValues[-1]
//     _parameterCaches[-1] = ...
// 于是越界**写** Core 的参数数组。破坏的是 Core 自己那一大块内存里的相邻数据，
// 堆元数据毫发无损，所以 _heapchk 一路全绿、任何堆检测都抓不到；真正的症状是
// **随后** csmUpdateModel 递归遍历变形器树时读到被写坏的索引、跳到无效地址而
// 段错误(gdb: 崩在 csmUpdateModel 内部，backtrace 报 "corrupt stack")。
// 实测该模型「能装载、能画首帧，第二帧 _model->Update() 才崩」——极难反推到物理文件。
//
// 这里在装载前把引用不到参数的 Input/Output 条目剔掉，让 SDK 永远拿不到 -1。
// 代价只有「那几个参数不再被物理驱动」——它们本来就不存在，本来也没被驱动过。
//
// Meta 的 TotalInputCount / TotalOutputCount 必须同步改小：SDK 用它们预分配
// Inputs / Outputs 数组(CubismPhysics.cpp 的 UpdateSize)。比实际大只是多几个空槽、
// 无害，但两边保持一致以后不容易踩坑。
//
// parameterIds 由调用方从 Core 取(见装载处)，这里只做集合比对，不依赖 Core 类型。
bool dropPhysicsMissingParams(QByteArray &buffer, const QSet<QByteArray> &parameterIds)
{
    if (buffer.isEmpty() || parameterIds.isEmpty()) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(buffer, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false; // 解析不了就原样交回去，宁可让 SDK 自己去判断。
    }
    QJsonObject root = doc.object();
    const QJsonArray settings = root.value(QLatin1String("PhysicsSettings")).toArray();
    if (settings.isEmpty()) {
        return false;
    }

    int droppedInputs = 0;
    int droppedOutputs = 0;
    int inputCount = 0;
    int outputCount = 0;
    QJsonArray fixedSettings;
    for (const QJsonValue &settingValue : settings) {
        QJsonObject setting = settingValue.toObject();

        QJsonArray inputs;
        for (const QJsonValue &value : setting.value(QLatin1String("Input")).toArray()) {
            const QJsonObject input = value.toObject();
            const QByteArray id = input.value(QLatin1String("Source"))
                                      .toObject()
                                      .value(QLatin1String("Id"))
                                      .toString()
                                      .toUtf8();
            if (!parameterIds.contains(id)) {
                ++droppedInputs;
                continue;
            }
            inputs.append(input);
        }
        setting.insert(QLatin1String("Input"), inputs);

        QJsonArray outputs;
        for (const QJsonValue &value : setting.value(QLatin1String("Output")).toArray()) {
            const QJsonObject output = value.toObject();
            const QByteArray id = output.value(QLatin1String("Destination"))
                                      .toObject()
                                      .value(QLatin1String("Id"))
                                      .toString()
                                      .toUtf8();
            if (!parameterIds.contains(id)) {
                ++droppedOutputs;
                continue;
            }
            outputs.append(output);
        }
        setting.insert(QLatin1String("Output"), outputs);

        inputCount += int(inputs.size());
        outputCount += int(outputs.size());
        fixedSettings.append(setting);
    }

    if (droppedInputs == 0 && droppedOutputs == 0) {
        return false; // 没有可剔的，保持原始字节流不动。
    }

    root.insert(QLatin1String("PhysicsSettings"), fixedSettings);
    QJsonObject meta = root.value(QLatin1String("Meta")).toObject();
    if (!meta.isEmpty()) {
        meta.insert(QLatin1String("TotalInputCount"), inputCount);
        meta.insert(QLatin1String("TotalOutputCount"), outputCount);
        root.insert(QLatin1String("Meta"), meta);
    }
    buffer = QJsonDocument(root).toJson(QJsonDocument::Compact);
    terminateJsonNumbers(buffer);
    cubismruntime::logWarn(
        QStringLiteral("物理文件引用了不存在的参数，已剔除：输入 %1 条 / 输出 %2 条")
            .arg(droppedInputs)
            .arg(droppedOutputs));
    return true;
}

// Cubism 的动作 JSON 里 Meta 计数一旦对不上，解析器会按声明值预分配缓冲区、
// 再按 Curves 实际内容写入，越界后往往到后续字符串赋值才崩成 0xc0000374。
// 这里用与 SDK 相同的段展开规则重算计数；能修复的走修复后的字节流，形状
// 无法保证安全的直接跳过。
bool normalizeMotionJson(QByteArray &buffer, const QString &path)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(buffer, &parseError);
    const QJsonObject root = doc.object();
    const QJsonObject meta = root.value(QLatin1String("Meta")).toObject();
    const QJsonArray curves = root.value(QLatin1String("Curves")).toArray();
    const QJsonArray events = root.value(QLatin1String("UserData")).toArray();
    if (!doc.isObject() || meta.isEmpty() || curves.isEmpty()) {
        return false;
    }

    qsizetype segmentCount = 0;
    qsizetype pointCount = 0;
    for (const QJsonValue &curveValue : curves) {
        const QJsonObject curve = curveValue.toObject();
        const QJsonArray segments = curve.value(QLatin1String("Segments")).toArray();
        if (curve.value(QLatin1String("Target")).type() != QJsonValue::String
            || curve.value(QLatin1String("Id")).type() != QJsonValue::String
            || segments.isEmpty()) {
            return false;
        }

        qsizetype position = 0;
        while (position < segments.size()) {
            if (position == 0) {
                if (segments.at(0).type() != QJsonValue::Double
                    || segments.at(1).type() != QJsonValue::Double) {
                    return false;
                }
                ++pointCount;
                position += 2;
            }
            const qsizetype segment = segments.at(position).toInteger(-1);
            switch (segment) {
            case 0:
            case 2:
            case 3:
                if (position + 2 >= segments.size()) {
                    return false;
                }
                ++pointCount;
                position += 3;
                break;
            case 1:
                if (position + 6 >= segments.size()) {
                    return false;
                }
                pointCount += 3;
                position += 7;
                break;
            default:
                return false;
            }
            ++segmentCount;
        }
    }

    for (const QJsonValue &eventValue : events) {
        const QJsonObject event = eventValue.toObject();
        if (event.value(QLatin1String("Time")).type() != QJsonValue::Double
            || event.value(QLatin1String("Value")).type() != QJsonValue::String) {
            return false;
        }
    }

    const qsizetype curveCount = curves.size();
    const qsizetype eventCount = events.size();
    if (meta.value(QLatin1String("CurveCount")).toInteger() == curveCount
        && meta.value(QLatin1String("TotalSegmentCount")).toInteger() == segmentCount
        && meta.value(QLatin1String("TotalPointCount")).toInteger() == pointCount
        && meta.value(QLatin1String("UserDataCount")).toInteger() == eventCount) {
        terminateJsonNumbers(buffer);
        return true;
    }

    QJsonObject fixedMeta = meta;
    fixedMeta.insert(QLatin1String("CurveCount"), static_cast<qint64>(curveCount));
    fixedMeta.insert(QLatin1String("TotalSegmentCount"), static_cast<qint64>(segmentCount));
    fixedMeta.insert(QLatin1String("TotalPointCount"), static_cast<qint64>(pointCount));
    fixedMeta.insert(QLatin1String("UserDataCount"), static_cast<qint64>(eventCount));
    QJsonObject fixedRoot = root;
    fixedRoot.insert(QLatin1String("Meta"), fixedMeta);
    QJsonDocument fixedDoc(fixedRoot);
    buffer = fixedDoc.toJson(QJsonDocument::Compact);
    terminateJsonNumbers(buffer);
    cubismruntime::logWarn(QStringLiteral("动作元数据与内容不一致，已按实际内容修正：%1")
                .arg(QFileInfo(path).fileName()));
    return true;
}

// —— 按网格隐藏：清单文件 ——
//
// 清单就是查看器(tools/live2d-part-inspector)导出的 `<模型名>.hidden.json`，**原样丢进
// 模型目录即可生效** —— 这是本程序里唯一「按约定文件名自动发现」的东西，因为路径是我们
// 自己扫出来的。反例是 pose3.json：它的路径只能由 model3.json 的 FileReferences.Pose 指定，
// 丢文件进去没有任何效果（这条差别值得记住，很容易混）。
//
// 只认两个字段：
//   hiddenParts     部件 Id 列表（连同它的整棵子树一起隐藏）
//   hiddenDrawables 单个网格 Id 列表
//
// `partOpacities` 刻意不读：那是「半透明」不是「隐藏」，而且部件不透明度每帧都会被 Pose
// 写一遍，外部塞一个值进去只会和 Pose 打架。读到就记一条日志，免得用户以为它生效了。
struct MeshHideList
{
    QStringList partIds;
    QStringList drawableIds;
    int files = 0;
    QStringList unreadable; // 读不出/不是合法 JSON 的文件名，装载后统一告警
};

MeshHideList readMeshHideLists(const QString &dirPath)
{
    MeshHideList out;
    const QDir dir(dirPath);
    const QStringList files =
        dir.entryList(QStringList{QStringLiteral("*.hidden.json")}, QDir::Files, QDir::Name);
    for (const QString &name : files) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            out.unreadable << name;
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            out.unreadable << name;
            continue;
        }
        ++out.files;
        const QJsonObject root = doc.object();
        const auto collect = [&root](const char *key, QStringList *into) {
            const QJsonArray array = root.value(QLatin1String(key)).toArray();
            for (const QJsonValue &value : array) {
                // 数组里混进非字符串是手改出来的，跳过即可，不该因此让整个模型装不上。
                const QString id = value.toString();
                if (!id.isEmpty()) {
                    *into << id;
                }
            }
        };
        collect("hiddenParts", &out.partIds);
        collect("hiddenDrawables", &out.drawableIds);
        if (root.contains(QLatin1String("partOpacities"))) {
            cubismruntime::logInfo(
                QStringLiteral("隐藏清单 %1 里的 partOpacities 已忽略：本程序只做「显示/隐藏」，"
                               "要半透明请改 model3.json 的参数绑定")
                    .arg(name));
        }
    }
    return out;
}
} // namespace

QString CubismModelImpl::relativeToHome(const csmChar *relative) const
{
    if (!relative || !*relative) {
        return QString();
    }
    return QDir(m_homeDir).filePath(QString::fromUtf8(relative));
}

bool CubismModelImpl::setup(const QString &modelJsonPath, QString *outError)
{
    m_homeDir = QFileInfo(modelJsonPath).absolutePath();

    if (!loadSettingJson(modelJsonPath, outError)) {
        return false;
    }

    // 1) 模型本体(.moc3)。失败时 _model 保持空，下面统一判定。
    if (const csmChar *mocFile = m_setting->GetModelFileName(); mocFile && *mocFile) {
        QByteArray buffer;
        QString readError;
        const QString path = relativeToHome(mocFile);
        if (!readFile(path, &buffer, &readError)) {
            if (outError) {
                *outError = readError;
            }
            return false;
        }
        const auto mocVersion = GetMocVersionFromBuffer(bytesOf(buffer), sizeOf(buffer));
        LoadModel(bytesOf(buffer), sizeOf(buffer), false);
        if (!_model) {
            if (outError) {
                *outError = QStringLiteral("%1 无法解析(moc 版本 0x%2)，"
                                           "可能由比本 SDK 更新的编辑器导出")
                                .arg(QFileInfo(path).fileName())
                                .arg(static_cast<quint32>(mocVersion), 8, 16, QLatin1Char('0'));
            }
            return false;
        }
    } else {
        if (outError) {
            *outError = QStringLiteral("%1 里没有 FileReferences.Model").arg(QFileInfo(modelJsonPath).fileName());
        }
        return false;
    }

    const csmInt32 expressionCount = m_setting->GetExpressionCount();
    for (csmInt32 i = 0; i < expressionCount; ++i) {
        const csmString name = m_setting->GetExpressionName(i);
        QByteArray buffer;
        if (!readFile(relativeToHome(m_setting->GetExpressionFileName(i)), &buffer, nullptr)) {
            // 可选表情缺失不阻止模型装载，但要说清楚**哪一个**没了 —— 以前全静默，
            // 用户看到「模型说有三个表情、实际只有两个」时无从下手。
            cubismruntime::logWarn(QStringLiteral("表情文件缺失，已跳过：%1 → %2")
                                       .arg(QString::fromUtf8(name.GetRawString()),
                                            QString::fromUtf8(m_setting->GetExpressionFileName(i))));
            continue;
        }
        ACubismMotion *motion = LoadExpression(bytesOf(buffer), sizeOf(buffer), name.GetRawString());
        if (!motion) {
            cubismruntime::logWarn(QStringLiteral("表情解析失败，已跳过：%1")
                                       .arg(QString::fromUtf8(name.GetRawString())));
            continue;
        }
        if (m_expressions.IsExist(name)) {
            ACubismMotion::Delete(m_expressions[name]);
        } else {
            m_expressionNames << QString::fromUtf8(name.GetRawString());
        }
        m_expressions[name] = motion;
    }
    if (!m_expressionNames.isEmpty()) {
        _updateScheduler.AddUpdatableList(CSM_NEW CubismExpressionUpdater(*_expressionManager));
    }

    if (const csmChar *physicsFile = m_setting->GetPhysicsFileName(); physicsFile && *physicsFile) {
        QByteArray buffer;
        if (readFile(relativeToHome(physicsFile), &buffer, nullptr)) {
            // 物理引用不到的参数必须在装载前剔掉，否则 SDK 会拿 -1 当索引用、
            // 越界写 Core 的参数数组。详见 dropPhysicsMissingParams 的说明。
            QSet<QByteArray> parameterIds;
            const csmInt32 parameterCount =
                Live2D::Cubism::Core::csmGetParameterCount(_model->GetModel());
            const char **ids = Live2D::Cubism::Core::csmGetParameterIds(_model->GetModel());
            for (csmInt32 i = 0; i < parameterCount; ++i) {
                parameterIds.insert(QByteArray(ids[i]));
            }
            dropPhysicsMissingParams(buffer, parameterIds);
            LoadPhysics(bytesOf(buffer), sizeOf(buffer));
            if (_physics) {
                _updateScheduler.AddUpdatableList(CSM_NEW CubismPhysicsUpdater(*_physics));
            } else {
                // 物理是可选件：装不上不该挡住模型，但要留痕 —— 否则「头发不晃」这种
                // 现象只能靠猜（素材坏了？还是本来就没物理文件？）。
                cubismruntime::logWarn(QStringLiteral("物理文件装载失败（模型仍可用）：%1")
                                           .arg(QFileInfo(modelJsonPath).fileName()));
            }
        }
    }

    if (const csmChar *poseFile = m_setting->GetPoseFileName(); poseFile && *poseFile) {
        QByteArray buffer;
        if (readFile(relativeToHome(poseFile), &buffer, nullptr)) {
            // Pose 与动作的 PartOpacity 曲线是**一套配合使用的机制，缺一不可**：
            //   动作里 `Target: PartOpacity, Id: Part01ArmRA001` 这条曲线写的是**与部件
            //   同名的参数**(见 CubismMotion.cpp 的 PartOpacity 分支)；Pose 再读这个参数
            //   判断「同组内哪层可见」，然后用 DoFade 把新层淡入、旧层淡出 ——
            //   切动作的交叉淡化就是它做的，不需要我们再自己补一套。
            // 曾经把 CubismMotion 那段改成直接 SetPartOpacity(绕开参数通道)，Pose 就再也
            // 收不到「可见层变了」的信号，只能恒定认定组内第 0 层可见、每帧把它抬起来，
            // 表现为切动作时多出一双手。**两处必须一起用原生的**：把那段改回去之后，
            // 这里也一定要装 Pose —— 否则部件显隐没人做淡化，过渡会变成硬切。
            // 淡化**时长**另说：SDK 默认 0.5s 太长，会让两套手臂长时间叠着，见
            // applyPoseFadeSeconds 的说明。
            applyPoseFadeSeconds(buffer);
            LoadPose(bytesOf(buffer), sizeOf(buffer));
            if (_pose) {
                _updateScheduler.AddUpdatableList(CSM_NEW CubismPoseUpdater(*_pose));
            } else {
                // Pose 装不上的后果比物理显眼得多：切动作时部件显隐没有淡化，会硬切、
                // 甚至短暂叠着两套手脚。必须留痕，否则用户只会说「切换时闪一下」。
                cubismruntime::logWarn(QStringLiteral("姿势文件装载失败（切动作可能出现硬切/部件重叠）：%1")
                                           .arg(QFileInfo(modelJsonPath).fileName()));
            }
        }
    }

    if (m_setting->GetEyeBlinkParameterCount() > 0) {
        _eyeBlink = CubismEyeBlink::Create(m_setting);
        if (_eyeBlink) {
            _updateScheduler.AddUpdatableList(CSM_NEW CubismEyeBlinkUpdater(m_motionUpdated, *_eyeBlink));
        }
    }

    _breath = CubismBreath::Create();
    {
        CubismIdHandle idBreath = CubismFramework::GetIdManager()->GetId(ParamBreath);
        csmVector<CubismBreath::BreathParameterData> breathParameters;
        breathParameters.PushBack(CubismBreath::BreathParameterData(parameterId(ParamAngleX), 0.0f, 15.0f, 6.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(parameterId(ParamAngleY), 0.0f, 8.0f, 3.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(parameterId(ParamAngleZ), 0.0f, 10.0f, 5.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(parameterId(ParamBodyAngleX), 0.0f, 4.0f, 15.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(idBreath, 0.5f, 0.5f, 3.2345f, 0.5f));
        _breath->SetParameters(breathParameters);
        _updateScheduler.AddUpdatableList(CSM_NEW CubismBreathUpdater(*_breath));
    }

    if (const csmChar *userDataFile = m_setting->GetUserDataFile(); userDataFile && *userDataFile) {
        QByteArray buffer;
        if (readFile(relativeToHome(userDataFile), &buffer, nullptr)) {
            LoadUserData(bytesOf(buffer), sizeOf(buffer));
        } else {
            cubismruntime::logWarn(QStringLiteral("用户数据文件读取失败（模型仍可用）：%1")
                                       .arg(QFileInfo(modelJsonPath).fileName()));
        }
    }

    // 让运动知道哪些参数归眨眼/口型接管，避免轨迹把它们按回原位。
    for (csmInt32 i = 0; i < m_setting->GetEyeBlinkParameterCount(); ++i) {
        m_eyeBlinkIds.PushBack(m_setting->GetEyeBlinkParameterId(i));
    }
    for (csmInt32 i = 0; i < m_setting->GetLipSyncParameterCount(); ++i) {
        m_lipSyncIds.PushBack(m_setting->GetLipSyncParameterId(i));
    }

    _look = CubismLook::Create();
    {
        csmVector<CubismLook::LookParameterData> lookParameters;
        lookParameters.PushBack(CubismLook::LookParameterData(parameterId(ParamAngleX), 30.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(parameterId(ParamAngleY), 0.0f, 30.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(parameterId(ParamAngleZ), 0.0f, 0.0f, -30.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(parameterId(ParamBodyAngleX), 10.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(parameterId(ParamEyeBallX), 1.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(parameterId(ParamEyeBallY), 0.0f, 1.0f));
        _look->SetParameters(lookParameters);
        _updateScheduler.AddUpdatableList(CSM_NEW CubismLookUpdater(*_look, *_dragManager));
    }

    _updateScheduler.SortUpdatableList();

    csmMap<csmString, csmFloat32> layout;
    m_setting->GetLayoutMap(layout);
    _modelMatrix->SetupFromLayout(layout);
    _model->SaveParameters();

    // 纹理校验(不解位图)：丢图/坏图要在装载这步报出来。真正解码放在 ensureGl，
    // 因为上限取决于绘制面尺寸。
    if (!validateTextures(outError)) {
        return false;
    }

    // 运动全部预载：边点边读盘会有明显卡顿。
    const csmInt32 groupCount = m_setting->GetMotionGroupCount();
    QVector<QVector<int>> loadedPerGroup; // 与 m_motionGroups 同序
    for (csmInt32 i = 0; i < groupCount; ++i) {
        const QString group = QString::fromUtf8(m_setting->GetMotionGroupName(i));
        m_motionGroups << group;
        if (group.compare(QStringLiteral("idle"), Qt::CaseInsensitive) == 0) {
            m_idleGroup = m_motionGroups.size() - 1;
        }
        loadedPerGroup.append(preloadMotionGroup(group));
    }
    if (m_idleGroup < 0 && !m_motionGroups.isEmpty()) {
        m_idleGroup = 0;
    }
    // 待机动作也是模型动作的一部分：统计与循环都包含它，不能只算普通动作。
    for (int g = 0; g < m_motionGroups.size(); ++g) {
        for (const int index : loadedPerGroup.at(g)) {
            m_playableMotions.append(qMakePair(g, index));
        }
    }
    m_motionCursor = 0;
    m_currentMotionOrdinal = m_playableMotions.isEmpty() ? 0 : 1;
    _motionManager->StopAllMotions();

    // 按网格隐藏清单：必须在 _model 装好之后才能把 Id 解析成下标，所以放在最后一步。
    // 没有清单 / 清单全是无效 Id 都只意味着「本模型不隐藏任何东西」，不影响装载成功。
    loadMeshHideList();

    _updating = false;
    _initialized = true;
    return true;
}

void CubismModelImpl::loadMeshHideList()
{
    m_meshHideDrawables.clear();
    m_meshHideFiles = 0;
    m_meshHideRules = 0;
    m_meshHideMissing = 0;
    if (!_model || m_homeDir.isEmpty()) {
        return;
    }

    const MeshHideList list = readMeshHideLists(m_homeDir);
    for (const QString &name : list.unreadable) {
        cubismruntime::logWarn(
            QStringLiteral("隐藏清单 %1 读不出来或不是合法 JSON，已跳过").arg(name));
    }
    m_meshHideFiles = list.files;
    m_meshHideRules = int(list.partIds.size() + list.drawableIds.size());
    if (m_meshHideRules == 0) {
        return;
    }

    const csmInt32 partCount = _model->GetPartCount();
    const csmInt32 drawableCount = _model->GetDrawableCount();
    CubismIdManager *idManager = CubismFramework::GetIdManager();
    QStringList missing;

    // 部件 Id → 下标。**必须与 partCount 比大小**：GetPartIndex 对模型里不存在的 Id 会现场
    // 分配一个 >= partCount 的合成下标(见 CubismModel.cpp 的 GetPartIndex 实现)，直接拿去
    // 当真实下标用会越界。
    std::vector<bool> partHidden(partCount > 0 ? size_t(partCount) : 0, false);
    for (const QString &id : list.partIds) {
        const QByteArray utf8 = id.toUtf8();
        const csmInt32 index = _model->GetPartIndex(idManager->GetId(utf8.constData()));
        if (index < 0 || index >= partCount) {
            ++m_meshHideMissing;
            missing << id;
            continue;
        }
        partHidden[size_t(index)] = true;
    }

    std::vector<csmInt32> hit;
    hit.reserve(size_t(drawableCount));
    for (csmInt32 d = 0; d < drawableCount; ++d) {
        // 沿父链往上找：部件树是嵌套的，隐藏父部件必须连同整棵子树一起藏。
        // 步数上限取部件总数，防素材把父指针写成环。
        csmInt32 part = _model->GetDrawableParentPartIndex(csmUint32(d));
        for (csmInt32 step = 0; part >= 0 && part < partCount && step <= partCount; ++step) {
            if (partHidden[size_t(part)]) {
                hit.push_back(d);
                break;
            }
            part = _model->GetPartParentPartIndex(csmUint32(part));
        }
    }

    for (const QString &id : list.drawableIds) {
        const QByteArray utf8 = id.toUtf8();
        const csmInt32 index = _model->GetDrawableIndex(idManager->GetId(utf8.constData()));
        if (index < 0 || index >= drawableCount) {
            ++m_meshHideMissing;
            missing << id;
            continue;
        }
        hit.push_back(index);
    }

    // 去重：同一层可能同时被「父部件」和「自己」点名。
    std::sort(hit.begin(), hit.end());
    hit.erase(std::unique(hit.begin(), hit.end()), hit.end());
    m_meshHideDrawables = std::move(hit);

    // 清单是用户手改得最多的东西，装载时把账目报清楚，省得「怎么没生效」要靠猜。
    QString line = QStringLiteral("网格隐藏清单：%1 份文件 / %2 条 Id → 隐藏 %3 个网格")
                       .arg(m_meshHideFiles)
                       .arg(m_meshHideRules)
                       .arg(int(m_meshHideDrawables.size()));
    if (m_meshHideMissing > 0) {
        line += QStringLiteral("；%1 条 Id 在本模型里找不到：%2")
                    .arg(m_meshHideMissing)
                    .arg(missing.mid(0, 8).join(QLatin1Char(',')));
        if (missing.size() > 8) {
            line += QStringLiteral("…");
        }
    }
    cubismruntime::logInfo(line);
}

bool CubismModelImpl::loadSettingJson(const QString &jsonPath, QString *outError)
{
    // 「读文件 + 修补 + 补贴图列表」三步已经收进 prepareSettingBytes —— **必须与
    // KanbanModelManager 的校验共用同一份**，否则会出现「列表里有、点开装不上」。
    // 素材里的 model3.json 常见三种「人眼看不出来」的毛病：全角空格、尾随逗号、非 UTF-8
    // 编码；贴图列表为空时还要按目录补一张（SDK 会把「贴图数为 0」判成无法装载）。
    PreparedSetting prepared = prepareSettingBytes(jsonPath);
    if (!prepared.ok) {
        if (outError) {
            *outError = prepared.readError;
        }
        return false;
    }
    if (prepared.repair.touched) {
        cubismruntime::logInfo(QStringLiteral("模型 json 已修补：%1 -> %2")
                                   .arg(QFileInfo(jsonPath).fileName(),
                                        prepared.repair.notes.join(QStringLiteral("; "))));
    }
    if (prepared.tex.inferred) {
        cubismruntime::logInfo(QStringLiteral("模型 json 贴图列表已补全：%1 -> %2")
                                   .arg(QFileInfo(jsonPath).fileName(), prepared.tex.note));
    }
    QByteArray &buffer = prepared.bytes;
    m_setting = CSM_NEW CubismModelSettingJson(bytesOf(buffer), sizeOf(buffer));
    if (!m_setting->IsValid()) {
        if (outError) {
            *outError = QStringLiteral("%1 不是合法的 model3.json").arg(QFileInfo(jsonPath).fileName());
        }
        CSM_DELETE(m_setting);
        m_setting = nullptr;
        return false;
    }
    return true;
}

CubismIdHandle CubismModelImpl::parameterId(const csmChar *name)
{
    return CubismFramework::GetIdManager()->GetId(name);
}

bool CubismModelImpl::validateTextures(QString *outError)
{
    const csmInt32 count = m_setting->GetTextureCount();
    if (count <= 0) {
        if (outError) {
            *outError = QStringLiteral("model3.json 里没有 FileReferences.Textures");
        }
        return false;
    }
    for (csmInt32 i = 0; i < count; ++i) {
        const QString path = relativeToHome(m_setting->GetTextureFileName(i));
        if (path.isEmpty()) {
            continue; // 空名字纹理位：model3.json 允许留空槽，交给 decodeTextures 放占位
        }
        QImageReader reader(path);
        if (!reader.canRead()) {
            if (outError) {
                *outError = QStringLiteral("%1 无法解码(不是 PNG 或已损坏)")
                                .arg(QFileInfo(path).fileName());
            }
            return false;
        }
    }
    return true;
}

bool CubismModelImpl::decodeTextures(int windowMaxDim, QString *outError)
{
    const csmInt32 count = m_setting->GetTextureCount();
    if (count <= 0) {
        if (outError) {
            *outError = QStringLiteral("model3.json 里没有 FileReferences.Textures");
        }
        return false;
    }
    m_textureImages.clear();
    m_textureImages.reserve(count);
    for (csmInt32 i = 0; i < count; ++i) {
        const QString path = relativeToHome(m_setting->GetTextureFileName(i));
        if (path.isEmpty()) {
            m_textureImages << QImage();
            continue;
        }
        QImageReader reader(path);
        // 上限要同时看「窗口够用值」与「素材自身尺寸」，后者才是关键：16384×8192 的打包
        // 图集压到窗口够用值(1024) 就是 1/16，整只角色糊掉。判定只用 reader.size()。
        const int maxDim = textureMaxDimFor(windowMaxDim, reader.size());
        // 全尺寸位图只在 scaled 之前短暂存在，0 = 原尺寸；顺带处理「图太大撞上 Qt 全局
        // 分配上限」，详见 kanban/ImageDecode.h。
        QImage image = readImageDownscaled(reader, maxDim);
        if (image.isNull()) {
            // 记住失败原因(ensureGl 由 30fps 时钟驱动，逐帧重试会变成读盘+解码风暴)，
            // 并带上 Qt 错误串 —— 否则「超过分配上限」会被误判成「文件坏了」。
            const QString why = reader.errorString();
            m_decodeError = QStringLiteral("%1 解码失败：%2")
                                .arg(QFileInfo(path).fileName(),
                                     why.isEmpty() ? QStringLiteral("原因未知") : why);
            if (outError) {
                *outError = m_decodeError;
            }
            return false;
        }
        // 预乘 alpha 的 RGBA8：GL 侧格式固定，边缘也不会有半透明白边。
        m_textureImages << image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    }
    m_decodeError.clear();
    return true;
}

QVector<int> CubismModelImpl::preloadMotionGroup(const QString &group)
{
    QVector<int> loaded; // 真读进来的组内序号
    const QByteArray groupUtf8 = group.toUtf8();
    const csmChar *groupName = groupUtf8.constData();
    const csmInt32 count = m_setting->GetMotionCount(groupName);
    for (csmInt32 i = 0; i < count; ++i) {
        const QString key = QStringLiteral("%1_%2").arg(group).arg(i);
        const QByteArray keyUtf8 = key.toUtf8();
        QByteArray buffer;
        const QString motionPath = relativeToHome(m_setting->GetMotionFileName(groupName, i));
        if (!readFile(motionPath, &buffer, nullptr) || !normalizeMotionJson(buffer, motionPath)) {
            continue;
        }
        CubismMotion *motion = static_cast<CubismMotion *>(
            LoadMotion(bytesOf(buffer), sizeOf(buffer), keyUtf8.constData(),
                       nullptr, nullptr, m_setting, groupName, i, _motionConsistency));
        if (!motion) {
            cubismruntime::logWarn(QStringLiteral("动作装载失败：%1")
                                       .arg(QFileInfo(motionPath).fileName()));
            continue;
        }
        // 告诉运动数据哪些参数归眨眼/口型接管，否则轨迹会把它们按回原位。
        motion->SetEffectIds(m_eyeBlinkIds, m_lipSyncIds);
        // 这里曾经把 fadeIn/fadeOut 都压到 0.3s 来「缩短切动作残影」，现已删除。
        // 那个补丁压的是**参数**的淡入淡出，而残影来自**部件显隐**——那条链路走的是
        // 「动作曲线写同名参数 → Pose 读参数做淡入淡出」(见 CubismMotion.cpp 的
        // PartOpacity 分支)，压根不看 motion 的 fade 时间，所以它从来压不掉残影，
        // 只剩「把姿势过渡从素材默认的 1.0s 压快到 0.3s」这个纯负面效果：
        // 切动作显得生硬、不自然。
        const csmString name(keyUtf8.constData());
        if (m_motions.IsExist(name)) {
            ACubismMotion::Delete(m_motions[name]);
        } else {
            m_motionKeys << key;
        }
        m_motions[name] = motion;
        loaded << int(i);
    }
    return loaded;
}

void CubismModelImpl::releaseCpu()
{
    if (m_voicePlayer) {
        m_voicePlayer->stop();
        delete m_voicePlayer;
        m_voicePlayer = nullptr;
    }

    // 队列不拥有预载动作，先清除引用再释放动作对象。
    _motionManager->StopAllMotions();
    _expressionManager->StopAllMotions();
    // csmMap::Clear 不释放指针指向的对象。
    for (const QString &key : m_motionKeys) {
        const QByteArray utf8 = key.toUtf8();
        const csmString name(utf8.constData());
        if (m_motions.IsExist(name)) {
            ACubismMotion::Delete(m_motions[name]);
        }
    }
    m_motionKeys.clear();
    m_motions.Clear();

    for (const QString &name : m_expressionNames) {
        const QByteArray utf8 = name.toUtf8();
        const csmString key(utf8.constData());
        if (m_expressions.IsExist(key)) {
            ACubismMotion::Delete(m_expressions[key]);
        }
    }
    m_expressionNames.clear();
    m_expressions.Clear();

    // 这几个裸指针基类析构也会删，先置空防止二次释放。
    if (_eyeBlink) {
        CubismEyeBlink::Delete(_eyeBlink);
        _eyeBlink = nullptr;
    }
    if (_breath) {
        CubismBreath::Delete(_breath);
        _breath = nullptr;
    }
    if (_look) {
        CubismLook::Delete(_look);
        _look = nullptr;
    }
    CSM_DELETE(m_setting);
    m_setting = nullptr;

    m_textureImages.clear();
    m_textureMaxDim = 0;
    m_decodeError.clear();
    m_motionGroups.clear();
    m_playableMotions.clear();
    m_motionCursor = 0;
    m_currentMotionOrdinal = 0;
    m_idleGroup = -1;
    m_nextExpression = 0;
    _initialized = false;
}

void CubismModelImpl::playMotionSound(const QString &group, int index)
{
    // 闸门放这里而不是各调用点：所有会发声的路径都得过这一关。
    // 关掉时连 QMediaPlayer 都不建，省一个音频设备。
    if (!m_soundEnabled) {
        return;
    }
    if (!m_voicePlayer) {
        m_voicePlayer = new QMediaPlayer();
        m_voicePlayer->setAudioOutput(new QAudioOutput(m_voicePlayer));
        m_voicePlayer->audioOutput()->setVolume(1.0f);
    }
    if (!m_setting) {
        return;
    }

    m_voicePlayer->stop();
    const csmChar *relative = m_setting->GetMotionSoundFileName(
        group.toUtf8().constData(), index);
    if (!relative || !*relative) {
        return;
    }

    const QString path = relativeToHome(relative);
    if (!QFile::exists(path)) {
        cubismruntime::logWarn(QStringLiteral("动作语音文件不存在：%1")
                                   .arg(QFileInfo(path).fileName()));
        return;
    }

    m_voicePlayer->setSource(QUrl::fromLocalFile(path));
    m_voicePlayer->play();
}

} // namespace kanban::detail

namespace kanban {

std::unique_ptr<CubismModel> createCubismModel()
{
    return std::make_unique<detail::CubismModelImpl>();
}

} // namespace kanban

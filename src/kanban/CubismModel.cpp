#include "kanban/CubismModel_p.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <limits>

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

    // 2) 表情
    const csmInt32 expressionCount = m_setting->GetExpressionCount();
    for (csmInt32 i = 0; i < expressionCount; ++i) {
        const csmString name = m_setting->GetExpressionName(i);
        QByteArray buffer;
        if (!readFile(relativeToHome(m_setting->GetExpressionFileName(i)), &buffer, nullptr)) {
            continue; // 可选表情缺失不阻止模型装载。
        }
        ACubismMotion *motion = LoadExpression(bytesOf(buffer), sizeOf(buffer), name.GetRawString());
        if (!motion) {
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

    // 3) 物理摆动
    if (const csmChar *physicsFile = m_setting->GetPhysicsFileName(); physicsFile && *physicsFile) {
        QByteArray buffer;
        if (readFile(relativeToHome(physicsFile), &buffer, nullptr)) {
            LoadPhysics(bytesOf(buffer), sizeOf(buffer));
            if (_physics) {
                _updateScheduler.AddUpdatableList(CSM_NEW CubismPhysicsUpdater(*_physics));
            }
        }
    }

    // 4) 姿态(-parts 二选一)
    if (const csmChar *poseFile = m_setting->GetPoseFileName(); poseFile && *poseFile) {
        QByteArray buffer;
        if (readFile(relativeToHome(poseFile), &buffer, nullptr)) {
            LoadPose(bytesOf(buffer), sizeOf(buffer));
            if (_pose) {
                _updateScheduler.AddUpdatableList(CSM_NEW CubismPoseUpdater(*_pose));
            }
        }
    }

    // 5) 眨眼
    if (m_setting->GetEyeBlinkParameterCount() > 0) {
        _eyeBlink = CubismEyeBlink::Create(m_setting);
        if (_eyeBlink) {
            _updateScheduler.AddUpdatableList(CSM_NEW CubismEyeBlinkUpdater(m_motionUpdated, *_eyeBlink));
        }
    }

    // 呼吸参数沿用官方示例。
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

    // 7) 用户数据(运动事件里按名取用，缺了不致命)
    if (const csmChar *userDataFile = m_setting->GetUserDataFile(); userDataFile && *userDataFile) {
        QByteArray buffer;
        if (readFile(relativeToHome(userDataFile), &buffer, nullptr)) {
            LoadUserData(bytesOf(buffer), sizeOf(buffer));
        }
    }

    // 8) 让运动知道哪些参数归眨眼/口型接管，避免运动轨迹把它们按回原位
    for (csmInt32 i = 0; i < m_setting->GetEyeBlinkParameterCount(); ++i) {
        m_eyeBlinkIds.PushBack(m_setting->GetEyeBlinkParameterId(i));
    }
    for (csmInt32 i = 0; i < m_setting->GetLipSyncParameterCount(); ++i) {
        m_lipSyncIds.PushBack(m_setting->GetLipSyncParameterId(i));
    }

    // 9) 看向(头/眼跟随鼠标)：把 2D 拖动量映射回旋转参数
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

    // 登记完毕后统一排序，保证看向等更新器在运动之后执行。
    _updateScheduler.SortUpdatableList();

    // 10) 布局(model3.json 的 Layout 可以改宽度和锚点)
    csmMap<csmString, csmFloat32> layout;
    m_setting->GetLayoutMap(layout);
    _modelMatrix->SetupFromLayout(layout);
    _model->SaveParameters();

    // 11) 纹理解码(还没上 GPU)：这一步就能发现丢图、坏图
    if (!decodeTextures(outError)) {
        return false;
    }

    // 12) 运动全部预载：看板娘会频繁触发动作，边点边读盘会有明显卡顿
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
    // 待机组确定后再收集可播动作，排除兜底为第 0 组的情况。
    for (int g = 0; g < m_motionGroups.size(); ++g) {
        if (g == m_idleGroup) {
            continue;
        }
        for (const int index : loadedPerGroup.at(g)) {
            m_playableMotions.append(qMakePair(g, index));
        }
    }
    _motionManager->StopAllMotions();

    _updating = false;
    _initialized = true;
    return true;
}

bool CubismModelImpl::loadSettingJson(const QString &jsonPath, QString *outError)
{
    QByteArray buffer;
    QString readError;
    if (!readFile(jsonPath, &buffer, &readError)) {
        if (outError) {
            *outError = readError;
        }
        return false;
    }
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

bool CubismModelImpl::decodeTextures(QString *outError)
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
        QByteArray buffer;
        QString readError;
        if (!readFile(path, &buffer, &readError)) {
            if (outError) {
                *outError = readError;
            }
            return false;
        }
        QImage image = QImage::fromData(buffer, "PNG");
        if (image.isNull()) {
            if (outError) {
                *outError = QStringLiteral("%1 解码失败(不是 PNG 或已损坏)")
                                .arg(QFileInfo(path).fileName());
            }
            return false;
        }
        // 统一成「预乘 alpha 的 RGBA8」：GL 侧格式固定，边缘也不会有半透明的白边。
        m_textureImages << image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    }
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
        if (!readFile(relativeToHome(m_setting->GetMotionFileName(groupName, i)), &buffer, nullptr)) {
            continue;
        }
        CubismMotion *motion = static_cast<CubismMotion *>(
            LoadMotion(bytesOf(buffer), sizeOf(buffer), keyUtf8.constData(),
                       nullptr, nullptr, m_setting, groupName, i, _motionConsistency));
        if (!motion) {
            continue;
        }
        // 告诉运动数据哪些参数归眨眼/口型接管，否则轨迹会把它们按回原位。
        motion->SetEffectIds(m_eyeBlinkIds, m_lipSyncIds);
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
    // 队列不拥有预载动作，先清除引用，再释放动作对象。
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

    // 这几个裸指针基类析构也会删，先置空就是防止二次释放。
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
    m_motionGroups.clear();
    m_playableMotions.clear();
    m_motionCursor = 0;
    m_idleGroup = -1;
    m_nextExpression = 0;
    _initialized = false;
}

} // namespace kanban::detail

namespace kanban {

std::unique_ptr<CubismModel> createCubismModel()
{
    return std::make_unique<detail::CubismModelImpl>();
}

} // namespace kanban

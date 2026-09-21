#include "kanban/CubismModel_p.h"
#include "kanban/CubismRuntime.h"

#include <QFileInfo>
#include <random>

#include "Motion/CubismExpressionMotionManager.hpp"
#include "Motion/CubismMotionManager.hpp"

namespace kanban::detail {

using cubismruntime::logDebug;

namespace {

// 与官方示例一致的优先级口径：数值越大越优先，低优先级请求会被拒绝。
constexpr int kPriorityIdle = 1;
constexpr int kPriorityForce = 3;

std::mt19937 &randomEngine()
{
    static std::mt19937 engine{std::random_device{}()};
    return engine;
}

int randomBelow(int bound)
{
    if (bound <= 1) {
        return 0;
    }
    std::uniform_int_distribution<int> dist(0, bound - 1);
    return dist(randomEngine());
}

} // namespace

void CubismModelImpl::update(float deltaSeconds)
{
    if (!_model) {
        return;
    }
    m_motionUpdated = false;

    _model->LoadParameters(); // 回到上一帧保存的状态，运动之间才能叠加
    // 推进判据只用 SDK 自己的 IsFinished()。本 SDK 的 ACubismMotion 把 _isLoop 默认设成
    // false，CubismMotion::Parse 里读 Meta.Loop 那行是注释掉的，全工程也没有任何地方调
    // SetIsLoop —— 于是 GetDuration() 返回的是真实时长，动作播完一定会置结束标志。
    // (曾按「循环动作 GetDuration() 恒为 -1、IsFinished() 恒假」加过一套按 Meta.Duration
    //  计时的兜底推进；那个前提不成立，那段代码从来没生效过，已删。)
    if (_motionManager->IsFinished()) {
        if (m_motionLoopEnabled && playableMotionCount() > 0) {
            playNextMotion();
        } else {
            startIdleMotion();
        }
    } else {
        m_motionUpdated = _motionManager->UpdateMotion(_model, deltaSeconds);
    }
    _model->SaveParameters();

    // 调度器同时推进拖动平滑，不能再次手动更新 _dragManager。
    _updateScheduler.OnLateUpdate(_model, deltaSeconds);
    _model->Update();

    // 顺序是硬要求：Update() 内部会 csmUpdateModel(从 opacities 重算 IsVisible 位) +
    // csmResetDrawableDynamicFlags，所以隐藏位只能在它**之后**写，写完直接进 draw()。
    // 反过来先写再 Update，Core 会把这一位重算回 1，网格照画 —— 这是最容易踩的一步。
    applyMeshHide();
}

void CubismModelImpl::applyMeshHide()
{
    if (!_model || m_meshHideDrawables.empty() || !meshHideEnabled()) {
        return;
    }
    // Core 给的是 const 视图，但这份标志位内存归模型所有、本身可写：浏览器端同样是直接改
    // 这份 Uint8 数组。这是「隐藏单个网格」唯一不需要改 SDK 的入口。
    // (csmGetDrawableDynamicFlags 声明在 Live2D::Cubism::Core，而本文件只 using 了
    //  Framework 命名空间，故这里写全名。)
    unsigned char *flags = const_cast<unsigned char *>(
        Live2D::Cubism::Core::csmGetDrawableDynamicFlags(_model->GetModel()));
    if (!flags) {
        return;
    }
    // 只清「可见」这一位，其余(顶点变化、绘制顺序变化…)留给 Core 自己维护。
    const unsigned char keepMask =
        static_cast<unsigned char>(~static_cast<unsigned int>(Live2D::Cubism::Core::csmIsVisible));
    const csmInt32 count = _model->GetDrawableCount();
    for (const csmInt32 index : m_meshHideDrawables) {
        if (index >= 0 && index < count) {
            flags[index] &= keepMask;
        }
    }
}

void CubismModelImpl::refreshMeshHide()
{
    if (!_model) {
        return;
    }
    // 开关刚被切换，而暂停时动画时钟不走、update() 不会被调用。IsVisible 位是 Core 在
    // Update() 里从 opacities 重算的(实测：手工清掉后下一次 Update() 会自己回来)，所以
    // 这里重跑一次 Update() 就拿到干净的位，再按新开关写一遍即可。
    _model->Update();
    applyMeshHide();
}

QString CubismModelImpl::meshHideText() const
{
    if (m_meshHideFiles <= 0) {
        return QString();
    }
    QString text = QStringLiteral("清单 %1 份，隐藏 %2 个网格")
                       .arg(m_meshHideFiles)
                       .arg(int(m_meshHideDrawables.size()));
    if (m_meshHideMissing > 0) {
        text += QStringLiteral("(%1 条 Id 未匹配)").arg(m_meshHideMissing);
    }
    return text;
}

void CubismModelImpl::setDragTarget(float x, float y)
{
    if (_dragManager) {
        _dragManager->Set(x, y); // Look 更新器每帧取这里的值
    }
}

bool CubismModelImpl::startHitReaction(const QPointF &normalized)
{
    if (!_model || !m_setting || m_setting->GetHitAreasCount() <= 0) {
        logDebug(QStringLiteral("点击无反应：本模型没有配置 HitAreas"));
        return false;
    }
    // IsHit 收到的是「投影之后、modelMatrix 之前」的坐标，故要除掉投影修正量。
    const float x = static_cast<float>(normalized.x() * 2.0 - 1.0) / m_projScaleX;
    const float y = static_cast<float>(1.0 - normalized.y() * 2.0) / m_projScaleY;

    const csmInt32 count = m_setting->GetHitAreasCount();
    for (csmInt32 i = 0; i < count; ++i) {
        if (!IsHit(m_setting->GetHitAreaId(i), x, y)) {
            continue;
        }
        const QString area = QString::fromUtf8(m_setting->GetHitAreaName(i));

        // 头部优先触发表情，兼容常见的中英文命名。
        const bool headLike = area.contains(QLatin1String("Head"), Qt::CaseInsensitive)
                              || area.contains(QStringLiteral("头"))
                              || area.contains(QStringLiteral("臉"))
                              || area.contains(QStringLiteral("脸"));
        if (headLike && playRandomExpression()) {
            logDebug(QStringLiteral("命中「%1」→ 表情反馈").arg(area));
            return true;
        }

        // 动作惯例：命中区名与运动组同名，或带 Tap 前缀，两种都认。
        const QStringList candidates = {area, QStringLiteral("Tap%1").arg(area)};
        for (const QString &candidate : candidates) {
            for (int g = 0; g < m_motionGroups.size(); ++g) {
                if (m_motionGroups.at(g).compare(candidate, Qt::CaseInsensitive) == 0 &&
                    motionCount(g) > 0) {
                    const bool played =
                        startGroupMotion(g, randomBelow(motionCount(g)), kPriorityForce);
                    logDebug(QStringLiteral("命中「%1」→ 动作组「%2」%3")
                                 .arg(area, candidate,
                                      played ? QStringLiteral("已播放")
                                             : QStringLiteral("被更高优先级的动作挡住")));
                    return played;
                }
            }
        }

        // 无匹配动作时尝试表情反馈。
        if (!headLike && playRandomExpression()) {
            logDebug(QStringLiteral("命中「%1」没有对应动作组 → 退化为表情反馈").arg(area));
            return true;
        }
        logDebug(QStringLiteral("命中「%1」但既无同名/Tap 动作组，也没有可用表情").arg(area));
    }
    logDebug(QStringLiteral("点击未落入任何命中区(归一化 %1, %2)")
                 .arg(normalized.x(), 0, 'f', 3)
                 .arg(normalized.y(), 0, 'f', 3));
    return false;
}

int CubismModelImpl::motionCount(int group) const
{
    if (!m_setting || group < 0 || group >= m_motionGroups.size()) {
        return 0;
    }
    return m_setting->GetMotionCount(m_motionGroups.at(group).toUtf8().constData());
}

bool CubismModelImpl::isIdleMotion(int group, int index) const
{
    if (!m_setting || group != m_idleGroup || group < 0 ||
        group >= m_motionGroups.size()) {
        return false;
    }
    const QString groupName = m_motionGroups.at(group);
    if (groupName.compare(QStringLiteral("idle"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    // 组名不叫 idle 时按文件名认：有些模型把待机和普通动作塞进同一个空名组里，
    // 只靠组名会把手势当待机播。
    if (index < 0 || index >= motionCount(group)) {
        return false;
    }
    const csmChar *fileName =
        m_setting->GetMotionFileName(groupName.toUtf8().constData(), index);
    return QFileInfo(QString::fromUtf8(fileName)).baseName()
               .compare(QStringLiteral("idle"), Qt::CaseInsensitive) == 0;
}

bool CubismModelImpl::startGroupMotion(int group, int index, int priority)
{
    if (!_motionManager || group < 0 || group >= m_motionGroups.size()) {
        return false;
    }
    const QString groupName = m_motionGroups.at(group);
    if (index < 0 || index >= motionCount(group)) {
        return false;
    }

    const QByteArray keyUtf8 = QStringLiteral("%1_%2").arg(groupName).arg(index).toUtf8();
    const csmString key(keyUtf8.constData());
    if (!m_motions.IsExist(key)) {
        return false; // 缺失动作不能占用预留优先级。
    }

    // 强制动作直接占位，其他动作遵循 SDK 优先级规则。
    if (priority == kPriorityForce) {
        _motionManager->SetReservePriority(priority);
    } else if (!_motionManager->ReserveMotion(priority)) {
        return false;
    }

    // 这里刻意不做任何「先停掉旧 motion」的处理：SDK 自己会给旧 motion 挂 fadeOut、给新
    // motion 跑 fadeIn，两段动作按队列顺序叠加，这就是官方的过渡方式。部件显隐那一路
    // (动作的 PartOpacity 曲线写「与部件同名的参数」→ CubismPose::DoFade 读它做交叉淡化)
    // 同样由 SDK 管，别在这里 StopAllMotions()，也别去改 CubismMotion 让它直接写部件。
    const bool started =
        _motionManager->StartMotionPriority(m_motions[key], false, priority) !=
        InvalidMotionQueueEntryHandleValue;
    if (started) {
        // idle 自动回填不覆盖用户最近选中的动作序号；手动播放和命中动作才更新。
        if (priority != kPriorityIdle) {
            m_currentMotionOrdinal = 0;
            for (int i = 0; i < m_playableMotions.size(); ++i) {
                if (m_playableMotions.at(i) == qMakePair(group, index)) {
                    m_currentMotionOrdinal = i + 1;
                    break;
                }
            }
        }
        playMotionSound(groupName, index);
    }
    return started;
}

bool CubismModelImpl::playNextMotion()
{
    const int total = m_playableMotions.size();
    if (total == 0) {
        return false;
    }
    // 顺序轮转避免重复；显式点击使用 Force，允许打断当前动作。
    for (int attempt = 0; attempt < total; ++attempt) {
        const int slot = m_motionCursor;
        const QPair<int, int> &motion = m_playableMotions.at(slot);
        m_motionCursor = (slot + 1) % total;
        if (startGroupMotion(motion.first, motion.second, kPriorityForce)) {
            logDebug(QStringLiteral("切换动作「%1_%2」(第 %3/%4 个)")
                         .arg(m_motionGroups.value(motion.first))
                         .arg(motion.second)
                         .arg(slot + 1)
                         .arg(total));
            return true;
        }
    }
    logDebug(QStringLiteral("切换动作失败：%1 个可播动作全部被优先级挡下").arg(total));
    return false;
}

bool CubismModelImpl::startIdleMotion()
{
    const int count = motionCount(m_idleGroup);
    if (count > 0) {
        // 有些模型把 idle 和普通动作混在同一个组里(Haru 的 Idle 组有 27 个)，必须只从
        // 真正的待机文件里挑；一个都认不出来的兜底组才退回全组随机。
        QVector<int> idleIndexes;
        for (int i = 0; i < count; ++i) {
            if (isIdleMotion(m_idleGroup, i)) {
                idleIndexes << i;
            }
        }
        if (idleIndexes.isEmpty()) {
            idleIndexes.reserve(count);
            for (int i = 0; i < count; ++i) {
                idleIndexes << i;
            }
        }
        // 预览图固定取首个待机动作，运行时随机播放。
        const int index = m_deterministicIdle
                              ? idleIndexes.first()
                              : idleIndexes.at(randomBelow(idleIndexes.size()));
        return startGroupMotion(m_idleGroup, index, kPriorityIdle);
    }
    // 没有 idle 组就随便挑一组顶上，站桩不动比动作重复更难看。
    for (int g = 0; g < m_motionGroups.size(); ++g) {
        const int n = motionCount(g);
        if (n > 0 && startGroupMotion(g, m_deterministicIdle ? 0 : randomBelow(n),
                                      kPriorityIdle)) {
            return true;
        }
    }
    return false;
}

bool CubismModelImpl::setExpressionIndex(int index)
{
    if (!_expressionManager || index < 0 || index >= m_expressionNames.size()) {
        return false;
    }
    const QByteArray utf8 = m_expressionNames.at(index).toUtf8();
    const csmString name(utf8.constData());
    if (!m_expressions.IsExist(name)) {
        return false;
    }
    if (_expressionManager->StartMotion(m_expressions[name], false) ==
        InvalidMotionQueueEntryHandleValue) {
        return false;
    }
    // 仅在播放成功后更新当前表情，供顺序和随机切换避重。
    m_lastExpression = index;
    return true;
}

bool CubismModelImpl::playNextExpression()
{
    if (!_expressionManager || m_expressionNames.isEmpty()) {
        return false;
    }
    const int total = m_expressionNames.size();
    // 顺序轮转而不是随机：用户点「切换表情」是想把几个表情看一遍，
    // 随机抽样会连着撞同一个，看起来像没生效。
    for (int attempt = 0; attempt < total; ++attempt) {
        const int index = m_nextExpression % total;
        m_nextExpression = (index + 1) % total;
        // 只有一个表情的模型不跳过 —— 那一个就是它的全部，重播也算切了。
        if (total > 1 && index == m_lastExpression) {
            continue;
        }
        if (setExpressionIndex(index)) {
            return true;
        }
    }
    return false;
}

bool CubismModelImpl::playRandomExpression()
{
    if (!_expressionManager || m_expressionNames.isEmpty()) {
        return false;
    }
    const int total = m_expressionNames.size();
    if (total == 1) {
        return setExpressionIndex(0);
    }
    // 避开当前这张：摸头若有一半概率挑回同一张，用户会以为点击没生效。
    int index = m_lastExpression;
    for (int attempt = 0; attempt < 8 && index == m_lastExpression; ++attempt) {
        index = randomBelow(total);
    }
    if (index == m_lastExpression) {
        index = (m_lastExpression + 1) % total; // 连续撞车(概率极低)时的确定性兜底
    }
    if (!setExpressionIndex(index)) {
        return false;
    }
    // 同步游标，免得用户接着点「切换表情」时又绕回刚随机挑中的这张。
    m_nextExpression = (index + 1) % total;
    return true;
}

} // namespace kanban::detail

#include "kanban/PlaceholderRenderer.h"
#include "kanban/ImageDecode.h"

#include "core/Diagnostics.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

namespace kanban {

namespace {

// 呼吸与摆动角速度(弧度/秒)取慢值：常驻角色动作幅度大会抢注意力。
constexpr float kBreathOmega = 1.15f;
constexpr float kSwayOmega = 0.55f;
constexpr float kBlinkDuration = 0.13f;

// 视线追踪的档位表 kGazeTuning 在 KanbanRenderer.h，两个后端共用，此处不重复定义。

// 写死的三形变动作，名称与 Live2D 侧 motion 语义对齐，接入后逐项替换。
struct MotionDef {
    const char *name;
    float duration;
};
const MotionDef kMotions[] = {
    {"tap", 0.6f},
    {"bounce", 0.9f},
    {"wave", 1.2f},
};
constexpr int kMotionCount = int(sizeof(kMotions) / sizeof(kMotions[0]));

// 写死的脸部形变组合，命名与 Live2D 侧语义对齐；各字段相对「默认脸」而言。
struct ExpressionDef {
    const char *name;
    float eyeOpen;    // 睁眼高度倍率：0.4≈半闭、1.25≈瞪大
    bool arcEyes;     // 画成上弯弧(开心时的「^ ^」)，盖过 eyeOpen
    float mouthOpen;  // 常态开口度，与动作带来的开口度相加
    float mouthCurve; // 嘴角方向：正=上扬、负=下撇，绝对值=幅度
    int petalTone;    // 花瓣发色偏移(-255..255)，给表情一点整体色温差
};
const ExpressionDef kExpressions[] = {
    {"default", 1.00f, false, 0.00f, 1.0f, 0},
    {"happy", 0.55f, true, 0.35f, 1.9f, 14},
    {"sleepy", 0.38f, false, 0.05f, -0.4f, -10},
    {"surprised", 1.25f, false, 0.70f, 0.15f, 0},
};
constexpr int kExpressionCount = int(sizeof(kExpressions) / sizeof(kExpressions[0]));

} // namespace

bool PlaceholderRenderer::initialize(QString *outError)
{
    if (outError)
        outError->clear();
    m_ready = true;
    m_time = 0.0f;
    m_blinkLeft = 0.0f;
    m_nextBlinkIn = 2.0f;
    m_motionActive = false;
    m_motionIsPlayable = false;
    m_motionIndex = 0;     // 与表情同理：重启后从头开始，不接着上次的序号
    m_currentMotionOrdinal = 1;
    m_expressionIndex = 0; // 重新初始化 = 回到默认脸，免得「重启后还是上次那副表情」
    applog::log(applog::Level::Info,
                   QStringLiteral("占位渲染器初始化完成(QPainter 直绘，无位图搬运)"),
                   QStringLiteral("Live2D"));
    return true;
}

bool PlaceholderRenderer::loadModel(const QString &modelJsonPath, QString *outError)
{
    if (!m_ready) {
        if (outError)
            *outError = QStringLiteral("渲染器尚未初始化");
        return false;
    }
    m_modelName = QFileInfo(modelJsonPath).completeBaseName();
    if (m_modelName.endsWith(QStringLiteral(".model3")))
        m_modelName.chop(7);

    m_modelTexture = QImage();
    m_modelTexturePath.clear();
    m_modelSourceSize = QSize();
    m_modelTextureLimit = 0;
    m_modelTextureFailedLimit = 0;
    m_modelDir = QFileInfo(modelJsonPath).absoluteDir().absolutePath();
    if (!decodeModelTexture(QDir(m_modelDir))) {
        applog::log(applog::Level::Warning,
            QStringLiteral("[Kanban] 占位渲染器未找到纹理，使用默认花朵: %1").arg(modelJsonPath),
            QStringLiteral("Live2D"));
    }

    if (outError)
        outError->clear();
    return true;
}

bool PlaceholderRenderer::decodeModelTexture(const QDir &modelDir)
{
    // 按候选名在模型目录里找纹理，找到第一张能解码的就用。
    const QStringList textureCandidates = {
        QStringLiteral("textures/texture_00.png"),
        QStringLiteral("textures/texture_0.png"),
        m_modelName + QStringLiteral(".png"),
    };
    for (const QString &rel : textureCandidates) {
        const QString path = modelDir.filePath(rel);
        if (!QFileInfo::exists(path))
            continue;

        QImageReader reader(path);
        const QSize sourceSize = reader.size();
        // 上限与 GPU 后端共用一份策略(见 KanbanRenderer.h)：此处常驻 CPU 位图，
        // 4096² 就是 64MB，而它最终只画进这么小的窗口。
        const int maxDim = effectiveTextureLimit(sourceSize);
        QImage decoded = readImageDownscaled(reader, maxDim);
        if (decoded.isNull())
            continue;
        decoded = decoded.convertToFormat(QImage::Format_RGBA8888_Premultiplied);

        // 成功才动成员：重解失败时保持原来那张(糊一点总比空着好)，调用方也靠
        // 「成员没变」区分成功与失败。
        m_modelTexture = decoded;
        m_modelTexturePath = path;
        m_modelSourceSize = sourceSize;
        m_modelTextureLimit = maxDim;
        applog::log(applog::Level::Info,
            QStringLiteral("[Kanban] 占位渲染器加载纹理: %1 (%2x%3, 上限 %4)")
                .arg(path)
                .arg(m_modelTexture.width())
                .arg(m_modelTexture.height())
                .arg(maxDim > 0 ? QString::number(maxDim) : QStringLiteral("原尺寸")),
            QStringLiteral("Live2D"));
        return true;
    }
    return false;
}

int PlaceholderRenderer::effectiveTextureLimit(const QSize &source) const
{
    const QSize pixels(qRound(m_width * m_dpr), qRound(m_height * m_dpr));
    return textureMaxDimFor(textureMaxDimFor(pixels), source);
}

bool PlaceholderRenderer::rebuildTexturesIfNeeded()
{
    // 本模型没解出过纹理就没有可重解的余地。
    if (m_modelTexturePath.isEmpty())
        return false;

    // 纯算术，不读盘：源尺寸是装载时记下的。所以每帧问一次也不心疼 ——
    // 也因此不需要 resize() 里那种 pending 标志。
    const int target = effectiveTextureLimit(m_modelSourceSize);
    if (target <= 0 || target <= m_modelTextureLimit)
        return false; // 原尺寸、或手上这张已经够细
    if (target <= m_modelTextureFailedLimit)
        return false; // 这个上限已经试过且失败了，不再每帧读盘重试

    // 候选纹理名相对**模型目录**，不是纹理文件自己所在的那层(textures/)。
    if (!decodeModelTexture(QDir(m_modelDir))) {
        m_modelTextureFailedLimit = target;
        applog::log(applog::Level::Warning,
            QStringLiteral("[Kanban] 占位渲染器按新上限 %1 重建纹理失败，保持原图").arg(target),
            QStringLiteral("Live2D"));
        return false;
    }
    applog::log(applog::Level::Info,
        QStringLiteral("[Kanban] 占位渲染器绘制面变大到 %1x%2，纹理按新上限 %3 重建")
            .arg(qRound(m_width * m_dpr))
            .arg(qRound(m_height * m_dpr))
            .arg(target),
        QStringLiteral("Live2D"));
    return true;
}

QString PlaceholderRenderer::textureDebugText() const
{
    if (m_modelTexture.isNull())
        return QStringLiteral("无纹理");
    // 报出「已用上限」与「按当前绘制面算出的目标上限」两个数：光看解出来的尺寸分不清
    // 「本来就没有更细的余地」和「该重解却没重解」，这两个数一比就清楚。
    QString text = QStringLiteral("纹理 %1x%2 已用上限 %3 目标上限 %4 源 %5x%6")
                       .arg(m_modelTexture.width())
                       .arg(m_modelTexture.height())
                       .arg(m_modelTextureLimit)
                       .arg(effectiveTextureLimit(m_modelSourceSize))
                       .arg(m_modelSourceSize.width())
                       .arg(m_modelSourceSize.height());
    if (m_modelTextureFailedLimit > 0) {
        // 失败时手上还是旧的那张，别让它看起来像已经重解过。
        text += QStringLiteral(" 重解失败于上限 %1").arg(m_modelTextureFailedLimit);
    }
    return text;
}

void PlaceholderRenderer::unloadModel()
{
    m_modelName.clear();
    m_modelTexture = QImage();
    m_modelDir.clear();
    m_modelTexturePath.clear();
    m_modelSourceSize = QSize();
    m_modelTextureLimit = 0;
    m_modelTextureFailedLimit = 0;
    m_motionActive = false;
    m_motionIsPlayable = false;
    m_currentMotionOrdinal = 0;
}

void PlaceholderRenderer::resize(int width, int height, float devicePixelRatio)
{
    m_width = qMax(1, width);
    m_height = qMax(1, height);
    m_dpr = devicePixelRatio > 0.0f ? devicePixelRatio : 1.0f;
}

void PlaceholderRenderer::update(float deltaSeconds)
{
    if (!m_ready || m_paused)
        return;
    m_time += deltaSeconds;

    // 眨眼：到点触发一次短闭眼。
    m_nextBlinkIn -= deltaSeconds;
    if (!m_motionActive && m_nextBlinkIn <= 0.0f) {
        m_nextBlinkIn = 2.4f + 2.6f * (0.5f + 0.5f * qSin(m_time * 0.37f));
        m_motion = {QStringLiteral("blink"), 0.0f, kBlinkDuration * 2.0f};
        m_motionActive = true;
        m_motionIsPlayable = false;
    }

    // 视线一阶低通缓动，避免跟随鼠标时头部抖动。
    const float k = qBound(0.0f, deltaSeconds * 8.0f, 1.0f);
    m_gaze += (m_gazeTarget - m_gaze) * k;

    if (m_motionActive) {
        m_motion.elapsed += deltaSeconds;
        if (m_motion.elapsed >= m_motion.duration) {
            const bool finishedPlayable = m_motionIsPlayable;
            m_motionActive = false;
            m_motion = {};
            m_motionIsPlayable = false;
            if (finishedPlayable && m_motionLoopEnabled) {
                playNextMotion();
            }
        }
    }
    // 默认开启循环时不等第一个眨眼周期，进场就依次播放全部动作。
    if (!m_motionActive && m_motionLoopEnabled) {
        playNextMotion();
    }
}

void PlaceholderRenderer::pointerMove(const QPointF &pos)
{
    if (!gazeEnabled()) {
        return;
    }
    // 与 Live2D 后端同一套映射口径、同一张档位表，两份实现必须一致。同样**不按半窗宽
    // 归一化** —— 窗口很小、鼠标几乎总在窗外，除半窗宽会让光标一离开就饱和到 ±1。
    const GazeTuning &tuning = kGazeTuning[clampGazeStrength(m_gazeStrength)];
    const float radiusX = qMax(m_width * tuning.radiusFactor, kGazeMinRadiusPx);
    const float radiusY = qMax(m_height * tuning.radiusFactor, kGazeMinRadiusPx);
    const float dx = static_cast<float>(pos.x()) - m_width * 0.5f;
    const float dy = static_cast<float>(pos.y()) - m_height * 0.5f;
    m_gazeTarget.setX(qBound(-1.0f, dx / radiusX, 1.0f));
    // 符号约定与 Live2D 后端统一：+1 表示光标在上方，而绘制坐标 y 向下，故取负号。
    m_gazeTarget.setY(qBound(-1.0f, -dy / radiusY * tuning.verticalScale, 1.0f));
    m_lastPointer = pos;
}

void PlaceholderRenderer::setGazeStrength(int strength)
{
    const int next = clampGazeStrength(strength);
    if (m_gazeStrength == next) {
        return;
    }
    const bool wasOn = gazeEnabled();
    m_gazeStrength = next;

    if (wasOn && !gazeEnabled()) {
        // 只清目标值，让低通缓动把 m_gaze 带回中心：直接抹零是瞬移。
        m_gazeTarget = QPointF(0.0, 0.0);
    } else if (gazeEnabled()) {
        // 开起来或切档位时按新半径立刻重算目标，否则要等下次鼠标移动才看出差别。
        if (m_lastPointer.x() >= 0.0) {
            pointerMove(m_lastPointer);
        }
    }
}

void PlaceholderRenderer::pointerClick(const QPointF &pos)
{
    Q_UNUSED(pos);
    m_motion = {QStringLiteral("tap"), 0.0f, kMotions[0].duration};
    m_motionActive = true;
    m_motionIsPlayable = true;
    m_currentMotionOrdinal = 1;
}

bool PlaceholderRenderer::playNextMotion()
{
    static const QStringList names = [] {
        QStringList l;
        for (const MotionDef &m : kMotions)
            l << QString::fromLatin1(m.name);
        return l;
    }();
    if (!m_ready || names.isEmpty())
        return false;
    // 游标是成员而非函数内 static：static 会被所有实例共用且重新初始化后不回零。
    const int i = m_motionIndex % names.size();
    m_motionIndex = (i + 1) % names.size();
    m_motion = {names.at(i), 0.0f, kMotions[i].duration};
    m_motionActive = true;
    m_motionIsPlayable = true;
    m_currentMotionOrdinal = i + 1;
    return true;
}

int PlaceholderRenderer::playableMotionCount() const
{
    return kMotionCount;
}

int PlaceholderRenderer::expressionCount() const
{
    return kExpressionCount;
}

bool PlaceholderRenderer::playNextExpression()
{
    if (!m_ready || kExpressionCount <= 0) {
        return false;
    }
    // 顺序轮转而非随机：随机抽样会连着撞同一个，看起来像没生效。
    m_expressionIndex = (m_expressionIndex + 1) % kExpressionCount;
    applog::log(applog::Level::Debug,
                   QStringLiteral("占位渲染器切表情：%1")
                       .arg(QString::fromLatin1(kExpressions[m_expressionIndex].name)),
                   QStringLiteral("Live2D"));
    return true;
}

void PlaceholderRenderer::shutdown()
{
    m_ready = false;
    m_motionActive = false;
    m_motionIsPlayable = false;
    m_currentMotionOrdinal = 0;
    unloadModel();
}

// 已加载模型纹理则居中绘制纹理+呼吸缩放+摆动，否则绘制默认虞美人花朵。
void PlaceholderRenderer::paint(QPainter *painter, const QSize &logicalSize)
{
    if (!m_ready || !painter)
        return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qreal w = logicalSize.width();
    const qreal h = logicalSize.height();
    const qreal t = m_time;

    qreal breath = 0.0, sway = 0.0, hop = 0.0, squash = 1.0, wave = 0.0, mouthOpen = 0.0;
    if (!m_paused) {
        breath = qSin(t * kBreathOmega);            // -1..1
        sway = qSin(t * kSwayOmega);                // -1..1
    }
    if (m_motionActive) {
        const float p = qBound(0.0f, m_motion.elapsed / qMax(0.001f, m_motion.duration), 1.0f);
        const float shape = qSin(p * float(M_PI));  // 0→1→0
        if (m_motion.name == QLatin1String("blink"))
            m_blinkLeft = shape;
        else if (m_motion.name == QLatin1String("tap"))
            squash = 1.0 - 0.06 * shape;
        else if (m_motion.name == QLatin1String("bounce"))
            hop = -18.0 * shape;
        else if (m_motion.name == QLatin1String("wave"))
            wave = shape;
        if (m_motion.name != QLatin1String("blink"))
            mouthOpen = shape;
    } else if (m_motion.name == QLatin1String("blink")) {
        m_blinkLeft = 0.0f;
    }

    const qreal cx = w * 0.5 + sway * w * 0.012 + m_gaze.x() * w * 0.01;
    const qreal baseY = h * 0.93 + hop;

    // 表情只影响脸(眼/嘴)与花瓣色温，不影响姿态 —— 两者分属不同通道才能组合出
    // 「一边蹦跳一边笑」。
    const ExpressionDef &ex = kExpressions[qBound(0, m_expressionIndex, kExpressionCount - 1)];

    if (!m_modelTexture.isNull()) {
        painter->save();
        const qreal texScale = (h * 0.75) / m_modelTexture.height();
        const qreal drawW = m_modelTexture.width() * texScale;
        const qreal drawH = m_modelTexture.height() * texScale;
        const qreal breathScale = 1.0 + breath * 0.015;
        const qreal finalW = drawW * squash * breathScale;
        const qreal finalH = drawH * breathScale;
        const qreal dx = cx - finalW * 0.5;
        const qreal dy = baseY - finalH;
        const QRectF dst(dx, dy, finalW, finalH);
        painter->drawImage(dst, m_modelTexture);
        painter->restore();
        painter->restore();
        return;
    }


    const qreal bodyH = h * 0.62 * (1.0 + breath * 0.012) * squash;
    const qreal headR = qMin(w, h) * 0.19;
    const qreal headCy = baseY - bodyH - headR * 0.35;

    {
        QPainterPath stem;
        const qreal shoulderY = headCy + headR * 0.75;
        const qreal sw = headR * 0.34;
        const qreal lw = headR * 0.72;
        stem.moveTo(cx - sw, shoulderY);
        stem.cubicTo(cx - sw * 1.25, shoulderY + bodyH * 0.35, cx - lw, baseY - bodyH * 0.25,
                     cx - lw * 0.55, baseY);
        stem.lineTo(cx + lw * 0.55, baseY);
        stem.cubicTo(cx + lw, baseY - bodyH * 0.25, cx + sw * 1.25, shoulderY + bodyH * 0.35,
                     cx + sw, shoulderY);
        stem.closeSubpath();
        QLinearGradient g(0, shoulderY, 0, baseY);
        g.setColorAt(0.0, QColor(0x2f, 0x6b, 0x46));
        g.setColorAt(1.0, QColor(0x1b, 0x3a, 0x27));
        painter->setPen(Qt::NoPen);
        painter->setBrush(g);
        painter->drawPath(stem);
    }

    for (int side = -1; side <= 1; side += 2) {
        painter->save();
        painter->translate(cx, baseY - bodyH * 0.32);
        painter->rotate(side * (16.0 + breath * 2.5 + wave * 6.0));
        QPainterPath leaf;
        const qreal ll = headR * 1.15 * side;
        leaf.moveTo(0, 0);
        leaf.quadTo(ll * 0.6, -headR * 0.42, ll, -headR * 0.06);
        leaf.quadTo(ll * 0.55, headR * 0.30, 0, 0);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x35, 0x77, 0x4d, 235));
        painter->drawPath(leaf);
        painter->restore();
    }

    // 花瓣「头发」：四片椭圆围绕头部，随视线整体微移，做出头发跟随感。
    {
        static const qreal angles[] = {200.0, 250.0, 290.0, 340.0};
        static const quint8 tones[] = {0xd8, 0xc4, 0xe6, 0xb2};
        for (int i = 0; i < 4; ++i) {
            painter->save();
            const qreal a = angles[i] + sway * 2.0 + breath * 1.2;
            painter->translate(cx + m_gaze.x() * headR * 0.10,
                               headCy - m_gaze.y() * headR * 0.08);
            painter->rotate(a);
            QPainterPath petal;
            const qreal pr = headR * (1.28 + 0.04 * breath);
            petal.addEllipse(QPointF(0, -pr * 0.62), pr * 0.46, pr * 0.66);
            QColor col(tones[i], 0x3a, 0x54);
            col.setAlpha(232);
            painter->setPen(QPen(QColor(0x6d, 0x1d, 0x30, 120), 1.0));
            painter->setBrush(col);
            painter->drawPath(petal);
            painter->restore();
        }
    }

    painter->setPen(QPen(QColor(0x5c, 0x22, 0x33, 140), 1.2));
    painter->setBrush(QColor(0xf6, 0xdf, 0xd2));
    painter->drawEllipse(QPointF(cx, headCy), headR, headR * 1.02);

    {
        QPainterPath fringe;
        fringe.moveTo(cx - headR * 0.98, headCy - headR * 0.10);
        fringe.quadTo(cx - headR * 0.45, headCy - headR * 1.35, cx + headR * 0.10,
                      headCy - headR * 0.90);
        fringe.quadTo(cx + headR * 0.55, headCy - headR * 0.62, cx + headR * 0.98,
                      headCy - headR * 0.02);
        fringe.quadTo(cx + headR * 0.30, headCy - headR * 0.52, cx - headR * 0.20,
                      headCy - headR * 0.34);
        fringe.quadTo(cx - headR * 0.66, headCy - headR * 0.20, cx - headR * 0.98,
                      headCy - headR * 0.10);
        fringe.closeSubpath();
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x35, 0x22, 0x2b));
        painter->drawPath(fringe);
    }

    // 睁眼高度按 1-blink 缩放，画成短弧即闭眼；表情再乘倍率或换成上弯弧(笑眼)。
    {
        const qreal eyeY = headCy + headR * 0.12;
        const qreal eyeDx = headR * 0.38;
        const qreal open = 1.0 - double(m_blinkLeft);
        const qreal eyeH = qMax(0.6, headR * 0.22 * open * double(ex.eyeOpen));
        const qreal eyeW = headR * 0.16;
        // 眨眼过半时一律退化成弧线，避免「笑眼 + 眨眼」互相打架。
        // 眨眼过半时退化成弧线，避免「笑眼 + 眨眼」互相打架。
        const bool asArc = ex.arcEyes && open > 0.55;
        painter->setPen(Qt::NoPen);
        for (int side = -1; side <= 1; side += 2) {
            const QPointF c(cx + side * eyeDx + m_gaze.x() * headR * 0.05,
                            eyeY - m_gaze.y() * headR * 0.04);
            if (asArc) {
                QPainterPath arc;
                arc.moveTo(c.x() - eyeW * 1.35, c.y() + eyeH * 0.9);
                arc.quadTo(c.x(), c.y() - eyeH * 1.5, c.x() + eyeW * 1.35, c.y() + eyeH * 0.9);
                painter->setBrush(Qt::NoBrush);
                painter->setPen(QPen(QColor(0x2b, 0x1c, 0x24), qMax(1.0, headR * 0.055),
                                     Qt::SolidLine, Qt::RoundCap));
                painter->drawPath(arc);
                painter->setPen(Qt::NoPen);
                continue;
            }
            painter->setBrush(QColor(0x2b, 0x1c, 0x24));
            painter->drawEllipse(c, eyeW, eyeH);
            if (open > 0.35) { // 高光只在睁眼时画
                painter->setBrush(QColor(255, 255, 255, 220));
                painter->drawEllipse(c + QPointF(-eyeW * 0.3, -eyeH * 0.35), eyeW * 0.30,
                                     eyeH * 0.28);
            }
        }
    }

    {
        const qreal mouthY = headCy + headR * 0.55;
        QPainterPath mouth;
        const qreal mw = headR * 0.22;
        const qreal curve = double(ex.mouthCurve);
        const qreal drop = (headR * 0.10 + headR * 0.12 * (mouthOpen + double(ex.mouthOpen)))
                           * curve;
        mouth.moveTo(cx - mw, mouthY);
        mouth.quadTo(cx, mouthY + drop, cx + mw, mouthY);
        mouth.quadTo(cx, mouthY + drop * 0.35, cx - mw, mouthY);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0xa8, 0x33, 0x4a));
        painter->drawPath(mouth);
    }

    {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0xf2, 0xc7, 0x4b));
        const QPointF center(cx, headCy - headR * 0.92);
        painter->drawEllipse(center, headR * 0.13, headR * 0.13);
        painter->setBrush(QColor(0x3a, 0x24, 0x1c));
        painter->drawEllipse(center, headR * 0.075, headR * 0.075);
    }

    painter->restore();
}

} // namespace kanban

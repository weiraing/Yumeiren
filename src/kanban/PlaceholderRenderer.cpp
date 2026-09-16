#include "kanban/PlaceholderRenderer.h"

#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include "core/Diagnostics.h"

namespace kanban {

namespace {

// 呼吸与摆动的角速度(弧度/秒)。取慢值：壁纸旁的常驻角色动作幅度过大会抢注意力。
constexpr float kBreathOmega = 1.15f;
constexpr float kSwayOmega = 0.55f;
constexpr float kBlinkDuration = 0.13f;

// 一次性动作表：占位渲染器没有 motion3.json，动作是写死的三形变。
// 名称与 Live2D 侧的 motion 语义对齐，方便接入后逐项替换。
struct MotionDef {
    const char *name;
    float duration;
};
const MotionDef kMotions[] = {
    {"tap", 0.6f},
    {"bounce", 0.9f},
    {"wave", 1.2f},
};

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
    videodiag::log(videodiag::Level::Info,
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
    // 占位后端不解析模型，只记名字用于界面展示；校验由 KanbanModelManager 做过。
    m_modelName = QFileInfo(modelJsonPath).completeBaseName();
    if (m_modelName.endsWith(QStringLiteral(".model3")))
        m_modelName.chop(7);
    if (outError)
        outError->clear();
    return true;
}

void PlaceholderRenderer::unloadModel()
{
    m_modelName.clear();
    m_motionActive = false;
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

    // 眨眼：到点触发一次短闭眼，其余时间按剩余时长归零。
    m_nextBlinkIn -= deltaSeconds;
    if (m_nextBlinkIn <= 0.0f) {
        m_nextBlinkIn = 2.4f + 2.6f * (0.5f + 0.5f * qSin(m_time * 0.37f));
        m_motion = {QStringLiteral("blink"), 0.0f, kBlinkDuration * 2.0f};
        m_motionActive = true;
    }

    // 视线缓动：一阶低通，避免跟随鼠标时头部抖动。
    const float k = qBound(0.0f, deltaSeconds * 8.0f, 1.0f);
    m_gaze += (m_gazeTarget - m_gaze) * k;

    if (m_motionActive) {
        m_motion.elapsed += deltaSeconds;
        if (m_motion.elapsed >= m_motion.duration) {
            m_motionActive = false;
            m_motion = {};
        }
    }
}

void PlaceholderRenderer::pointerMove(const QPointF &pos)
{
    // 窗口坐标 -> 归一化视线偏移：左上 (-1,-1)，右下 (1,1)。
    const float halfW = qMax(1.0f, m_width * 0.5f);
    const float halfH = qMax(1.0f, m_height * 0.5f);
    m_gazeTarget.setX(qBound(-1.0f, float(pos.x() - halfW) / halfW, 1.0f));
    m_gazeTarget.setY(qBound(-1.0f, float(pos.y() - halfH) / halfH, 1.0f));
}

void PlaceholderRenderer::pointerClick(const QPointF &pos)
{
    Q_UNUSED(pos);
    m_motion = {QStringLiteral("tap"), 0.0f, kMotions[0].duration};
    m_motionActive = true;
}

bool PlaceholderRenderer::playNextMotion()
{
    static const QStringList names = [] {
        QStringList l;
        for (const MotionDef &m : kMotions)
            l << QString::fromLatin1(m.name);
        return l;
    }();
    static int cursor = 0;
    if (!m_ready || names.isEmpty())
        return false;
    const int i = cursor % names.size();
    ++cursor;
    m_motion = {names.at(i), 0.0f, kMotions[i].duration};
    m_motionActive = true;
    return true;
}

void PlaceholderRenderer::shutdown()
{
    m_ready = false;
    m_motionActive = false;
    unloadModel();
}

// —— 绘制 ——
//
// 构图：一朵虞美人(罂粟科)拟人形象站在画面下半部，四片花瓣是头发，
// 深色花心与雄蕊圈化成头饰，叶子当裙摆。全部矢量路径。
void PlaceholderRenderer::paint(QPainter *painter, const QSize &logicalSize)
{
    if (!m_ready || !painter)
        return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const qreal w = logicalSize.width();
    const qreal h = logicalSize.height();
    const qreal t = m_time;

    // 形变量：呼吸缩放、左右摆动、动作叠加的纵向位移。
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
        // 说话感的开口度：除眨眼外的一次性动作都张一下嘴。
        if (m_motion.name != QLatin1String("blink"))
            mouthOpen = shape;
    } else if (m_motion.name == QLatin1String("blink")) {
        m_blinkLeft = 0.0f;
    }

    const qreal cx = w * 0.5 + sway * w * 0.012 + m_gaze.x() * w * 0.01;
    const qreal baseY = h * 0.93 + hop;
    const qreal bodyH = h * 0.62 * (1.0 + breath * 0.012) * squash;
    const qreal headR = qMin(w, h) * 0.19;
    const qreal headCy = baseY - bodyH - headR * 0.35;

    // 茎(身体)：上窄下宽的贝塞尔轮廓
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

    // 叶子：左右各一片，随呼吸轻微张合
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

    // 花瓣「头发」：四片椭圆围绕头部，随视线整体微移，做出头发跟随感
    {
        static const qreal angles[] = {200.0, 250.0, 290.0, 340.0};
        static const quint8 tones[] = {0xd8, 0xc4, 0xe6, 0xb2};
        for (int i = 0; i < 4; ++i) {
            painter->save();
            const qreal a = angles[i] + sway * 2.0 + breath * 1.2;
            painter->translate(cx + m_gaze.x() * headR * 0.10,
                               headCy + m_gaze.y() * headR * 0.08);
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

    // 头部
    painter->setPen(QPen(QColor(0x5c, 0x22, 0x33, 140), 1.2));
    painter->setBrush(QColor(0xf6, 0xdf, 0xd2));
    painter->drawEllipse(QPointF(cx, headCy), headR, headR * 1.02);

    // 刘海
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

    // 眼睛：睁开高度按 1-blink 缩放，画成短弧即得「闭眼」效果
    {
        const qreal eyeY = headCy + headR * 0.12;
        const qreal eyeDx = headR * 0.38;
        const qreal open = 1.0 - double(m_blinkLeft);
        const qreal eyeH = qMax(0.6, headR * 0.22 * open);
        const qreal eyeW = headR * 0.16;
        painter->setPen(Qt::NoPen);
        for (int side = -1; side <= 1; side += 2) {
            const QPointF c(cx + side * eyeDx + m_gaze.x() * headR * 0.05,
                            eyeY + m_gaze.y() * headR * 0.04);
            painter->setBrush(QColor(0x2b, 0x1c, 0x24));
            painter->drawEllipse(c, eyeW, eyeH);
            if (open > 0.35) { // 高光只在睁眼时画
                painter->setBrush(QColor(255, 255, 255, 220));
                painter->drawEllipse(c + QPointF(-eyeW * 0.3, -eyeH * 0.35), eyeW * 0.30,
                                     eyeH * 0.28);
            }
        }
    }

    // 嘴：一条小弧，tap 动作时张开
    {
        const qreal mouthY = headCy + headR * 0.55;
        QPainterPath mouth;
        const qreal mw = headR * 0.22;
        const qreal drop = headR * 0.10 + headR * 0.12 * mouthOpen;
        mouth.moveTo(cx - mw, mouthY);
        mouth.quadTo(cx, mouthY + drop, cx + mw, mouthY);
        mouth.quadTo(cx, mouthY + drop * 0.35, cx - mw, mouthY);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0xa8, 0x33, 0x4a));
        painter->drawPath(mouth);
    }

    // 花蕊头饰：头部上方一小圈黄点，把「虞美人」的身份点亮出来
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

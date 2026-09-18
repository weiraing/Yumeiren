#include "ui/TooltipStyle.h"

#include <QApplication>
#include <QFontMetrics>
#include <QString>
#include <QStringList>
#include <QVector>

namespace {

// 浮层横向装饰 = 2 * (QTipLabel margin 1 + QSS padding 10 + border 1)。
constexpr int kChromeWidth = 24;

// 与 QSS 的 `font-size: 12px` 对齐；字号写死，折行结果才不会随 DPI
// 与系统默认字号漂移(样式表里的 px 是逻辑像素，这里也是)。
QFont tipFont()
{
    QFont f = QApplication::font();
    f.setPixelSize(12);
    return f;
}

// 中日韩文字与全角标点：任意两侧都可以断行。
bool isCjk(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x2E80 && u <= 0x9FFF)    // 部首/假名/汉字
        || (u >= 0xF900 && u <= 0xFAFF)    // 兼容汉字
        || (u >= 0xFE30 && u <= 0xFE4F)    // 兼容标点
        || (u >= 0x3000 && u <= 0x303F)    // CJK 符号
        || (u >= 0xFF00 && u <= 0xFF65);   // 全角形式
}

// 断行点优先级：标点后(含右引号/右括号)最自然，其次是空白，最后才是
// 汉字之间的任意位置。
bool isPreferredAfter(QChar c)
{
    // 全角标点直接写在表里：这些字符后面断行读起来最自然。
    static const QString kTail =
        QStringLiteral("，。；：、！？）》”’%;,.)]}>");
    return kTail.contains(c);
}

// 能否在 i 之前断行。核心约束：纯 ASCII 连续段(路径、文件名、"1080p30"、
// "PNG/JPG")内部绝不拆，否则提示里会出现半截单词，比横条更难读。
bool canBreakBefore(const QString &s, int i)
{
    if (i <= 0 || i >= s.size())
        return false;
    const QChar prev = s.at(i - 1);
    const QChar next = s.at(i);
    if (prev.isSpace() || next.isSpace())
        return true;
    return isCjk(prev) || isCjk(next);
}

} // namespace

namespace tooltipstyle {

QString format(const QString &text)
{
    if (text.isEmpty())
        return text;

    const QFontMetrics fm(tipFont());
    const int maxWidth = kMaxTipWidth - kChromeWidth;
    // 注：QFontMetrics::horizontalAdvance 对含 '\n' 的串返回的是**各行宽度之和**
    // （2026-09-18 实测：两行 36+108 的串报 144），不是"最宽一行"。所以多行文本
    // 基本都会走进下面的折行分支 —— 无害，每行各自量一次、放得下就原样成一行，
    // 结果与不折时逐字相同，只是白算一遍。
    if (maxWidth <= 0 || fm.horizontalAdvance(text) <= maxWidth)
        return text;

    // 折好的每一行，最后统一 join('\n')。
    //
    // 2026-09-18 修：原来是「算好切点 → result += piece」，而换行只在源文本自带的
    // '\n' 处补过一次 —— 于是**同一源行里折出来的第 2 段起被原样拼回上一段后面**，
    // 等于没折：830px 的长句进去、830px 一行出来，一个 '\n' 都没有。
    // 之所以长期没暴露，是因为既有提示每一行都短于 maxWidth，从没走进折行分支。
    // 改成收集「行」再 join，顺带把「源文本里的空行」也自然保留下来。
    QStringList outLines;
    int pos = 0;
    while (pos <= text.size()) {
        const int nextNl = text.indexOf(QChar('\n'), pos);
        const int end = nextNl < 0 ? text.size() : nextNl;

        const QString line = text.mid(pos, end - pos);
        // 逐字累加宽度，省掉反复 mid() 测量
        QVector<int> advance(line.size() + 1, 0);
        for (int i = 0; i < line.size(); ++i)
            advance[i + 1] = advance[i] + fm.horizontalAdvance(line.at(i));

        if (line.isEmpty()) {
            outLines << QString(); // 源文本里的空行：原样占一行
        } else {
            int start = 0;
            while (start < line.size()) {
                int i = start;
                int allowed = -1;   // 最近的可行断行点
                int preferred = -1; // 最近的标点后断行点
                while (i < line.size() && advance[i + 1] - advance[start] <= maxWidth) {
                    if (i > start && canBreakBefore(line, i + 1)) {
                        allowed = i + 1;
                        if (isPreferredAfter(line.at(i)))
                            preferred = i + 1;
                    }
                    ++i;
                }

                int cut = i; // 默认：整行剩余都能放下，或遇到不可断的长串
                if (i < line.size()) {
                    // 溢出：优先落在标点后，其次任意可行点；都不存在时硬切，
                    // 硬切只在"一个不可断的 ASCII 长串本身就超宽"时发生。
                    cut = preferred > start ? preferred : (allowed > start ? allowed : i);
                    if (cut <= start)
                        cut = i + 1;
                }

                QString piece = line.mid(start, cut - start);
                while (!piece.isEmpty() && piece.at(piece.size() - 1).isSpace())
                    piece.chop(1);
                outLines << piece;
                start = cut;
            }
        }

        if (nextNl < 0)
            break;
        pos = nextNl + 1;
    }
    return outLines.join(QChar('\n'));
}

} // namespace tooltipstyle

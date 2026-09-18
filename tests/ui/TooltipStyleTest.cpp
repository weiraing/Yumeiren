// 悬浮提示折行的回归测试。
//
// 存在的理由：2026-09-18 发现 tooltipstyle::format() **从来没真的折过行** ——
// 它算好切点之后把片段又拼回上一段后面，换行符只在源文本自带的 '\n' 处补过一次，
// 于是 830px 的长句进去、830px 一行出来，一个换行符都没有。之所以长期没暴露，
// 是因为既有提示每一行都短于上限，从没走进折行分支。
//
// 所以第一条断言是「长句折行后必须出现换行符」——**光量宽度是量不出这个 BUG 的**：
// 拼接回去的串和折好的串，总宽度、字符集合都一样，只有 '\n' 的个数不同。
#include "ui/TooltipStyle.h"

#include <QApplication>
#include <QDebug>
#include <QFontMetrics>
#include <QStringList>

namespace {

// 与 TooltipStyle.cpp 的 kChromeWidth 对齐（浮层横向装饰：margin 1 + padding 10
// + border 1，两侧各一份）。
constexpr int kChromeWidth = 24;

// 与 format() 内部用同一套字体，否则量出来的宽度和它的判据对不上。
QFontMetrics tipMetrics()
{
    QFont f = QApplication::font();
    f.setPixelSize(12);
    return QFontMetrics(f);
}

int widestLine(const QString &text, const QFontMetrics &fm)
{
    int widest = 0;
    const QStringList lines = text.split(QChar('\n'));
    for (const QString &line : lines)
        widest = qMax(widest, fm.horizontalAdvance(line));
    return widest;
}

} // namespace

int main(int argc, char *argv[])
{
    // format() 里读 QApplication::font()，所以必须是 QApplication。
    QApplication application(argc, argv);

    int failures = 0;
    int checks = 0;
    const auto check = [&](bool condition, const char *message) {
        ++checks;
        if (!condition) {
            ++failures;
            qCritical() << message;
        }
    };

    const QFontMetrics fm = tipMetrics();
    const int limit = tooltipstyle::kMaxTipWidth - kChromeWidth;

    // 够长的中文长句：单行远超上限，必须被折。
    const QString longText = QStringLiteral(
        "这是一句刻意写得很长的说明文字，用来检查折行函数是不是真的插入了换行符，"
        "而不是把切开的片段又拼接回去——后者会让提示浮层依旧拉成一条横条。");

    // —— 1. 本来就放得下的短文本必须原样返回，不能凭空多出换行 ——
    {
        const QString shortText = QStringLiteral("重新扫描模型目录，并重新生成全部预览图");
        check(fm.horizontalAdvance(shortText) <= limit,
              "Fixture must fit, otherwise this case tests nothing");
        check(tooltipstyle::format(shortText) == shortText,
              "Text that already fits must be returned unchanged");
    }

    // —— 2. 超宽的长句必须真的插入换行（本次 BUG 的回归钉子）——
    {
        check(fm.horizontalAdvance(longText) > limit,
              "Fixture must be too wide, otherwise this case tests nothing");
        const QString wrapped = tooltipstyle::format(longText);
        check(wrapped.count(QChar('\n')) > 0,
              "A too-wide line must gain line breaks; pasting the pieces back is the bug");
        QString stripped = wrapped;
        stripped.remove(QChar('\n'));
        check(stripped == longText,
              "Wrapping must not drop or reorder characters");
    }

    // —— 3. 折完每一行都不超限（单行输入）——
    {
        const QString wrapped = tooltipstyle::format(longText);
        check(widestLine(wrapped, fm) <= limit,
              "No wrapped line may exceed the limit");
    }

    // —— 4. 源文本里已有的换行与**空行**必须原样保留 ——
    // 这条要走折行分支才有意义（第 3 行超宽），否则会被「整体不超宽就原样返回」
    // 的早退挡住，测了个寂寞。
    {
        const QString text =
            QStringLiteral("模型目录：<程序目录>\\data\\models\n\n") + longText;
        check(fm.horizontalAdvance(text) > limit,
              "Fixture must be too wide so the wrapping path really runs");
        const QStringList lines = tooltipstyle::format(text).split(QChar('\n'));
        check(lines.size() >= 4,
              "Wrapping must keep the source line structure (3 source lines + wraps)");
        check(lines.value(0) == QStringLiteral("模型目录：<程序目录>\\data\\models"),
              "A source line that fits must come through untouched");
        check(lines.value(1).isEmpty(),
              "A blank source line must survive wrapping instead of being swallowed");
    }

    // —— 5. 窄于上限的 ASCII 连续段（路径）绝不能被从中间拆开 ——
    {
        const QString path = QStringLiteral("C:/Yumeiren/data/models/Haru");
        const QString text = QStringLiteral("模型放在 ") + path
            + QStringLiteral(" 这个目录下面，每个模型一个子目录，"
                             "里面放它的 model3.json 与贴图、动作文件。");
        check(fm.horizontalAdvance(text) > limit,
              "Fixture must be too wide so the wrapping path really runs");
        const QString wrapped = tooltipstyle::format(text);
        check(wrapped.contains(path),
              "An ASCII run narrower than the limit must never be split mid-way");
        check(widestLine(wrapped, fm) <= limit,
              "No wrapped line may exceed the limit (multi-line input)");
    }

    // —— 6. 空串与纯换行不能崩、也不能被改写 ——
    {
        check(tooltipstyle::format(QString()).isEmpty(),
              "Empty input must stay empty");
        const QString blank = QStringLiteral("\n\n");
        check(tooltipstyle::format(blank) == blank,
              "A pure line-break input must stay as-is");
    }

    if (failures == 0)
        qInfo() << "TooltipStyle:" << checks << "checks passed";
    return failures == 0 ? 0 : 1;
}

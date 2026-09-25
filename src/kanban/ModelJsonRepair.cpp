#include "kanban/ModelJsonRepair.h"

#include <QChar>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringConverter>

#include <string>

#include <windows.h>

namespace kanban {

namespace {

// 非 UTF-8 时按这些代码页依次试。**顺序有意义**：本仓库的素材以日系 galgame 为主，
// 932(Shift-JIS) 命中率最高，先试它；936(GBK) 兜中文素材。
// 两个都解不干净就整个放弃 —— 拿乱码去装模型比装不上更难查。
constexpr UINT kFallbackCodePages[] = {932, 936};

QString codePageName(UINT codePage)
{
    switch (codePage) {
    case 932:
        return QStringLiteral("Shift-JIS");
    case 936:
        return QStringLiteral("GBK");
    default:
        return QString::number(codePage);
    }
}

// 按代码页解码。**必须带 MB_ERR_INVALID_CHARS**：不带的话 Windows 会把解不出来的字节
// 悄悄换成 '?'，于是「解成功」了但内容全是问号，比直接失败更难定位。
bool decodeCodePage(const QByteArray &bytes, UINT codePage, QString *out)
{
    const char *src = bytes.constData();
    const int length = int(bytes.size());
    const int needed =
        ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, src, length, nullptr, 0);
    if (needed <= 0)
        return false;

    std::wstring wide(size_t(needed), L'\0');
    if (::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, src, length, wide.data(), needed)
        != needed) {
        return false;
    }
    *out = QString::fromWCharArray(wide.data(), qsizetype(needed));
    return true;
}

// JSON 只认 ' ' / '\t' / '\n' / '\r' 这四种空白。
bool isJsonWhitespace(QChar c)
{
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r';
}

// 「长得像空格、但 JSON 不认」的字符。这类字符混进来会让解析器直接报语法错，
// 而人眼在编辑器里完全看不出来 —— 日系素材用 IME 编辑时最容易带进全角空格。
// 只列实测见过与 Unicode 里同类的；宁可少列，也不要漏判成「正常空白」而放过去。
bool isLookalikeSpace(QChar c)
{
    switch (c.unicode()) {
    case 0x00A0: // 不换行空格
    case 0x1680:
    case 0x2000:
    case 0x2001:
    case 0x2002:
    case 0x2003:
    case 0x2004:
    case 0x2005:
    case 0x2006:
    case 0x2007:
    case 0x2008:
    case 0x2009:
    case 0x200A:
    case 0x2028: // 行分隔符
    case 0x2029: // 段分隔符
    case 0x202F:
    case 0x205F:
    case 0x3000: // 全角空格 —— 实测素材里最常见的一个
    case 0xFEFF: // 零宽不换行空格（BOM 跑到文件中间）
        return true;
    default:
        return false;
    }
}

/// 扫描文本并就地修掉「非法空白」与「尾随逗号」，回改动次数。
struct ScanCounts
{
    int lookalikeSpaces = 0;
    int trailingCommas = 0;
};

// 逐字符扫，全程跟踪「是否在字符串字面量里」与转义状态 —— 这是本函数唯一要紧的事：
// 字符串里的 `,` 和全角空格都是**内容**，改了就是篡改素材。
//
// 尾随逗号用**前瞻**判，不搞「延迟提交」：曾经把逗号先攒着、等下一个有效字符再决定，
// 结果逗号被推到了空白**后面**（`0.5, "x"` 变成 `0.5 ,"x"`）。对 QJsonDocument 毫无影响，
// 对 Cubism SDK 却是致命的 —— 它的 ParseNumeric 遇到数字后的空格就报
// "non-numeric charactor found"（见 fixNumberTerminators 的说明）。
// 前瞻只需要往后看几个空白，代价可以忽略，换来的是**输出与输入逐字符同序**。
ScanCounts normalize(QString *text)
{
    ScanCounts counts;
    const QString &in = *text;
    const int n = in.size();
    QString out;
    out.reserve(in.size());

    bool inString = false;
    bool escaped = false;

    for (int i = 0; i < n; ++i) {
        const QChar c = in.at(i);
        if (inString) {
            out.append(c);
            if (escaped)
                escaped = false;
            else if (c == u'\\')
                escaped = true;
            else if (c == u'"')
                inString = false;
            continue;
        }

        if (c == u'"') {
            inString = true;
            out.append(c);
            continue;
        }
        if (c == u',') {
            // 跳过空白后若是收尾括号，它就是尾随逗号，丢掉。跳过的一定是空白，
            // 所以那个括号必然也在字符串之外。
            int k = i + 1;
            while (k < n && isJsonWhitespace(in.at(k)))
                ++k;
            if (k < n && (in.at(k) == u'}' || in.at(k) == u']')) {
                ++counts.trailingCommas;
                continue;
            }
            out.append(c);
            continue;
        }
        if (isJsonWhitespace(c)) {
            out.append(c);
            continue;
        }
        if (isLookalikeSpace(c)) {
            out.append(u' ');
            ++counts.lookalikeSpaces;
            continue;
        }
        out.append(c);
    }

    *text = out;
    return counts;
}

bool isNumberChar(QChar c)
{
    const ushort u = c.unicode();
    return (u >= '0' && u <= '9') || c == u'.' || c == u'-' || c == u'+' || c == u'e'
           || c == u'E';
}

// 给「数字 token 后面跟了 SDK 不认的字符」的地方补一个换行。
//
// 这条修补看着莫名其妙，但它是**必须的**：Cubism SDK 自带的 JSON 解析器
// (third_party/cubism/Framework/src/Utils/CubismJson.cpp 的 ParseNumeric) 只在
// `\n` 与 `,` 两处结束一个数字（`\r` 也只是被跳过、不结束），**其余字符一律判成
// "non-numeric charactor found" 并让整份 json 解析失败 —— 空格也算**。于是
//   · `{"FadeOutTime":0}`      —— `}` 紧贴数字
//   · `{"FadeInTime":0.5 , ...}` —— 数字后面有空格
// 这两种用 QJsonDocument 读都完全合法，到了 SDK 手里却是「不是合法的 model3.json」，
// 模型根本装不上。实测用户的 [ryoheyLab.] 模型有 66 处这种写法。
//
// 判据：数字 token 结束后，**紧跟的第一个字符**若不是 `\n` 或 `,`，就在数字后面插一个
// 换行。插换行对 JSON 语义零影响，而 SDK 恰好认它。
int fixNumberTerminators(QString *text)
{
    const QString &in = *text;
    const int n = in.size();
    QString out;
    out.reserve(in.size() + 64);
    int fixes = 0;

    bool inString = false;
    bool escaped = false;
    int i = 0;
    while (i < n) {
        const QChar c = in.at(i);
        if (inString) {
            out.append(c);
            if (escaped)
                escaped = false;
            else if (c == u'\\')
                escaped = true;
            else if (c == u'"')
                inString = false;
            ++i;
            continue;
        }
        if (c == u'"') {
            inString = true;
            out.append(c);
            ++i;
            continue;
        }
        // 字符串之外，数字与 `-`/`.`/`+`/`e`/`E` 只可能出现在数字 token 里。
        if (isNumberChar(c)) {
            int j = i;
            while (j < n && isNumberChar(in.at(j)))
                out.append(in.at(j++));
            // 紧跟的字符必须是 SDK 认的结束符：`\n` 或 `,`。`\r` 只在后面还有 `\n` 时
            // 才算安全（ParseNumeric 会把 `\r` 跳过、由 `\n` 结束数字）——
            // 否则 CRLF 素材会被误判成「要修补」，白白重写一遍字节。
            bool terminated = false;
            if (j < n) {
                const QChar next = in.at(j);
                terminated = (next == u'\n' || next == u',')
                             || (next == u'\r' && j + 1 < n && in.at(j + 1) == u'\n');
            }
            if (j < n && !terminated) {
                out.append(u'\n');
                ++fixes;
            }
            i = j;
            continue;
        }
        out.append(c);
        ++i;
    }

    *text = out;
    return fixes;
}

} // namespace

JsonRepair repairJson(QByteArray *buffer)
{
    JsonRepair result;
    if (!buffer || buffer->isEmpty())
        return result;

    QStringList &notes = result.notes;

    // BOM 要单独判：QStringDecoder 的 ConvertInitialBom 会把它吃掉，但那样「文本没变」
    // 就说明不了「字节没变」，我们会漏掉「只多一个 BOM」这种文件（它同样解析不了）。
    if (buffer->startsWith(QByteArray::fromHex("efbbbf"))) {
        notes << QStringLiteral("去掉开头的 UTF-8 BOM");
        result.touched = true;
    }

    QString text;
    QStringDecoder utf8(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
    text = utf8.decode(*buffer);
    if (utf8.hasError()) {
        bool decoded = false;
        for (const UINT codePage : kFallbackCodePages) {
            if (decodeCodePage(*buffer, codePage, &text)) {
                notes << QStringLiteral("不是 UTF-8，按 %1 重新解码").arg(codePageName(codePage));
                result.touched = true;
                decoded = true;
                break;
            }
        }
        if (!decoded) {
            // 字节流原样留着，但把「解不出来」记进 notes：上层据此把报错写成
            // 「已尝试按 Shift-JIS / GBK 解码」——用户才知道该去转码，而不是去改语法。
            notes << QStringLiteral("不是 UTF-8，且按 %1 都解不出来")
                         .arg(QStringLiteral("Shift-JIS / GBK"));
            return result;
        }
    }

    const ScanCounts counts = normalize(&text);
    if (counts.lookalikeSpaces > 0) {
        notes << QStringLiteral("字符串外的非法空白 %1 处换成普通空格")
                     .arg(counts.lookalikeSpaces);
        result.touched = true;
    }
    if (counts.trailingCommas > 0) {
        notes << QStringLiteral("删掉尾随逗号 %1 处").arg(counts.trailingCommas);
        result.touched = true;
    }

    const int numberFixes = fixNumberTerminators(&text);
    if (numberFixes > 0) {
        notes << QStringLiteral("数字后补换行 %1 处（SDK 的数字解析只认换行/逗号作结束）")
                     .arg(numberFixes);
        result.touched = true;
    }

    // 没改动就**原样还回去**：正常素材（本机实测 1392 个里 1386 个）不该被无谓地
    // 重写一遍字节，多出来的编码往返只会引入新风险。
    if (result.touched)
        *buffer = text.toUtf8();
    return result;
}

namespace {

// 浅层找候选贴图，返回的是**相对模型目录**的路径（'/' 分隔，与 model3.json 里的写法一致）。
// 先 <模型目录>/textures/*.png，没有再看 <模型目录>/*.png。
// **刻意不下钻**：深一点的目录里往往同时躺着换装变体的贴图，下钻等于替用户乱猜。
QStringList textureCandidates(const QString &modelDir)
{
    const QDir root(modelDir);
    const QStringList filters{QStringLiteral("*.png")};
    const QString subdir = QStringLiteral("textures");
    const QStringList inSubdir =
        QDir(root.absoluteFilePath(subdir)).entryList(filters, QDir::Files, QDir::Name);
    if (!inSubdir.isEmpty()) {
        QStringList out;
        out.reserve(inSubdir.size());
        for (const QString &name : inSubdir)
            out << subdir + QLatin1Char('/') + name;
        return out;
    }
    return root.entryList(filters, QDir::Files, QDir::Name);
}

} // namespace

TextureCompletion completeTextures(QByteArray *buffer, const QString &modelDir)
{
    TextureCompletion result;
    if (!buffer || buffer->isEmpty() || modelDir.isEmpty())
        return result;

    const QJsonDocument doc = QJsonDocument::fromJson(*buffer);
    if (!doc.isObject())
        return result; // 连 json 都不是：调用方自己会报「解析失败」，这里不抢着解释

    QJsonObject root = doc.object();
    QJsonObject refs = root.value(QStringLiteral("FileReferences")).toObject();
    const QJsonArray declared = refs.value(QStringLiteral("Textures")).toArray();

    // 声明了就**照单全收**，哪怕文件缺失 —— 那是「素材缺件」，由调用方按缺件报错。
    // 这里若擅自换成推断结果，就会把「贴图丢了」悄悄粉饰成「贴图还在」。
    if (!declared.isEmpty()) {
        for (const QJsonValue &v : declared) {
            const QString rel = v.toString();
            if (!rel.isEmpty())
                result.textures << rel;
        }
        return result;
    }

    const QStringList candidates = textureCandidates(modelDir);
    if (candidates.isEmpty()) {
        result.note = QStringLiteral("贴图列表为空，模型目录里也找不到 png");
        return result;
    }
    if (candidates.size() > 1) {
        result.note = QStringLiteral("贴图列表为空，目录里有 %1 张候选 png(%2)，无法判断用哪张")
                          .arg(candidates.size())
                          .arg(candidates.mid(0, 3).join(QStringLiteral(", ")));
        return result;
    }

    const QString rel = candidates.first();
    QJsonArray injected;
    injected.append(rel);
    refs.insert(QStringLiteral("Textures"), injected);
    root.insert(QStringLiteral("FileReferences"), refs);
    // ⚠️ **必须用 Indented，不能用 Compact**。Cubism SDK 自带的 JSON 解析器
    // (third_party/cubism/.../Utils/CubismJson.cpp) 的 ParseNumeric 只在 `\n`、`,`、`\r`
    // 三处结束一个数字，别的字符一律判成 "non-numeric charactor found" 直接解析失败。
    // 紧凑写法里 `"Version":3}` 的 `}` 正好撞上这一条 —— 结果是 SDK 认为整份 json 非法
    // (CubismModelSettingJson::IsValid() == false)，模型报「不是合法的 model3.json」。
    // Indented 让每个值后面都跟一个换行，数字就落在合法终止符上了。
    *buffer = QJsonDocument(root).toJson(QJsonDocument::Indented);

    result.textures << rel;
    result.inferred = true;
    result.note = QStringLiteral("贴图列表为空，按目录里唯一的 png 补上: %1").arg(rel);
    return result;
}

PreparedSetting prepareSettingBytes(const QString &jsonPath)
{
    PreparedSetting out;

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        out.readError = QStringLiteral("无法读取 %1").arg(QFileInfo(jsonPath).fileName());
        return out;
    }
    out.bytes = file.readAll();
    file.close();

    // 顺序是契约的一部分：先补排版（repairJson），再补数据（completeTextures）。
    // completeTextures 在推断成功时会把自己的结果**序列化回整个 buffer**，所以它必须
    // 看到已经修好的排版；反过来先补贴图、后修排版的话，repairJson 会去动一份刚被
    // QJsonDocument 重新生成过的字节，白白多做一轮全文遍历。
    out.repair = repairJson(&out.bytes);
    out.tex = completeTextures(&out.bytes, QFileInfo(jsonPath).absolutePath());
    out.ok = true;
    return out;
}

} // namespace kanban

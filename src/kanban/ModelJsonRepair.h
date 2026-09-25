/**
 * @file ModelJsonRepair.h
 * @brief 把用户素材里「语义正确、写法不合规」的 JSON 修成标准解析器能读的形态。
 */
#ifndef MODELJSONREPAIR_H
#define MODELJSONREPAIR_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace kanban {

/**
 * @brief JSON 修补结果。
 *
 * @note touched 为 false 时，传入的缓冲区**一个字节都没被碰过** —— 调用方据此决定
 *       要不要把修好的字节交回去。
 */
struct JsonRepair
{
    bool touched = false;  ///< 缓冲区是否被改写过
    QStringList notes;     ///< 每项一句「改了什么」，直接给日志用
};

/**
 * @brief 修补一段 JSON 字节流，使其能被 QJsonDocument 与 Cubism SDK 解析。
 *
 * 只做四类**不改语义**的修补，且全部在字符串字面量之外进行：
 *   1. 剥掉 UTF-8 BOM（Qt 与 SDK 的解析器都不认它）；
 *   2. 不是合法 UTF-8 时按常见日系/中文代码页重解（实测素材里有 Shift-JIS 的 model3.json）；
 *   3. 非法空白（全角空格 U+3000、不换行空格 U+00A0 等）换成普通空格，
 *      尾随逗号（`,` 后面只剩 `}` 或 `]`）删掉；
 *   4. 数字紧贴着收尾括号时补一个换行 —— **Cubism SDK 自带的 JSON 解析器只在
 *      `\n`、`,`、`\r` 处结束数字**，`{"FadeOutTime":0}` 这种紧凑写法（QJsonDocument
 *      读完全合法）会让 SDK 判整份 json 非法，模型根本装不上。实测用户的
 *      [ryoheyLab.] 模型有 66 处。
 *
 * **不猜数据**：字符串**里面**的字符一个都不动 —— 那是内容，不是排版。
 *
 * @param buffer [in,out] 待修补的字节流；有改动时原地换成修好的 UTF-8。
 * @return 改动清单；`touched == false` 表示没动过。
 *
 * @note 线程安全：只操作传入的缓冲区，不读写任何共享状态，可并发调用。
 */
JsonRepair repairJson(QByteArray *buffer);

/**
 * @brief 贴图列表的补全结果。
 */
struct TextureCompletion
{
    QStringList textures;   ///< 生效的贴图相对路径（相对模型目录，'/' 分隔）。空 = 没有可用贴图
    bool inferred = false;  ///< true 表示这份列表是推断出来的（json 里原本是空的）
    QString note;           ///< 一句给人看的说明：推成功说「用哪张」，推不出来说「为什么不敢猜」
};

/**
 * @brief 取 model3.json 里生效的贴图列表；列表为空时按模型目录推断唯一候选并写回 json。
 *
 * 实测素材里有一类模型：`"Textures": []`（或整个字段缺失），但目录里明明躺着一张
 * `textures/texture_00.png`。SDK 的 CubismModelSettingJson 会把「贴图数为 0」判成
 * **无法装载**，于是模型在墙上根本不出现 —— 而它其实只差这一个字段。
 *
 * 推断规则刻意保守，**宁可判不出来也不猜**：
 *   1. 只在声明列表为空时推断（声明了就不动，哪怕文件缺失 —— 那是另一类问题，交给调用方报错）；
 *   2. 候选只看 `<模型目录>/textures/*.png`，没有再看 `<模型目录>/*.png`（浅层，不下钻）；
 *   3. 候选**恰好一个**才采用。两个以上说明分不清哪张属于这个 moc，保持原样并记 note。
 *
 * @param buffer   [in,out] 已过 repairJson 的字节流；推断成功时原地写回带 Textures 的版本。
 * @param modelDir model3.json 所在目录（绝对路径）。
 * @return 生效的贴图列表与说明。
 *
 * @note 与 repairJson 同理：**校验器与装载器必须共用这一份**，否则会出现
 *       「列表里有、点开装不上」。
 */
TextureCompletion completeTextures(QByteArray *buffer, const QString &modelDir);

/**
 * @brief 读盘 → repairJson → completeTextures 这一整套「装载前准备」的结果。
 */
struct PreparedSetting
{
    bool ok = false;         ///< 读盘与修补是否走完（**不代表 JSON 语法合法**，那由调用方自己解析判定）
    QByteArray bytes;        ///< 可直接交给解析器的字节流
    QString readError;       ///< ok==false 时非空：读盘失败原因
    JsonRepair repair;       ///< 修补明细（日志用）
    TextureCompletion tex;   ///< 贴图补全结果（日志/警告用）
};

/**
 * @brief 把 model3.json 准备成「可以直接喂给解析器」的状态。
 *
 * 这是**校验器与装载器的唯一公共入口**。两边原本各抄一遍
 * 「readFile → repairJson → completeTextures」，顺序与调参稍有出入就会漂开，而漂开的
 * 后果是最难查的那种组合：列表里点得动、点开却装不上（详见 `MEMORY-kanban.md` 的
 * 「素材容错：两处 JSON 解析点口径必须一致」）。
 *
 * 只做「读 + 修 + 补」，**不解析、不判合法性** —— 校验器用 QJsonDocument 判、装载器用
 *  Cubism 的 CubismModelSettingJson 判，两边宽容点不重合，那一层交给各自决定。
 *
 * @param jsonPath model3.json 的绝对路径。
 * @return 见 PreparedSetting。`ok==false` 时看 readError。
 */
PreparedSetting prepareSettingBytes(const QString &jsonPath);

} // namespace kanban

#endif // MODELJSONREPAIR_H

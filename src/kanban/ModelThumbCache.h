// 看板娘「模型」模块的静态预览图缓存：命名、查找、落盘。
//
// 命名规则由用户指定：**文件名 = 模型文件夹名**。ModelInfo::id 恰好就是模型
// 目录的叶子名(QDir(rootDir).dirName())，所以 id 直接拿来当文件名。
//
// 取图逻辑同样是用户指定的：**先按名字找，找不到才生成**。因此这里刻意没有
// 任何「自动失效」判断 —— 换过贴图/动作的模型会一直显示旧图，必须走强制重建
// (设置页「刷新」按钮就是这么做的)。
//
// 路径规则只此一处：界面侧(取图)与生成进程(写图)共用同一个函数，两边不可能
// 对不上。缓存根目录一律经 CachePaths，不自行拼接 ".cache"。
#ifndef KANBANMODELTHUMBCACHE_H
#define KANBANMODELTHUMBCACHE_H

#include <QString>

class QImage;

namespace kanban {

class ModelThumbCache
{
public:
    // 缓存目录 <程序目录>/.cache/model-thumbs，不存在时创建。
    // 创建失败返回空串，调用方按「本机没有可用缓存」处理(出不了图，但不崩)。
    static QString directory();

    // 模型 id → 缓存 PNG 的绝对路径。**不检查文件是否存在，也不创建目录**，
    // 所以它是纯函数，可以被反复调用。
    // id 为空、含路径分隔符、或就是 "." / ".." 时返回空串 —— 宁可不出图，
    // 也绝不让一个畸形 id 把文件写到缓存目录之外。
    static QString pathFor(const QString &modelId);

    // 按名字找图：路径合法、文件存在、且非零字节时为 true。
    // 加「非零字节」是因为上一次写到一半被杀会留下空文件，那种情况必须重生成。
    static bool has(const QString &modelId);

    // 生成进程用：把渲染结果写进缓存位置，成功返回写出的绝对路径，失败返回空串。
    //
    // 先写进程独占的临时文件再改名：界面随时可能在读这个目录，直接往目标路径
    // 写会让它读到半张图(甚至读到只写了一半的 PNG 头，解码失败)。
    static QString store(const QString &modelId, const QImage &image);

    // 清掉上次生成中途被杀留下的 *.tmp。
    //
    // 只在「确认没有生成进程在跑」时调用(界面里就是启动新任务之前那一瞬)：
    // 那些临时文件只可能来自已经死掉的进程，留着只会让用户在自己被告知过的
    // 缓存目录里看到一堆垃圾。
    static void sweepTempFiles();
};

} // namespace kanban

#endif // KANBANMODELTHUMBCACHE_H

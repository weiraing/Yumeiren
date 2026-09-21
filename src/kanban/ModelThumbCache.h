// 看板娘模型预览图缓存：命名、查找、落盘。
//
// 文件名 = ModelInfo::thumbKey(模型目录相对 data/models 的路径，分隔符换成 '#'，如
// 分类#分类#模型文件夹名)；取图先按名字找，找不到才生成 —— 刻意没有「自动失效」
// 判断，换过贴图/动作的模型会一直显示旧图，必须走强制重建。
// 路径规则只此一处，界面侧与生成进程共用；缓存根目录一律经 CachePaths。
#ifndef KANBANMODELTHUMBCACHE_H
#define KANBANMODELTHUMBCACHE_H

#include <QString>

class QImage;

namespace kanban {

class ModelThumbCache
{
public:
    // 缓存目录 <程序目录>/.cache/model-thumbs，不存在时创建；创建失败返回空串。
    static QString directory();

    // thumbKey → 缓存 PNG 绝对路径。不检查文件是否存在、不创建目录，是纯函数。
    // key 为空/含分隔符/为 "."/".." 时返回空串 —— 绝不让畸形 key 写出缓存目录之外。
    static QString pathFor(const QString &thumbKey);

    // 路径合法、文件存在且非零字节时返回 true：写一半被杀会留下空文件，必须重生成。
    static bool has(const QString &thumbKey);

    // 删掉某个模型的缓存图。删不到不算失败，返回值只用来决定要不要写日志。
    static bool remove(const QString &thumbKey);

    // 生成进程用：写入成功返回绝对路径，失败返回空串。先写进程独占的临时文件再改名，
    // 否则界面可能读到半张图。
    static QString store(const QString &thumbKey, const QImage &image);

    // 清掉上次生成中途被杀留下的 *.tmp。只在「确认没有生成进程在跑」时调用。
    static void sweepTempFiles();
};

} // namespace kanban

#endif // KANBANMODELTHUMBCACHE_H

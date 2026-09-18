#ifndef KANBAN_IMAGEDECODE_H
#define KANBAN_IMAGEDECODE_H

#include <QImage>
#include <QImageReader>

namespace kanban {

// 读一张图，并在解码阶段就缩到 maxDim（最长边上限，0 = 保持原尺寸）。
//
// 为什么要包这一层：Qt 6 的 QImageReader 有一个**进程级全局**的分配上限
// （QImageReader::allocationLimit()，Qt 6.10.2 实测默认 256 MiB）。超过就整张图
// 拒绝解码，只报一句 "Unable to read image data"。而 Live2D 素材里真的存在
// 16384x8192 的图集（解压后 512 MiB）—— 明明马上就要缩到 2048，却连读都读不进来。
//
// 三个已经踩过的坑，别再重走：
//   * QImageReader::canRead() **只读文件头，不查上限**。所以「装载成功、紧接着
//     解码失败」会同时出现，看着像素材损坏，其实是撞了上限。诊断时别去验文件格式。
//   * setScaledSize() 绕不过去：上限检查针对**源图**尺寸，发生在缩放之前。
//     （PNG 的 handler 本来也不支持 ScaledSize 选项，实测设置后照样失败。）
//   * 上限是全局的，不是每个 reader 一份 → 抬升与还原之间必须串行，否则会把
//     别的线程设的值覆盖掉。这里用一把锁把「抬 → 读 → 还原」整个包住。
//
// 注意：这里只放宽「能不能读进来」，不改变「读多大」—— 缩放仍在读取之后做，
// 所以峰值内存依旧是源图大小。调用方给的 maxDim 越接近源图，收益越小。
QImage readImageDownscaled(QImageReader &reader, int maxDim);

} // namespace kanban

#endif // KANBAN_IMAGEDECODE_H

#ifndef SHELLFILEOPS_H
#define SHELLFILEOPS_H

// Win32 shell 层的文件操作。与 desktopmount 分开一个文件，是因为那个文件的职责
// 明确限定在「视频壁纸的 WorkerW 挂载 + 系统探针」，把文件操作塞进去会让它的
// 边界糊掉。命名空间沿用 fbswin，界面/业务代码依旧不直接碰 Win32。

#include <QtCore/qglobal.h>

class QString;

namespace fbswin {

// 把文件或目录**整体移入回收站**（可撤销）。成功返回 true。
//
// 为什么不用 QDir::removeRecursively()：那是不可恢复的永久删除。模型是用户自己的
// 素材（从各处下载、自己整理、可能还改过贴图），误点一次就真没了。走回收站删错了
// 还能在资源管理器里捡回来 —— 这也正是 Windows 上「删除」的默认语义。
//
// **返回 false 的含义是「什么都没删」**：本函数只把「目标路径已经不存在」当作成功，
// 返回码非零但东西确实没了的情况算成功（原因见 .cpp 里的长注释，本机实测到的
// SHFileOperation 怪癖）。所以调用方可以放心地把失败当作「原样未动」来措辞。
//
// 失败时把**中文原因**写进 error（可为 nullptr）。调用方应当把原因原样转达给用户，
// **不要静默回退成永久删除** —— 那等于把「可撤销」偷偷降级成「不可撤销」。
//
// 已知局限（写在这里免得以后有人以为它永远可撤销）：FOF_ALLOWUNDO 只在目标卷有
// 回收站时生效。网络盘、或没启用回收站的可移动介质上，Windows 会退化为永久删除。
// 回收站装不下时同理。本项目的模型目录固定在程序目录下，正常都落在本地卷上。
//
// 只接受绝对路径；路径不存在、或指向驱动器根目录时直接判失败。
bool moveToRecycleBin(const QString &absolutePath, QString *error);

} // namespace fbswin

#endif // SHELLFILEOPS_H

// 「生成模型预览图」这个隐藏进程模式。
//
// 必须是独立进程而非工作线程：Cubism 的着色器缓存是进程级单例，在第二个 GL 上下文
// 里没法安全复用。
//
// 命令行：--render-model-thumbs [--force]
// **契约是文件系统**：每张图落盘到 <程序目录>/.cache/model-thumbs/<模型名>.png 即为
// 完成信号；stdout 只供人看，主程序不依赖它。
#ifndef KANBANMODELTHUMBJOB_H
#define KANBANMODELTHUMBJOB_H

#include <QString>
#include <QStringList>

namespace kanban {

// args 传整个命令行(含 argv[0])。失败原因写进 *error(可传 nullptr)。
int runModelThumbJob(const QStringList &args, QString *error);

} // namespace kanban

#endif // KANBANMODELTHUMBJOB_H

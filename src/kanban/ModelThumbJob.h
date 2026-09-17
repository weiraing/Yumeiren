// 「生成模型预览图」这个隐藏进程模式的全部实现。
//
// ── 为什么必须是独立进程，而不是工作线程 ──────────────────────────────────
// Cubism 的着色器缓存(CubismShader_OpenGLES2)是**进程级单例**，里面存的是
// GL program id，而 program id 只在创建它的那个上下文里有效。要在第二个上下文
// 里渲染，只有两条路，两条都不可接受：
//   · 沿用主上下文编出来的 id → glUseProgram 静默失败，出的是空图
//     (即「静默不出图」的第 4 号坑，表现是装载/上传/DrawFrame 全部正常但画面空)；
//   · 为了换上下文而 ReleaseInvalidShaderProgram() → 把桌面上**正在跑的看板娘**
//     的着色器一起丢掉，小人当场黑掉，还要多花 ~800ms 重编译。
// 独立进程有独立的地址空间：两套单例、两个上下文，互不干扰。
//
// ── 与主程序的契约 ────────────────────────────────────────────────────────
// 命令行：--render-model-thumbs [--force]
//   --force 重新生成全部模型；不给则只补缺的(命中缓存直接跳过)。
//
// **契约是文件系统，不是标准输出**：每张图成功落盘到 <程序目录>/.cache/model-thumbs/
// <模型文件夹名>.png，就是唯一的完成信号。主程序在任务期间轮询该目录(比对修改
// 时间)，进程退出后按「有没有图」整面重贴。
//
// 为什么不拿 stdout 当协议：本程序是 **GUI 子系统**的可执行文件，实测它的 stdout
// 在 GL 上下文建立之后就写不出去了(见 ModelThumbJob.cpp 里 report() 的说明)。
// 下面这些行仍然会打印，但只是给人看的 —— 在控制台里手工跑一次时有用，
// 主程序的正确性不依赖它们。
//   "SKIP <id>"          已有缓存，跳过
//   "OK <id>"            已出图并落盘
//   "FAIL <id> <原因>"   该模型出图失败，不中断，继续下一个
//   "DONE <成功> <失败> <跳过>"
// 退出码：0 = 没有失败项；1 = 有失败项；2 = 本构建无 Live2D；3 = GL 环境不可用。
#ifndef KANBANMODELTHUMBJOB_H
#define KANBANMODELTHUMBJOB_H

#include <QString>
#include <QStringList>

namespace kanban {

// args 传整个命令行(含 argv[0]，与 QCoreApplication::arguments() 一致)。
// 失败原因写进 *error(可传 nullptr)。另外还会往 stdout 打几行进度，但那只是
// 给人看的 —— 完成与否请以 .cache/model-thumbs/ 下的文件为准。
int runModelThumbJob(const QStringList &args, QString *error);

} // namespace kanban

#endif // KANBANMODELTHUMBJOB_H

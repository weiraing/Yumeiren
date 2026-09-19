#ifndef VIDEODIAG_H
#define VIDEODIAG_H

#include <QString>

class QObject;

// 视频壁纸诊断日志：统一入口、分级、文件滚动、可选资源采样。
//
//   - 默认 Error+Warning+Info 写文件；Debug 仅诊断模式输出(环境变量
//     YUMEIREN_DIAG=1 或 HKCU video/diag=true)；
//   - 高频资源采样默认关，间隔由 YUMEIREN_DIAG_SAMPLE_MS 指定(毫秒)；
//   - 日志 1MB 滚动，保留一份 .old；
//   - 每行 [HH:mm:ss.zzz][LEVEL][tid=NNNN][Module] message —— 线程 ID 与模块名
//     用于把崩溃前的时序串到具体对象/线程上；
//   - 媒体只记文件名；不输出完整路径，不记录用户数据。
namespace videodiag {

enum class Level { Error = 0, Warning = 1, Info = 2, Debug = 3 };

// 创建日志目录并打开文件；幂等，在 main() 早期调用。
void init();

bool diagEnabled();

// module 省略时按视频壁纸模块记录。
void log(Level lv, const QString &msg, const QString &module = QStringLiteral("VideoWallpaper"));

// 诊断模式下定时采样进程资源；非诊断模式为 no-op(由 VideoWallpaper 构造时调用)。
void startDiagSampling();

// 启动流程分段测量：以 init() 为原点、QElapsedTimer 单调时钟，每阶段打印
// "本段耗时 + 累计"。
void stage(const QString &name);

// 距进程创建的单调毫秒数(未初始化返回 -1)。
qint64 elapsedMs();

// 生命周期事件：只记对象地址与类名(把创建/销毁两端对上)，不含路径或用户数据；
// 非诊断模式零写盘。
void logObjectEvent(const char *action, const QObject *obj,
                    const QString &detail = QString());

} // namespace videodiag

#endif // VIDEODIAG_H

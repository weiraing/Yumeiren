#ifndef VIDEODIAG_H
#define VIDEODIAG_H

#include <QString>

// 视频壁纸诊断日志(阶段7)：统一入口、分级、文件滚动、可选资源采样。
// 设计约束(任务书 10.2-10.4)：
//   - 默认 Error+Warning+Info 写入文件；Debug 仅诊断模式开启时输出；
//   - 诊断模式：环境变量 YUMEIREN_DIAG=1 或 HKCU video/diag=true；
//   - 高频资源采样默认关闭，间隔由 YUMEIREN_DIAG_SAMPLE_MS 指定(毫秒)；
//   - 日志文件 1MB 滚动，保留一份 .old，不无限增长；
//   - 每行带毫秒时间戳；媒体只记文件名，不输出完整路径。
namespace videodiag {

enum class Level { Error = 0, Warning = 1, Info = 2, Debug = 3 };

// 创建日志目录并打开文件；重复调用无害(幂等)。在 main() 早期调用。
void init();

bool diagEnabled();

void log(Level lv, const QString &msg);

// 诊断模式下定时采样进程资源(WS/Private/线程/句柄/GPU 由外部采样器覆盖)。
// startDiagSampling 由 VideoWallpaper 构造时调用：非诊断模式为 no-op。
void startDiagSampling();

} // namespace videodiag

#endif // VIDEODIAG_H

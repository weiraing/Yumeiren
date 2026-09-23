#ifndef SUSPENDPOLICY_H
#define SUSPENDPOLICY_H

#include <QtGlobal>

// 视频壁纸 / 网页壁纸 / 看板娘三个模块共用的「长挂起释放」分级阈值。
//
// 语义：持续挂起超过阈值就释放重型资源(视频解码管线 / WebView2 控制器 / Live2D
// 模型与纹理)，恢复必然伴随一次重建(视频≈2s)。阈值按「恢复是否需要人工动作」
// 分档 —— 恢复要靠人的档位放得越积极，随时可能自己切回来的档位越保守。
namespace suspendpolicy {

// 锁屏 / 熄屏：画面根本不存在，且恢复必然伴随人工动作 —— 放得最积极。
inline constexpr qint64 kHiddenMs = 5000;
// 电池供电：省电优先。
inline constexpr qint64 kBatteryMs = 30000;
// 全屏 / 桌面被遮挡：用户可能随时切回桌面，等 60s 再放。
inline constexpr qint64 kCoveredMs = 60000;
// 其余档位的兜底(理论上到不了)：最长。
inline constexpr qint64 kDefaultMs = 180000;

// 自动化测试的压档机制：环境变量给出正数时**覆盖全部分级**(壁纸侧 YUMEIREN_LONG_SUSPEND_MS、
// 看板娘侧 YUMEIREN_KANBAN_SUSPEND_MS、网页壁纸侧 YUMEIREN_WEB_SUSPEND_MS)。
// 名字沿用各自历史叫法，脚本不破坏。
inline qint64 thresholdWithEnvOverride(const char *envName, qint64 gradedMs)
{
    if (const int overrideMs = qEnvironmentVariableIntValue(envName); overrideMs > 0)
        return overrideMs;
    return gradedMs;
}

} // namespace suspendpolicy

#endif // SUSPENDPOLICY_H

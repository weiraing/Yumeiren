#ifndef CONFIGKEYS_H
#define CONFIGKEYS_H

// 统一配置键定义(任务书 §六)：全项目的设置键集中于此，业务代码禁止手写键字符串。
// 存储格式为 INI：键 "image/rotate" 对应 INI 中的 [Image] 节 rotate 项。
namespace ConfigKeys {

namespace Meta {
inline constexpr auto ConfigVersion = "meta/configVersion";
}

namespace Ui {
inline constexpr auto Theme = "ui/theme";
}

namespace Image {
inline constexpr auto Rotate = "image/rotate";
inline constexpr auto Scale = "image/scale";
inline constexpr auto Brightness = "image/brightness";
inline constexpr auto Contrast = "image/contrast";
inline constexpr auto Blur = "image/blur";
inline constexpr auto Opacity = "image/opacity";
inline constexpr auto PosType = "image/posType";
inline constexpr auto FolderExt = "image/folderExt";
// 图片背景模式(互斥单选)：0=单图 1=随机，默认单图。
inline constexpr auto Mode = "image/mode";
// image/comboEffect(应用图片时叠加全窗口效果)与 effect/keepImage(应用特效时叠加图片)
// 已废弃：图片与特效各自独立生效，不再互相覆盖。旧配置里的这两行留着不读不写，
// 回滚旧版本仍可读取。
inline constexpr auto Preset = "image/preset";
inline constexpr auto CustomPath = "image/customPath";
inline constexpr auto GalleryDir = "image/galleryDir";
}

namespace Effect {
inline constexpr auto Type = "effect/type";
inline constexpr auto LightColor = "effect/lightColor";
inline constexpr auto DarkColor = "effect/darkColor";
inline constexpr auto LightAlpha = "effect/lightAlpha";
inline constexpr auto DarkAlpha = "effect/darkAlpha";
inline constexpr auto ClearAddress = "effect/clearAddress";
inline constexpr auto ClearBarBg = "effect/clearBarBg";
inline constexpr auto ClearWinUIBg = "effect/clearWinUIBg";
inline constexpr auto ShowLine = "effect/showLine";
// effect/keepImage(特效时叠加图片背景)已废弃，理由同上。
}

namespace Video {
inline constexpr auto Playlist = "video/playlist";
inline constexpr auto WasPlaying = "video/wasPlaying";
inline constexpr auto Volume = "video/volume";
// 播放模式(三选一)：0=单循环 1=列表循环 2=随机。见 VideoWallpaper::PlayMode。
inline constexpr auto PlayMode = "video/playMode";
// 旧版两个开关(列表循环播放 / 随机播放)已被 PlayMode 取代，仅保留键名供
// AppConfig 一次性迁移读取，产品代码不再写入。
inline constexpr auto AutoLoopLegacy = "video/autoLoop";
inline constexpr auto RandomLegacy = "video/random";
inline constexpr auto PauseFullscreen = "video/pauseFullscreen";
inline constexpr auto PauseBattery = "video/pauseBattery";
inline constexpr auto TargetFps = "video/targetFps";
// 限帧方式：true=保速丢帧(画面速度不变，3D 引擎占用按比例下降，解码开销不变)，
// false=慢动作(setPlaybackRate 放慢，解码与 GPU 同时下降，最省资源但画面变慢)。
// 见 VideoWallpaper::setKeepSpeed。
inline constexpr auto FpsKeepSpeed = "video/fpsKeepSpeed";
inline constexpr auto Reclaim = "video/reclaim";
inline constexpr auto ScreenMode = "video/screenMode";
inline constexpr auto AffinityLimit = "video/affinityLimit";
inline constexpr auto Diag = "video/diag";
}

namespace Window {
inline constexpr auto Width = "window/width";
inline constexpr auto Height = "window/height";
inline constexpr auto X = "window/x";
inline constexpr auto Y = "window/y";
inline constexpr auto Maximized = "window/maximized";
}

// 看板娘(Live2D 桌宠)。只持久化用户设置，运行态(当前状态机状态、当前动作)
// 一律不落盘：进程重启后回到"未启动"，再由 kanban/enabled 决定是否自动拉起
// ——「记住上次状态」，首次安装该键不存在，默认不启动。
namespace Kanban {
inline constexpr auto Enabled = "kanban/enabled";         // 用户最后一次是否让它在跑
inline constexpr auto ModelPath = "kanban/modelPath";     // 当前模型 model3.json 绝对路径
inline constexpr auto Scale = "kanban/scale";             // 显示缩放(%)，相对模型基准高度
inline constexpr auto Opacity = "kanban/opacity";         // 窗口不透明度(%)
inline constexpr auto Width = "kanban/width";             // 窗口宽(逻辑像素)
inline constexpr auto Height = "kanban/height";           // 窗口高(逻辑像素)
inline constexpr auto PosX = "kanban/x";                  // 窗口左上角(逻辑像素)
inline constexpr auto PosY = "kanban/y";
inline constexpr auto AlwaysOnTop = "kanban/alwaysOnTop"; // 置顶(关=贴在桌面之上的一般层)
inline constexpr auto MouseThrough = "kanban/mouseThrough"; // 鼠标穿透
inline constexpr auto TargetFps = "kanban/targetFps";     // 动画目标帧率
inline constexpr auto AllowInteraction = "kanban/allowInteraction"; // 允许点击/悬停互动
// 视线追踪强度：0=无 1=弱 2=中 3=强。默认 2(中) —— 这是「看起来像活物」的核心，
// 关掉之后模型只会呆立着，所以默认开；而中档是此前实测手感满意的那个值。
inline constexpr auto GazeStrength = "kanban/gazeStrength";
// 纹理按窗口尺寸降采样后再上传(默认开)。关掉即恢复原尺寸上传，代价是显存与
// 装载峰值按素材原始尺寸走(实测一个 4×4096² 的模型在 320×480 窗口下常驻 393MB
// 显存)。判据见 KanbanRenderer.h 的 textureMaxDimFor。
inline constexpr auto TextureDownscale = "kanban/textureDownscale";
// 旧版的布尔开关(有则视为「开=中档」)。只在读配置时作为迁移来源使用，
// 新写入一律走 GazeStrength —— 见 KanbanController::loadSettings 里的迁移注释。
inline constexpr auto GazeTrackingLegacy = "kanban/gazeTracking";
}

// 系统托盘与"关窗不等于退出"策略。
namespace Tray {
inline constexpr auto Enabled = "tray/enabled";                       // 允许使用托盘
inline constexpr auto MinimizeToTrayOnClose = "tray/minimizeToTrayOnClose";
}

} // namespace ConfigKeys

#endif // CONFIGKEYS_H

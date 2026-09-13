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
inline constexpr auto ComboEffect = "image/comboEffect";
inline constexpr auto Preset = "image/preset";
inline constexpr auto CustomPath = "image/customPath";
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
inline constexpr auto KeepImage = "effect/keepImage";
}

namespace Video {
inline constexpr auto Playlist = "video/playlist";
inline constexpr auto WasPlaying = "video/wasPlaying";
inline constexpr auto Volume = "video/volume";
inline constexpr auto AutoLoop = "video/autoLoop";
inline constexpr auto Random = "video/random";
inline constexpr auto PauseFullscreen = "video/pauseFullscreen";
inline constexpr auto PauseBattery = "video/pauseBattery";
inline constexpr auto TargetFps = "video/targetFps";
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

} // namespace ConfigKeys

#endif // CONFIGKEYS_H

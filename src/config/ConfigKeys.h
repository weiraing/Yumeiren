#ifndef CONFIGKEYS_H
#define CONFIGKEYS_H

// 统一配置键定义：业务代码禁止手写键字符串。存储为 INI，键 "image/rotate"
// 对应 INI 中的 [Image] 节 rotate 项。
namespace ConfigKeys {

namespace Meta {
inline constexpr auto ConfigVersion = "meta/configVersion";
}

namespace Ui {
inline constexpr auto Theme = "ui/theme";
}

namespace Image {
// 透明度上限(%)。88 对应旧版「不透明度」滑杆的 alpha 下限 30(1 - 30/255 ≈ 88%)：
// 再往上背景几乎不可见，用户容易以为图弄丢了。UI 滑杆、配置钳位、旧值迁移共用。
inline constexpr int MaxTransparency = 88;
inline constexpr auto Rotate = "image/rotate";
inline constexpr auto Scale = "image/scale";
inline constexpr auto Brightness = "image/brightness";
inline constexpr auto Contrast = "image/contrast";
inline constexpr auto Blur = "image/blur";
// 透明度(%)：0=不透明，MaxTransparency=最透。绘制/写入 DLL 配置时换算成
// alpha=round((100-透明度)×255/100)；旧键迁移在 AppConfig 默认值补齐之前做。
inline constexpr auto Transparency = "image/transparency";
inline constexpr auto OpacityLegacy = "image/opacity"; // 旧版「不透明度」(alpha 30..255)，仅迁移用
inline constexpr auto PosType = "image/posType";
inline constexpr auto FolderExt = "image/folderExt";
inline constexpr auto Mode = "image/mode";
// image/comboEffect 与 effect/keepImage(图片/特效互相叠加)已废弃：两者现在各自独立
// 生效。旧配置里的这两行留着不读不写，回滚旧版本仍可读取。
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
}

namespace Video {
inline constexpr auto Playlist = "video/playlist";
inline constexpr auto WasPlaying = "video/wasPlaying";
inline constexpr auto Volume = "video/volume";
// 播放模式(三选一)：0=单循环 1=列表循环 2=随机。
inline constexpr auto PlayMode = "video/playMode";
// 旧版两个开关(列表循环播放 / 随机播放)已被 PlayMode 取代，仅保留键名供
// AppConfig 一次性迁移读取，产品代码不再写入。
inline constexpr auto AutoLoopLegacy = "video/autoLoop";
inline constexpr auto RandomLegacy = "video/random";
inline constexpr auto PauseFullscreen = "video/pauseFullscreen";
inline constexpr auto PauseBattery = "video/pauseBattery";
inline constexpr auto TargetFps = "video/targetFps";
inline constexpr auto Reclaim = "video/reclaim";
inline constexpr auto AffinityLimit = "video/affinityLimit";
inline constexpr auto Diag = "video/diag";
}

// 动态网页壁纸(WebView2)。Source 存「url 或 data/web 下的相对路径/文件名」。
namespace Web {
inline constexpr auto Enabled = "web/enabled";           // 上次退出时在跑(启动恢复判据)
inline constexpr auto Source = "web/source";
inline constexpr auto RefreshMode = "web/refreshMode";   // 0=实时 1=快照·每分钟 2=快照·每小时
inline constexpr auto Volume = "web/volume";             // 0=静音；WebView2 只有静音两档
inline constexpr auto Zoom = "web/zoom";                 // 50~200
inline constexpr auto FpsCap = "web/fpsCap";             // 0=跟随页面自身帧率(默认档);出厂 30
}

namespace Window {
inline constexpr auto Width = "window/width";
inline constexpr auto Height = "window/height";
inline constexpr auto X = "window/x";
inline constexpr auto Y = "window/y";
inline constexpr auto Maximized = "window/maximized";
// 一次性标志：是否已把历史遗留的「超高窗口高度」(默认 840、实际存着 929)收到新
// 默认值——光改默认值不生效，得主动收一次。判据见 MainWindow 的几何注释。
inline constexpr auto HeightFit = "window/heightFit";
}

// 看板娘。只持久化用户设置，运行态(状态机状态、当前动作)不落盘：重启后回到
// "未启动"，再由 kanban/enabled 决定是否自动拉起。
namespace Kanban {
inline constexpr auto Enabled = "kanban/enabled";         // 用户最后一次是否让它在跑
inline constexpr auto ModelPath = "kanban/modelPath";     // 当前模型 model3.json 绝对路径
inline constexpr auto Scale = "kanban/scale";             // 显示缩放(%)，相对模型基准高度
inline constexpr auto Transparency = "kanban/transparency"; // 窗口透明度(%)，0=不透明
inline constexpr auto OpacityLegacy = "kanban/opacity";     // 旧版「不透明度」，仅迁移用
inline constexpr auto MenuBgColor = "kanban/menuBgColor";     // 右键菜单底色(空=主题默认)
inline constexpr auto MenuTransparency = "kanban/menuTransparency"; // 右键菜单透明度(%)，0=不透明
inline constexpr auto MenuOpacityLegacy = "kanban/menuOpacity"; // 旧版「不透明度」，仅迁移用
inline constexpr auto MenuGlass = "kanban/menuGlass";         // 右键菜单玻璃效果(0=关)
inline constexpr auto Width = "kanban/width";             // 窗口宽(逻辑像素)
inline constexpr auto Height = "kanban/height";           // 窗口高(逻辑像素)
inline constexpr auto PosX = "kanban/x";                  // 窗口左上角(逻辑像素)
inline constexpr auto PosY = "kanban/y";
inline constexpr auto AlwaysOnTop = "kanban/alwaysOnTop"; // 置顶(关=贴在桌面之上的一般层)
inline constexpr auto MouseThrough = "kanban/mouseThrough"; // 鼠标穿透
inline constexpr auto TargetFps = "kanban/targetFps";     // 动画目标帧率
inline constexpr auto AllowInteraction = "kanban/allowInteraction"; // 允许点击/悬停互动
inline constexpr auto MotionLoop = "kanban/motionLoop";   // 自动轮流播放全部动作
inline constexpr auto PlaySound = "kanban/playSound";     // 播放模型动作语音(默认开)
inline constexpr auto DoubleClickSwitch = "kanban/doubleClickSwitch"; // 双击切换动作
// 视线追踪强度：0=无 1=弱 2=中 3=强。默认 2(中)：追踪是「看起来像活物」的核心，
// 而中档是实测手感最满意的值。
inline constexpr auto GazeStrength = "kanban/gazeStrength";
// 纹理按窗口尺寸降采样后再上传(默认开)；关掉即按素材原始尺寸上传(实测 4×4096²
// 模型在 320×480 窗口下常驻 393MB 显存)。判据见 KanbanRenderer.h。
inline constexpr auto TextureDownscale = "kanban/textureDownscale";
// 按清单隐藏网格(默认开)：读模型目录里的 *.hidden.json(查看器导出)，隐藏其中列出的
// 部件与网格。关掉 = 完全照模型原样显示，不改任何文件。
inline constexpr auto MeshHide = "kanban/meshHide";
// 旧版布尔开关(有则视为「开=中档」)：只在读配置时作为迁移来源，新写入一律走
// GazeStrength —— 见 KanbanController::loadSettings 里的迁移注释。
inline constexpr auto GazeTrackingLegacy = "kanban/gazeTracking";
// 全局显示/隐藏看板娘的快捷键，存 QKeySequence 的 PortableText(如 "Ctrl+Alt+K")。
// 空串 = 停用。注册失败(被占用/组合不合法)时不落盘半吊子值，见设置页接线处。
inline constexpr auto ToggleHotkey = "kanban/toggleHotkey";
}

namespace Tray {
inline constexpr auto Enabled = "tray/enabled";                       // 允许使用托盘
inline constexpr auto MinimizeToTrayOnClose = "tray/minimizeToTrayOnClose";
}

} // namespace ConfigKeys

#endif // CONFIGKEYS_H

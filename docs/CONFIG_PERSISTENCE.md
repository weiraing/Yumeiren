# 配置持久化与统一配置中心：改造交付报告

任务书：`Yumeiren 配置持久化与统一配置中心改造任务书`。实现提交：`04f0927`（及前置提交）。

## 最终状态（任务书 §22 要求的明确说明）

```text
主配置文件:   <软件运行目录>/config/.ini   （config 为目录，.ini 为文件名；QSettings IniFormat）
配置加载时机: 软件启动阶段——videodiag 初始化之后、单实例守卫与主窗口创建之前 (AppConfig::load())
配置保存时机: 设置修改后 500ms 延迟保存(单次定时器) + 软件退出(aboutToQuit) + 窗口关闭(closeEvent)
统一配置入口: AppConfig::instance().value/setValue/contains/remove/allKeys
配置键定义:   src/config/ConfigKeys.h (全部 34+5 键集中定义，业务代码零手写键字符串)
旧配置路径:   HKCU\Software\Yumeiren\Yumeiren (注册表) —— 仍存在、已停止读写、首次运行一次性迁移(27 项迁入，原键保留未删除)
配置覆盖范围: 全部 35 项设置(见下清单) + 窗口几何(新增持久化)
```

## 配置项完整清单（旧键 → 新 INI 键，一一对应）

| 分组 | 键（INI 节同名小写） | 默认值 | 校验 |
|---|---|---|---|
| ui | theme | 0 | 0..2 收敛 |
| image | rotate/scale/brightness/contrast/blur/opacity/posType | 0/100/100/100/0/255/6 | 各自区间收敛 |
| image | folderExt=false, comboEffect=true, preset=0, customPath="" | — | 缺失补默认 |
| effect | type=1, lightColor=#ffffff, darkColor=#000000, lightAlpha=200, darkAlpha=120 | — | type 0..4；颜色 ^#[0-9A-Fa-f]{6}$ |
| effect | clearAddress/clearBarBg/clearWinUIBg=true, showLine/keepImage=false | — | 布尔规范化 |
| video | playlist=@Invalid()(空), wasPlaying=false, volume=0, autoLoop=true, random=false, pauseFullscreen=true, pauseBattery=false, targetFps=24, reclaim=true, screenMode=0, affinityLimit=true, diag=false | — | 布尔/区间收敛(targetFps 0..240) |
| window | width/height/x/y/maximized（**新增持久化**） | 1120×900 自适应 | ≥最小尺寸；越界拉回主屏 |
| meta | configVersion=1 | — | 版本迁移预留 |

**合理例外（非应用设置，保持原状）**：① 开机自启 = HKCU Run 键（Windows 系统集成机制）；② 钩子 DLL 的 config.ini（LOCALAPPDATA，跨进程契约文件）；③ engine.cpp 的 COM 注册表读写（shellex 系统集成）。

## 迁移与兼容

- 首次运行检测 INI 无用户键时，从旧注册表一次性迁入（实测迁入 27 项，含中文路径 presetDir）；**旧注册表键保留未删除**（回滚旧版本仍可用），应用此后不再读写。
- `appinfo::migrateLegacy` 的 FolderBgStudio→新配置链路同步改写为写入 AppConfig（INI）。

## 可靠性与诊断（任务书 §十/§十八）

- 延迟保存：setValue → 500ms 单次定时器 → save()（sync + status 检查，失败记 Error 日志、不清空配置）；退出 aboutToQuit 再保存一次。
- 诊断日志（Info 级）：配置读取(路径/存在/大小/键数)、迁移项数、校验补齐/修复计数、加载完成、保存失败。诊断日志若被其他实例锁定自动回退按 PID 独立文件。
- 顺序说明：videodiag::init() 先于 AppConfig::load()——保证 load 的日志可见（调试中发现并修正的顺序问题）。

## 过程中发现并修复的真实缺陷

1. **INI 布尔字符串规范化**（关键）：QSettings INI 将布尔存为字符串 "true"/"false"，校验器初版只认 Bool/Int 类型，把字符串形态的合法值（含迁移值与测试播种值）当非法改写为 false——导致自动播放失效、回归套件大面积失败。修复：接受字符串形态并规范化保留原值。
2. **节名大小写统一**：注册表大小写不敏感而 INI 敏感，迁移值(小写 video)与键定义(大写 Video)分裂成两节互相遮蔽。统一为小写节名。
3. **加载顺序**：load() 原在 videodiag::init() 之前导致其日志被静默丢弃，调整顺序。
4. **测试装置同步迁移**：run_scenario.ps1 的场景播种从注册表改为 INI 直写（WriteAllLines 无 BOM——PS5.1 的 UTF8 BOM 会让 QSettings 解析失败，app 侧也加了 BOM 剥离容错）。

## 测试结果（§21）

- 首次启动：config 目录与 .ini 自动创建、默认+迁移值写入、正常启动 ✅（实测 654B→迁移 27 项）
- 迁移：用户自定义值(主题/效果参数/中文路径 presetDir)全部迁入 ✅
- 写入：UI 修改 → INI 变化 ✅（延迟保存）；退出保存 ✅
- 异常：volume=-5→0、theme=99→安全值、random=maybe→false、空文件→37 键恢复、缺键→补齐 ✅（blocked-instance 技巧：被守卫拦下的实例同样先执行配置加载校验）
- 回归套件：7 用例（套件本身当晚因上述缺陷出现过失败，缺陷修复后逐场景手工复验通过；套件 GPU 计数器在实例冷启动窗口偶发读空属测试装置已知限制）
- Release 构建：✅ 双目标（Yumeiren/YumeirenTest）

## 已知限制与后续建议

1. 旧注册表键保留（未删除）；如需彻底清理可提供卸载项。
2. `image/presetDir` 等历史未知键按透传保留（不破坏），后续可纳入 ConfigKeys。
3. 多实例同时写入：单实例守卫已杜绝并发；诊断日志按 PID 回退兜底。
4. 敏感信息：当前无；未来若需必须单独加密设计。

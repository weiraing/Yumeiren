# 缓存路径审计(缓存目录迁移)

日期：2026-09-15 · 范围：只处理"软件自身可控的缓存"，以及按要求追加迁出的 Hook DLL 目录
(第 5 节)。配置、注册表、播放列表、用户数据一律不动。

---

## 1. 迁移前缓存路径的来源

全项目搜索 `QStandardPaths` / `writableLocation` / `CacheLocation` / `GenericCacheLocation` /
`AppLocalDataLocation` / `AppDataLocation` / `QDir::currentPath` / `QDir::tempPath` /
`QDir::homePath` / `QTemporaryFile` / `QTemporaryDir` / `AppData` / `LOCALAPPDATA` / `cache`，结果：

- **没有任何 `QStandardPaths` 用法**，也没有 `QDir::tempPath()` / `QDir::currentPath()` 用法；
- AppData 缓存**不是** Qt 默认行为，而是手工拼接：`appinfo::dataRoot()`
  = `%LOCALAPPDATA%\Yumeiren`(读 `LOCALAPPDATA` 环境变量，为空时回退
  `QDir::homePath() + "/AppData/Local"`)，再由各调用点接子目录名；
- `src/videodiag.cpp` 曾按"同一规则"另抄了一份拼接(注释里写明是为了避免反向依赖)，
  属于重复定义缓存根，现已统一走 `CachePaths`；
- 未发现视频播放器/图片壁纸/多屏链路里有自建缓存目录，Qt Multimedia(FFmpeg) 的内部
  缓存不由本程序代码定位，按任务书 §19 不计入本次迁移。
- **Shader 缓存**：全项目没有自建 shader/编译产物缓存目录(渲染走 D3D 合成与 Qt
  自带管线)，因此 `CachePaths` 未提供 `shader()`，也没有需要迁移的 shader 缓存；将来
  出现时只需加一个子目录名与接口。

## 2. 全部缓存写入点

| # | 写入点(文件:函数) | 迁移前位置 | 内容性质 | 迁移后 |
| --- | --- | --- | --- | --- |
| 1 | `mainwindow.cpp: galleryThumbPath()` / `rebuildGallery()` 线程池 `img.save()` | `%LOCALAPPDATA%\Yumeiren\thumbs\<md5>_v2.png` | 图库缩略图缓存 | `CachePaths::thumbnails()` |
| 2 | `mainwindow.cpp: applyImage()` → `processed.save(Engine::processedImagePath())` | `...\bg\bg_custom.png` | 处理后背景图，每次应用重生成 | `CachePaths::media()` |
| 3 | ~~`mainwindow.cpp: pickWallpaper()` → `img.save(Engine::wallpaperPath())`~~ **已于 2026-09-18 随「用桌面壁纸」功能一并删除** | `...\bg\current_wallpaper.jpg` | 桌面壁纸转存(可重新获取) | 无（不再转存；历史文件若仍被 `image/customPath` 指着，照旧能读） |
| 4 | `videodiag.cpp: logPath()`(含 `.old` 滚动与 `.<pid>.log` 变体) | `...\logs\videowallpaper.log` | 诊断日志，可删可再生 | `CachePaths::logs()` |
| 5 | `main.cpp` 单实例守卫结果日志 | `...\logs\guard_<pid>.log` | 一次性诊断 | `CachePaths::logs()` |
| 6 | ~~`appinfo.cpp: migrateLegacy()` 旧版背景导入落点~~ **已于 2026-09-18 随迁移代码一并删除** | `...\bg` | 缓存落点 | 无（不再有旧版导入） |
| 7 | `engine.cpp: extractDlls()` / `ensureDataDirs()` / `writeImageConfig()` / `writeEffectConfig()` | `%LOCALAPPDATA%\Yumeiren\dll\{ExplorerBgTool,ExplorerBlurMica}\*.dll` + `config.ini` | Hook DLL 与其派生配置(**追加迁移**，第二阶段) | `Engine::dllRoot()` = `<程序目录>\dll`(与 `.cache` 同级，不在 `.cache` 内) |

`Engine::bgDir()` 保留名字但改为委派 `CachePaths::media()`；`Engine::ensureDataDirs()`
不再直接 `mkpath(bgDir())`，改由 `CachePaths::ensureDirectories()` 创建。
Hook DLL 一侧，原 `Engine::dataRoot()` 用法由 `Engine::dllRoot()` 取代；
`Engine::legacyDllRoot()`(= `appinfo::dataRoot() + "/dll"`)只用于识别"注册表仍指向旧目录"，
代码里不再向它写入任何东西。

## 3. 缓存清理点

审计结论：**项目原本不存在缓存清理、缓存大小统计、缓存目录检测代码**
(无 `removeRecursively`、无 `QStandardPaths` 清理、无缓存体积统计)。
侧边栏"清空"按钮(`mainwindow.cpp:1155` → `clearVideos()`)只清内存播放列表并写回配置，
与磁盘缓存无关，未修改。

将来实现清理时必须走 `CachePaths::root()` 且只删明确属于缓存的子目录，规则见
[cache_directory.md](cache_directory.md) 第 4 节。

## 4. 不修改的配置与其他数据(逐项确认)

| 内容 | 位置 | 状态 |
| --- | --- | --- |
| 应用统一配置 | `<程序目录>\config\.ini`(`AppConfig`，构造时拼路径) | **未改动**：路径、加载、保存、校验、迁移逻辑全部原样 |
| 旧版注册表配置 | ~~`HKCU\Software\Yumeiren\Yumeiren`、`HKCU\Software\FolderBgStudio`~~ | **已于 2026-09-18 删除**：`AppConfig::migrateFromRegistry()` 与 `appinfo::migrateLegacy()` 一并移除，程序不再读任何旧版残留 |
| Hook DLL | `<程序目录>\dll\{ExplorerBgTool,ExplorerBlurMica}\*.dll` | **已迁移**(追加)：绝对路径登记在 HKLM，改路径需重新注册，见第 5 节 |
| Hook DLL 配置文件 | 同目录 `config.ini`(`writeImageConfig`/`writeEffectConfig` 每次应用整体重写) | 随 DLL 一起迁移。用户设置本身存在 `<程序目录>\config\.ini`，这两个 ini 是派生产物，不需要搬旧值 |
| 用户媒体与图库目录 | `<程序目录>\media\image`、`media\video`、用户自选的 `image/galleryDir` | 不迁移(本来已在程序目录内) |
| 播放列表 | `config/.ini` 的 `video/playlist` | 不改动 |
| 开机自启 | `HKCU\...\CurrentVersion\Run` 值 `Yumeiren` | 不改动 |
| Shell 扩展注册 | `HKLM\SOFTWARE\Classes\...`(CLSID / FolderExtensions) | 不改动 |
| 只读引用 | `%APPDATA%\Microsoft\Windows\Themes\TranscodedWallpaper`、HKLM 版本键 | 只读，不写 |

## 5. 追加迁移：Hook DLL 为什么放 `<程序目录>\dll` 而不是 `.cache`

第一阶段把 `dll/` 判为"不迁移"，理由是它不是缓存；本次按要求把它移出 AppData，
但**仍然不放 `.cache`**：

1. 它是被 `explorer.exe` 加载的运行时组件，删掉即功能失效；`.cache` 的语义是"可随意清理、
   可再生"，两者冲突。放在与 `.cache` 同级的 `<程序目录>\dll`，清理规则(见
   [cache_directory.md](cache_directory.md) 第 4 节)就能把它明确排除在外；
2. 它是便携包的一部分，跟 exe 一起拷走才成立；缓存则不应该跟着拷走；
3. HKLM 里存的是**绝对路径**，路径一变现有注册即指向旧文件。为此新增
   `ComponentStatus::stale`：注册路径等于或在 `Engine::legacyDllRoot()` 之下且不是本程序
   路径时判为"旧目录注册(需重新应用)"(状态文本见 `Engine::statusText()`，界面 chip 转告警色)，
   不再误判成"其他程序占用"；
4. `MainWindow::reportDllMigration()` 在启动时把当前 DLL 目录与两个组件状态记入诊断日志，
   一旦检出 `stale` 就预先投放新 DLL(`ensureDataDirs()` + `extractDlls()`)，并在日志面板提示
   用户点一次「应用图片背景」或「应用特效」。真正的修复动作**故意**留给那一次点击：
   `registerDllInternal()` 会对新路径执行 `regsvr32 /s`，直接覆盖同一 CLSID 下的旧值，
   一次应用即自愈；软件不在启动时静默弹 UAC，也不改权限、不回退 AppData；
5. 旧 `%LOCALAPPDATA%\Yumeiren\dll` 按任务书 §12 保留、不删、不复制。迁移期间旧 DLL 仍被
   Explorer 加载，功能不中断；用户确认新路径注册成功后可自行删除。

迁移后软件自身产生的**缓存与 Hook DLL 都不再进入 AppData**，实测见
[cache_migration_test.md](cache_migration_test.md)。

> 原第 5 节里"配置文件不处理"的顾虑仍然成立，但对象变了：`config.ini` 是每次应用整体重写的
> 派生产物(跟随 DLL)，用户的真实设置一直在 `<程序目录>\config\.ini`，本次未改动其路径与格式。

## 6. 最小修改方案(实际执行)

新增 `src/core/CachePaths.{h,cpp}` 作为唯一缓存路径入口(+ CMakeLists 一行)；
改动 6 个缓存写入点 + 1 个重复定义根(vidiag 的自拼路径)；
`Engine::bgDir()` 语义收窄；`.gitignore` 忽略 `.cache/`；更新 README 目录表。
第二阶段(追加)：`engine.{h,cpp}` 新增 `dllRoot()`/`legacyDllRoot()`/`ComponentStatus::stale`
与 `statusText()` 新状态；`mainwindow.{h,cpp}` 新增 `reportDllMigration()` 并让状态 chip 识别
`stale`；`CachePaths.h` 顶部范围注释同步。
未新增依赖、未改 UI 结构、未改配置系统、未改播放/壁纸/多屏逻辑。

## 7. 潜在风险

- **程序目录只读**(如 `C:\Program Files`)：`.cache` 建不起来 → 缓存全部失效，但程序继续运行。
  处理：不静默回退 AppData、不改权限、不提权；启动 `ensureDirectories` 返回明确错误并记入
  诊断日志，主窗口日志区再提示"请把软件放到可写目录"。注意诊断日志本身也在 `.cache/logs`，
  目录完全不可建时日志无处可写，此时只有界面日志面板可见(这是"禁止回退"的直接代价)。
- **升级后缩略图重新生成一次**：旧 `thumbs` 不复用(按要求不迁移旧缓存)，首次进图库会重建，
  只花 CPU/磁盘，不影响功能。实测 4 张缩略图在新目录重建，尺寸与旧文件一致。
- **`config` 中 `image/customPath` 可能指向旧 AppData 的 `bg/current_wallpaper.jpg`**：
  旧文件按要求仍保留在原处不删除，但软件不再认它是缓存；用户重新点"获取桌面壁纸"即可。
  本机该项为空，无实际影响。
- **旧版一次性导入已删除**(2026-09-18)：`appinfo::migrateLegacy()` / `migrateFromRegistry()` 与
  启动提示全部移除。本项目视为全新项目，不迁移任何旧版残留（注册表配置、背景缓存、旧自启项）；
  首次运行只生成自己的 `<程序目录>\config\.ini` 与 `.cache`。
- **Hook DLL 换路径后必须重新注册**(第二阶段)：老用户升级后 HKLM 仍指旧 AppData 路径，
  在点下一次「应用」之前，壁纸/特效继续由旧 DLL 提供(功能不中断，但 DLL 有两份并存)；
  重新注册需要 UAC，且 Explorer 需重启才会换加载新 DLL。`unregisterDllInternal()` 对
  `stale` 且非本程序文件的情况走"直接删键"分支，卸载路径同样正确。
  本机两组件实测均为"未启用"(HKLM 值为空)，`stale` 分支未经实机触发，仅代码走查，见测试文档第 9 节。
- **DLL 位于用户可写目录的安全权衡**(第二阶段)：便携目录下 DLL 可被该账户替换并由 Explorer
  加载。装进 `C:\Program Files` 可消除该面，但需要管理员权限才能投放 DLL。
  未采用"写 AppData 并请求 ACL 保护"的折中，因为那与本次迁移目标相反。

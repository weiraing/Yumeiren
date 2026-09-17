# 缓存目录迁移测试记录

日期：2026-09-15 · 结论：**缓存已统一落在 `<程序目录>\.cache`，AppData 不再产生新的软件
缓存，配置系统零改动**。

---

## 1. 测试环境

| 项 | 值 |
| --- | --- |
| 系统 | Windows x64 · build 26200 · 时区 Asia/Shanghai |
| Qt | 6.10.2 (`D:/Develop/Qt/6.10.2/mingw_64`)，MinGW 工具链 |
| 构建目录 | `new-build`(CMake + Ninja/MinGW，`cmake --build new-build --target YumeirenTest`) |
| 被测程序 | `new-build\YumeirenTest.exe`(诊断版，**非管理员**运行) |
| 程序目录 | `C:\Users\rain\Documents\ExplorerBg\Yumeiren\new-build` |
| 旧缓存目录 | `C:\Users\rain\AppData\Local\Yumeiren` |

说明：正式 `Yumeiren.exe` 只做了**编译链接验证**(通过)，没有实际运行 —— 它启动会走
HKLM Shell 扩展注册/UAC 与重启 Explorer，属于本次不验证的范围；两者的缓存代码路径完全相同
(同一份 `CachePaths`、同一份 `main.cpp`)。

## 2. 编译

```text
cmake --build new-build --target Yumeiren       OK (link)
cmake --build new-build --target YumeirenTest   OK (link)
```

## 3. 首次启动(删除 `.cache`)

步骤：删除整个 `new-build\.cache` → 以工作目录 `C:\Users\rain\Documents`(刻意与程序目录不同)
启动 `YumeirenTest.exe` → 12 秒后结束进程。

结果：

```text
new-build\.cache\logs\videowallpaper.log          1855 B   03:39:12
new-build\.cache\media\bg_custom.png            125203 B   (旧版一次性导入，见 §9.5)
new-build\.cache\media\current_wallpaper.jpg   1612158 B   (同上)
new-build\.cache\thumbnails\3f31efb7..._v2.png   31381 B   03:39:12
new-build\.cache\thumbnails\6e3f4835..._v2.png   32095 B   03:39:12
new-build\.cache\thumbnails\96a70b23..._v2.png   50449 B   03:39:12
new-build\.cache\thumbnails\cfeeff0f..._v2.png   55642 B   03:39:12
new-build\.cache\temp\                              空     03:39:12
```

四个子目录全部新建在**exe 所在目录**下，工作目录 `C:\Users\rain\Documents` 内没有产生
`.cache`(`Test-Path` 为 `False`)。

## 4. 缓存命中与可写探针

紧接着以工作目录 `C:\Users\rain` 再启动一次同一构建：

- 四张缩略图仍是 4 个、时间戳仍为 `03:39:12` → 走缓存命中，没有重算(读的是
  `CachePaths::thumbnails()`，说明子目录名与文件命名规则两边一致)；
- `.cache\temp` 目录项数 `0` → `isWritable()` 的探针 `writetest-<pid>.tmp` 用完即删；
- 日志仍是单个 `.cache\logs\videowallpaper.log`(第二次运行为追加写，1855 → 3710 字节；
  `videodiag` 的 `.log` → `.log.old` 滚动规则未改，只改了所在目录)，全文没有 `[Cache]`
  或 `ERROR` 行，即缓存目录创建与可写检测都成功。

## 5. AppData 是否仍有新缓存

对比三次运行前后 `%LOCALAPPDATA%\Yumeiren` 的完整清单：**同样是 11 个文件，逐个字节数与
`LastWriteTime` 都没变**，最新一个仍是迁移前就存在的 `02:34` 日志。

```text
bg\bg_custom.png               265780 B  09-14 20:57
bg\current_wallpaper.jpg      3457865 B  09-14 20:30
thumbs\..._v2.png x4           见 §3 的同一批字节数  09-15 00:40
logs\videowallpaper.log         50416 B  09-15 02:34
dll\ExplorerBgTool\...          未触碰
dll\ExplorerBlurMica\...        未触碰
```

即：迁移后软件自身**零**新增 AppData 缓存；旧 `thumbs\`、`bg\`、`logs\` 按要求原地保留，
未复制、未删除。

## 6. 路径越界

- 代码侧：`CachePaths::contains()` / `isInsideProgramDir()` 用 `resolvedPath()`(折叠 `..`、
  统一 `\` 与 `/`、Windows 忽略大小写、对已存在的最深祖先做 canonical 解析)做子串判断，
  junction/符号链接把 `.cache` 指向程序目录之外时 `ensureDirectories()` 直接判失败并拒绝写；
- 静态侧：全项目搜索 `QStandardPaths`、`QDir::tempPath`、`QDir::currentPath`、
  `LOCALAPPDATA`，业务代码里的缓存用途为 0 处(`.cache` 字面量只剩 `CachePaths.cpp` 里的
  `kCacheDirName` 一处)；Hook DLL 迁到 `<程序目录>\dll` 之后，`%LOCALAPPDATA%\Yumeiren`
  在代码里已无任何**写入**用途，只残留 `Engine::legacyDllRoot()` 一处**只读**引用，
  用来识别"注册表还指向旧目录"(见第 9 节)；
- 运行侧(不可写演练)：把 `new-build\.cache` 整体改名，放一个**同名普通文件**占位，再以工作
  目录 `C:\Users\rain` 启动 → 程序照常启动、跑满 9 秒、正常结束，没有崩溃；
  `%LOCALAPPDATA%\Yumeiren` 文件数 11 → 11(**没有回退写 AppData**)；工作目录与用户目录下
  都没有新建 `.cache`；占位文件保持是文件，说明 `ensureDirectories()` 确实失败而非偷偷重建。
  此时界面日志面板应显示"缓存目录不可写/无法创建缓存目录"提示(该提示走 `setLog`，日志文件
  本身无处可写，属代码验证)。演练后已恢复原 `.cache` 目录。

## 7. 配置回归

```text
配置读取: path=.../new-build/config/.ini exists=1 size=967 keys=43 wasPlaying=false
```

- 配置文件路径、体积(967 B)、修改时间(02:34:49)在三次运行前后完全不变；
- `git diff --stat -- src config` 里 `config/` 目录**零改动**，`AppConfig` 与注册表迁移
  逻辑未被触碰；
- 旧 AppData 中的 `dll\*\config.ini`(Hook 配置)保持原样。

## 8. 功能覆盖

已覆盖：启动与单实例守卫、配置加载、图库缩略图生成与命中、旧版背景一次性导入落点、日志写入与
滚动、缓存不可写降级、换工作目录启动。

未覆盖(需交互操作，本轮未做)：视频播放、多屏切换、播放列表切换、"获取桌面壁纸"与图片壁纸应用
(即 `.cache\media` 的实际再写入)、界面"清空"按钮(该按钮只清内存播放列表，与磁盘缓存无关)。
这些代码路径的落点都是 `CachePaths::*`，与已验证路径同源。

## 9. 追加验证：Hook DLL 目录迁移(第二阶段)

按要求把原审计里判定"不迁移"的 `%LOCALAPPDATA%\Yumeiren\dll` 一并迁入程序目录。
落点是 `<程序目录>\dll`(与 `.cache` 同级、**不在** `.cache` 内)，理由见审计文档第 5 节。

改动后的路径映射(实测由 `new-build/Yumeiren.exe` 同目录探针程序打印)：

```text
appDir         =C:/Users/rain/Documents/ExplorerBg/Yumeiren/new-build
cacheRoot      =C:/Users/rain/Documents/ExplorerBg/Yumeiren/new-build/.cache
dllRoot        =C:/Users/rain/Documents/ExplorerBg/Yumeiren/new-build/dll
legacyDllRoot  =C:\Users\rain\AppData\Local/Yumeiren/dll
imageDllPath   =.../new-build/dll/ExplorerBgTool/ExplorerBgTool.dll
effectDllPath  =.../new-build/dll/ExplorerBlurMica/ExplorerBlurMica.dll
```

- **投放验证**：工作目录故意设为 `C:\Users\rain\Documents`，调用真实代码
  `Engine::ensureDataDirs()` + `Engine::extractDlls()` → `extractDlls=true`、无错误，
  `dll\ExplorerBgTool\ExplorerBgTool.dll` 221696 B、`dll\ExplorerBlurMica\ExplorerBlurMica.dll`
  427008 B，与旧 AppData 里两份 DLL 字节数一致；`config.ini` 仍由
  `writeImageConfig()`/`writeEffectConfig()` 写在各自 DLL 旁边，外部工具"DLL 同目录"读法不变；
  验证后已删除探针产物与该目录，软件在下一次「应用」时自行重建；
- **启动不写 DLL**：正式跑一次 `YumeirenTest.exe`(工作目录同上，14 s 后结束)，
  `<程序目录>\dll` **未被创建**(未注册时不做无谓投放)，日志 3710 → 5713 字节，新增一行：
  `[10:32:37.173][INFO][tid=7776][Engine] Hook DLL 目录 C:\...\new-build\dll | 图片 未启用 | 特效 未启用`；
- **AppData 零写入**：本次运行前后 `%LOCALAPPDATA%\Yumeiren` 仍是 **11 个文件**，
  含 `dll\` 下 4 项，`LastWriteTime` 全部未变(最新仍是 09-14 20:57)，即新代码完全不碰旧位置；
- **配置回归**：`config\.ini` MD5 运行前后同为 `40FFB6E453242DF1E48C3E2AECC116D4`，
  修改时间仍为 02:34:49；
- **构建**：`cmake --build new-build --target Yumeiren YumeirenTest` 通过，
  仅有两个改动前就存在的 `NOMINMAX redefined` 警告，两个 exe 均正常链接。

**未实测项(须如实记录)**：

1. `stale` 分支(注册表仍指旧目录 → 预投放 + 提示重新应用)本机无法自然触发：HKLM 的
   `Drive\shellex\FolderExtensions\{ED15A97D-…}`、`CLSID\{ED15A97D-…}\InprocServer32`、
   `CLSID\{887D3A6A-…}\InprocServer32` 三处默认值实测均为 `(value not set)`，即两组件当前
   都是"未启用"，`stale` 恒为 `false`。该分支只经过代码走查：判定用 `legacyDllRoot()` 前缀
   比对，提示文案与新状态文本见 `MainWindow::reportDllMigration()`、`Engine::statusText()`。
2. **完整迁移(重新注册到新路径)需要一次真实 UAC 授权**：请在界面上点一次
   「应用图片背景」或「应用特效」。`registerDllInternal()` 会对新路径下的 DLL 执行
   `regsvr32 /s`，直接覆盖同一 CLSID 下登记的旧值，因此点一次即自愈；本轮未做该操作，
   因为它会改写系统注册表并重启资源管理器。
3. 旧 `%LOCALAPPDATA%\Yumeiren\dll` 按要求保留、不删除、不复制；它已不被软件引用，
   确认功能正常后可手工删除。

## 10. 遗留事项

1. **缩略图升级后重建一次**：按要求不复用旧 `AppData\thumbs`，首次进图库多一次 CPU/磁盘开销，
   实测 4 张全部重建且与旧文件字节一致；旧文件仍在原处，需要时可手工删。
2. **`config` 里 `image/customPath` 可能仍指旧 AppData 的 `bg\current_wallpaper.jpg`**：旧
   文件按要求保留，但软件不再把它当缓存；重新点一次"获取桌面壁纸"即可。本机该项为空，无实际影响。
3. **Hook DLL 已迁到 `<程序目录>\dll`，但"注册表指向哪"仍要一次点击收尾**：见第 9 节
   未实测项 2。老用户(已注册)升级后首次启动会看到"旧目录注册(需重新应用)"提示，
   点一次「应用」即可；期间旧 DLL 仍在工作，功能不会中断。
4. **正式 Release 包未实机运行**(见 §1)，发布前建议手工跑一遍 §8 未覆盖项。
5. `new-build\.cache\media` 里两个文件来自旧版 `FolderBgStudio` 的一次性导入(`QFile::copy`
   保留了源文件时间戳)，不是从 `AppData\Local\Yumeiren` 搬来的；若不希望保留这段历史导入行为，
   删 `appinfo.cpp` 的第 2 段(约 20 行)即可。

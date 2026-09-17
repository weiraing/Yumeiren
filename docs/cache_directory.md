# 缓存目录说明(`.cache`)

适用版本：缓存目录迁移之后。审计与"哪些没迁"见
[cache_path_audit.md](cache_path_audit.md)，实测记录见
[cache_migration_test.md](cache_migration_test.md)。

---

## 1. 位置

```text
<Yumeiren.exe 所在目录>\.cache
```

程序目录只取自 `QCoreApplication::applicationDirPath()`，因此缓存位置**与当前工作目录
无关**：双击、命令行、快捷方式、任务计划、从别的目录 `start` 起来，都指向同一处 `.cache`。
代码里不使用 `QDir::currentPath()`，也不使用 `QStandardPaths` 的任何缓存位置。

本机示例：`C:\Users\rain\Documents\ExplorerBg\Yumeiren\new-build\.cache`。

## 2. 子目录用途

| 子目录 | 谁写 | 内容 | 能不能删 |
| --- | --- | --- | --- |
| `.cache\thumbnails` | `mainwindow.cpp: galleryThumbPath()` | 图库缩略图 `<md5>_v2.png` | 可删，进图库自动重建 |
| `.cache\media` | `Engine::bgDir()`(壁纸/图片处理) | `bg_custom.png`、`current_wallpaper.jpg` 等处理后背景图 | 可删，下次应用壁纸重生成 |
| `.cache\logs` | `videodiag.cpp`、`main.cpp` 守卫 | `videowallpaper.log`(追加写，超限时滚成 `.log.old`)、`guard_<pid>.log` | 可删，下次写日志自动重建 |
| `.cache\temp` | `CachePaths::isWritable()` | 可写性探针 `writetest-<pid>.tmp`，用完即删 | 可删 |

`CachePaths::ensureDirectories()` 只创建**确实会被写入**的这四个子目录，不预留空目录。
项目当前没有 Shader 缓存，所以没有 `.cache\shader`(将来接 GPU 缓存时只需在
`subDirNames()` 加一项并新增一个 `CachePaths::shader()` 接口，业务代码不用改)。

## 3. 统一入口

所有缓存路径必须来自 `src/core/CachePaths.h`：

```cpp
#include "core/CachePaths.h"

const QString dir = CachePaths::thumbnails();      // <程序目录>/.cache/thumbnails
const QString log = QDir(CachePaths::logs())      // 诊断日志
                        .filePath(QStringLiteral("videowallpaper.log"));

QString err;
if (!CachePaths::ensureDirectories(&err))         // 首次写入前建目录，幂等
    videodiag::log(err, QStringLiteral("Cache"));

if (!CachePaths::contains(thumbPath)) { /* 越界，拒绝写 */ }
```

约束(硬性)：

- 业务代码**不得**自行拼 `".cache"`，全项目这个字面量只允许出现在
  `CachePaths.cpp` 的 `kCacheDirName`；
- 不得在业务代码里再调 `QStandardPaths::writableLocation(...)`、`QDir::tempPath()`、
  `QDir::currentPath()`、`LOCALAPPDATA` 推断缓存位置；
- 不得在不同模块重复定义缓存根(`videodiag.cpp` 原来自己拼了一份 AppData 路径，已并入
  `CachePaths`)；
- 路径越界判断用 `CachePaths::contains()` / `isInsideProgramDir()`，它们走
  `resolvedPath()`：折叠 `.` `..`、统一 `\` 与 `/`、Windows 忽略大小写，并对
  **已存在的最深祖先**做 `canonicalFilePath()`，因此符号链接/junction 指向程序目录之外
  同样会被判为越界；尚未创建的文件名按原样接回，所以能对"将要写入的路径"提前校验。

## 4. 缓存清理范围

程序目前没有缓存清理入口(审计确认)。将来实现时必须满足：

1. 只遍历 `CachePaths::thumbnails()` / `media()` / `logs()` / `temp()` 这些**明确属于缓存的
   子目录**，逐文件删除；
2. **不要** `QDir(CachePaths::root()).removeRecursively()` 直接删整个 `.cache`，更不得删
   整个程序目录 —— `.cache` 与 `config\`、`media\` 同级，任何"程序目录减缓存"式的写法都可能
   连带删掉配置和用户媒体；
3. 删除前对每个目标路径调用 `CachePaths::contains()`，不通过就跳过；
4. 不得删除 `.cache` 之外的任何文件：配置 `<程序目录>\config\.ini`、注册表
   `HKCU\Software\Yumeiren`、用户图库、播放列表(`config/.ini` 的 `video/playlist`)、
   **`<程序目录>\dll`(Hook DLL 与其 `config.ini`)** 都与缓存无关，清理不得触碰。
   特别强调：`dll` 与 `.cache` **同级**，它虽然也在程序目录里，但不是缓存 ——
   它的绝对路径登记在 HKLM 的 CLSID / FolderExtensions 里，由 `explorer.exe` 加载，
   删掉即壁纸/特效功能失效且必须重新 UAC 注册；只有 HKLM 里已不再引用的
   `%LOCALAPPDATA%\Yumeiren\dll` 才是历史残留，可由用户手工删除；
5. 推荐写法：

```cpp
void clearCacheSubdir(const QString &dir)   // dir 必须来自 CachePaths
{
    if (!CachePaths::contains(dir))
        return;
    QDir d(dir);
    const QFileInfoList entries = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : entries)
        if (fi.isDir())
            fi.dir().removeRecursively();
        else
            QFile::remove(fi.absoluteFilePath());
    d.mkpath(QStringLiteral("."));          // 清完保留空目录
}
```

清理旧 AppData 缓存**不在**本任务范围：`%LOCALAPPDATA%\Yumeiren` 里的历史 `thumbs\`、
`bg\`、`logs\` 按任务书要求原地保留、不迁移、不删除。

## 5. 程序目录不可写时

装在 `C:\Program Files` 这类受保护目录时 `.cache` 建不起来。处理规则：

- 启动时 `main.cpp` 调 `CachePaths::ensureDirectories()`，失败只记一条
  `[Cache] 无法创建缓存目录：...` 诊断；`MainWindow` 构造里再调 `CachePaths::isWritable()`，
  把错误文案显示到界面日志面板："缓存目录不可写：... 请把软件放到可写目录后重试；软件不会改用
  AppData 等其他位置存放缓存。"
- **绝不**静默回退 AppData 或临时目录，也绝不出现"一半写 `.cache`、一半写 AppData"；
- **绝不**改目录权限(不 `SetSecurityInfo`/`icacls`)、**绝不**为写缓存而请求管理员权限；
- 也不因为建不了缓存就改动或重写配置文件；
- 后果：诊断日志本身也在 `.cache\logs`，目录完全建不起来时日志无处可落，只剩界面日志面板
  可见。这是"禁止回退"的直接代价，已记在审计文档第 7 节。

实测(把 `.cache` 换成同名普通文件阻断创建)：程序不崩、功能可用、AppData 没有新增任何文件。

Hook DLL 及其 `config.ini`(已随本次迁移搬到 `<程序目录>\dll`)适用同一套规则：目录建不出来或
DLL 写不进去时，`Engine::extractDlls()` 直接返回"无法写入 <路径>"，由点「应用图片背景」/
「应用特效」的调用方显示到界面日志面板；**不回退** `%LOCALAPPDATA%`、不提前弹 UAC、
不改目录权限。启动阶段只有 HKLM 仍指向旧目录时才预投放 DLL(`MainWindow::reportDllMigration()`)，
因此只读目录下的普通启动不会产生任何写入失败。

附带的安全权衡：HKLM 注册的 DLL 位于用户可写目录时，该目录的属主可以替换被 `explorer.exe`
加载的 DLL。这是便携化(以及"不写 AppData")换来的代价，与缓存迁移本身无关；若要求该 DLL
受系统 ACL 保护，应把软件安装到 `C:\Program Files`(ACL 受保护，同时需要管理员权限投放 DLL)，
而不是退回 AppData。

## 6. Git

`.gitignore` 已忽略 `.cache/`，缓存不进仓库。程序目录下的 `data\image`、`data\video`
是用户媒体目录(图片图库的默认浏览目录、视频播放列表的默认扫描目录)，不是缓存，未受影响。
早期版本的 `media\image`、`media\video` 已废弃，代码里不再有任何引用。

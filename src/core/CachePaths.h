#pragma once

#include <QString>

// 软件自身缓存的唯一路径入口。
//
// 根目录固定 <exe 所在目录>/.cache，位置只取 applicationDirPath()，与当前工作
// 目录无关：换目录启动、快捷方式启动、任务计划启动都指向同一处缓存。
//
// 范围边界(缓存目录迁移，非配置迁移)：
//   - 应用配置仍在 <程序目录>/config/.ini(AppConfig)；
//   - Hook DLL 及其 config.ini 在 <程序目录>\dll(Engine::dllRoot())：它同样离开了
//     %LOCALAPPDATA%，但不放进 .cache —— 注册表里存的是绝对路径，清缓存不能把它
//     一起删掉；
//   - 用户媒体目录仍是 <程序目录>/media/{image,video}。
// 业务代码不得自行拼接 ".cache"，也不得用 QStandardPaths / QDir::tempPath() /
// QDir::currentPath() 推断缓存位置。
class CachePaths
{
public:
    static QString root();           // <程序目录>/.cache
    static QString galleryThumbs();  // .cache/gallery-thumbs  图库缩略图(哈希命名)
    // .cache/model-thumbs  看板娘模型预览图(离屏渲染的透明底 PNG，一模型一张)。
    // 不并进 gallery-thumbs：那边按「绝对路径+修改时间哈希」命名，这边按「模型
    // 文件夹名」命名，混在一处将来按规则清理必然互相误伤。
    static QString modelThumbs();
    static QString renderedBg();     // .cache/rendered-bg  处理后/转存的背景图(单图模式)
    // .cache/image-pool  随机模式的背景图池。必须与 rendered-bg 平级而非其子目录：
    // Hook DLL 只按 folder= 取图，单图模式的 folder 指向 rendered-bg，池子放外面
    // 才能保证两种模式互不串台。
    static QString imagePool();
    static QString logs();           // .cache/logs  诊断日志
    // .cache/web-profile  WebView2 浏览器配置(网页壁纸)。归清缓存管：可再生的
    // 用户级数据(重新登录即可)，不放在 data/ 是因为浏览器缓存动辄上百 MB。
    static QString webProfile();
    // .cache/web-snapshot  网页壁纸快照模式的静态页(截图 PNG + 展示 HTML)。
    // 归清缓存管：内容随时可以重新截取。
    static QString webSnapshot();

    // 创建根目录与实际用到的子目录。幂等。失败时给出明确原因，绝不回退 AppData。
    static bool ensureDirectories(QString *errorMessage = nullptr);

    // 把一个缓存子目录按「容量上限」修剪到不超过 maxBytes：超了就按**最后修改时间**
    // 从旧到新删，直到降到上限以下。返回删除的文件数（失败/无需修剪返回 0）。
    //
    // ⚠️ 只对「文件数会无限增长」的缓存目录用。判据是：缓存键**含内容/时间成分**
    // （如图片浏览的「路径+时间」键）—— 素材一换就是新键，旧文件再没人读，只增不减。
    // **反例：看板娘模型缩略图**的键是「模型目录相对路径」，不含内容指纹，模型改了是
    // 原地覆盖 ⇒ 文件数天然等于模型数，是正当工作集。对它做 LRU 会每轮删掉一半有效图、
    // 再全部重画。用之前先确认键是内容相关的。
    //
    // 判据用 mtime 而非 atime：NTFS 默认关闭 atime 更新，读它只会得到一组恒定的值。
    //
    // 只删 dir 的**直接子文件**，不递归、不碰子目录：缓存目录的布局由各自的 Cache 类
    // 决定，这里不做超出约定的清理。dir 不在 .cache 之内时一律拒绝（返回 0）。
    static int pruneDirectory(const QString &dir, qint64 maxBytes);

    // 按"写探针 → 关闭 → 删除探针"验证可写。不改目录权限，不提权。
    static bool isWritable(QString *errorMessage = nullptr);

    // 规范化(展开 "." ".."、统一分隔符、忽略大小写)后是否落在 .cache 内。
    static bool contains(const QString &path);

    // 规范化并解析符号链接/junction 后是否仍在 exe 所在目录内：.cache 被链到
    // 程序目录之外时同样判越界。
    static bool isInsideProgramDir(const QString &path);

private:
    CachePaths() = delete;
};

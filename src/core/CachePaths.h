#pragma once

#include <QString>

// 软件自身缓存的唯一路径入口。
//
// 根目录固定为 <Yumeiren.exe 所在目录>/.cache，程序目录只取自
// QCoreApplication::applicationDirPath()，与当前工作目录无关；换目录启动、
// 快捷方式启动、任务计划启动都指向同一处缓存。
//
// 范围边界(缓存目录迁移，非配置迁移)：
//   - 应用配置仍是 <程序目录>/config/.ini(AppConfig，未改动)；
//   - Hook DLL 及其 config.ini 在 <程序目录>\dll(由 Engine::dllRoot() 管理)。
//     它同样离开了 %LOCALAPPDATA%，但不放进 .cache：注册表里存的是绝对路径，
//     清理缓存不能把它一起删掉；
//   - 用户媒体目录仍是 <程序目录>/media/{image,video}。
// 业务代码不得自行拼接 ".cache"，也不得再用 QStandardPaths / QDir::tempPath()
// / QDir::currentPath() 推断缓存位置。
class CachePaths
{
public:
    static QString root();        // <程序目录>/.cache
    static QString thumbnails();  // .cache/thumbnails  图库缩略图缓存
    // .cache/model-thumbs  看板娘「模型」模块的静态预览图缓存。
    //
    // 为什么不并进 thumbnails：那个目录的命名是「绝对路径+修改时间的哈希」，
    // 这里的命名是「模型文件夹名」。两种命名混在一个目录里，将来按规则清理
    // 时必然互相误伤(哈希名删不掉、模型名被当成孤儿)。
    //
    // 文件是 Cubism 离屏渲染出的透明底 PNG，一张约几十 KB，一个模型一张。
    static QString modelThumbs();
    static QString media();       // .cache/media       处理后/转存的背景图缓存
    // .cache/bg_random  「随机」模式的背景图池。与 media 平级而不是它的子目录：
    // Hook DLL 只按 folder= 目录取图，单图模式的 folder 指向 media，池子放外面
    // 才能保证两种模式互不串台。
    static QString imagePool();
    static QString logs();        // .cache/logs        诊断日志
    static QString temp();        // .cache/temp        临时与中间文件(含可写性探针)

    // 创建根目录与实际用到的子目录。幂等，可在首次写入前任意时刻调用。
    // 失败时 errorMessage 给出明确原因；绝不改用其他目录，也绝不回退 AppData。
    static bool ensureDirectories(QString *errorMessage = nullptr);

    // 按“建 temp → 写探针 → 关闭 → 删除探针”验证可写，返回 false 时 errorMessage
    // 说明原因并提示把软件移到可写目录。不修改目录权限，不请求管理员权限。
    static bool isWritable(QString *errorMessage = nullptr);

    // path 规范化(展开 "." ".."、统一分隔符、Windows 忽略大小写)后是否落在
    // .cache 内；用于路径越界校验。
    static bool contains(const QString &path);

    // path 规范化并解析符号链接/junction 后是否仍在 exe 所在目录内。
    // .cache 被链接到程序目录之外时同样判为越界。
    static bool isInsideProgramDir(const QString &path);

private:
    CachePaths() = delete;
};

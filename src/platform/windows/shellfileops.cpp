#include "platform/windows/shellfileops.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QString>
#include <QThread>

#include <string>

// 与 VideoWallpaper.cpp 同一处理：mingw 的头文件里已经定义过 NOMINMAX，
// 无条件 define 会换来一条 redefined 警告。这里只做「没定义才定义」。
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>

namespace {

// SHFileOperationW 的错误码是把 DE_* 常量按字符拼成的 DWORD（例如 0x78 表示
// 'x'）。整个表有二十来个，全翻译一遍没意义 —— 只挑真会撞上的那几个，
// 其余统一报十六进制值，至少能拿去搜。
QString describeShellError(int code)
{
    switch (code) {
    case 0x71: return QStringLiteral("源路径与目标路径相同");
    case 0x74: return QStringLiteral("无法删除：文件正在被使用");
    case 0x78: return QStringLiteral("拒绝访问：文件被占用或权限不足");
    case 0x7C: return QStringLiteral("路径无效");
    case 0x7D: return QStringLiteral("路径过长");
    case 0x80: return QStringLiteral("找不到文件或目录");
    case 0x81: return QStringLiteral("路径中包含非法字符");
    case 0x82: return QStringLiteral("目录非空或路径非法");
    case 0x83: return QStringLiteral("目标已存在");
    case 0x84: return QStringLiteral("文件太大，回收站放不下");
    case 0x85: return QStringLiteral("路径无效或包含通配符");
    case 0x86: return QStringLiteral("目标已被占用");
    case 0x87: return QStringLiteral("源位于回收站内，无法再次删除");
    case 0x10000: return QStringLiteral("未知错误");
    default: break;
    }
    return QStringLiteral("系统错误 0x%1").arg(code, 0, 16);
}

// 目录里还有没有东西（递归）。**只用来措辞**，不参与成败判定 ——
// 失败时得能分清「一点没动」和「已经删掉大半」，否则界面会拿「模型文件未被改动」
// 去骗一个模型已经被毁掉的用户。
bool directoryHasContent(const QString &dir)
{
    QDirIterator it(dir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    return it.hasNext();
}

} // namespace

namespace fbswin {

bool moveToRecycleBin(const QString &absolutePath, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    if (absolutePath.trimmed().isEmpty())
        return fail(QStringLiteral("路径为空"));

    const QFileInfo info(absolutePath);
    if (!info.isAbsolute())
        return fail(QStringLiteral("只接受绝对路径"));
    if (!info.exists())
        return fail(QStringLiteral("路径不存在"));
    // 驱动器根目录（"C:\" 这类）不该整盘丢进回收站。真发生说明调用方算错了路径，
    // 这种时候宁可报错也不能照做。
    if (QDir(info.absoluteFilePath()).isRoot())
        return fail(QStringLiteral("拒绝删除驱动器根目录"));

    // pFrom 要求「双 \0 结尾的路径列表」，单个路径也要补两个 \0；
    // 且必须是反斜杠形式，正斜杠会被判成非法字符。
    std::wstring buffer =
        QDir::toNativeSeparators(info.absoluteFilePath()).toStdWString();
    buffer.push_back(L'\0');
    buffer.push_back(L'\0');

    // 失败要重试。**这不是保险，是实测必须**。
    //
    // 2026-09-18 实测（tools/winprobe/rb_retry_probe.py，同一个复制出来的真模型连做
    // 5 轮「复制→删」）：4 轮第一次都返回 0x78(拒绝访问)，把 47 个文件**全部**送进了
    // 回收站，只留下一个**空的顶层目录**；隔 150ms 再删一次，4 轮**全部**干净通过
    // （第 5 轮第一次就过）。也就是说拿住那个目录句柄的是个短暂的扫描/索引类的东西，
    // 与我们要删的内容无关 —— 而「只剩空壳」这种中间态，光看返回码会误判成
    // 「什么都没删」。
    //
    // 不重试的后果很具体：界面弹「删除失败，模型文件未被改动」，而模型其实已经被
    // 毁掉了（网格刷新后它消失、下次扫描也不见）—— 用户会以为还能重来一次。
    constexpr int kMaxAttempts = 3;
    constexpr int kRetryDelayMs = 150;
    int rc = 0;
    bool aborted = false;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = buffer.c_str();
        // ALLOWUNDO 才是「进回收站」；NOCONFIRMATION 是因为确认已经由本程序的界面
        // 做过一次了，再弹一个系统的只会让人以为要删两次。SILENT/NOERRORUI 关掉进度条
        // 与系统错误框 —— 失败原因由我们转达，风格才和界面一致。
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

        rc = SHFileOperationW(&op);
        aborted = op.fAnyOperationsAborted != FALSE;

        // ---- 判据是文件系统，不是返回码 ----
        //
        // 2026-09-18 实测：**同一段代码、同样的参数，返回值会因目标路径而异**。删
        // %TEMP% 下的目录返回 0，删 C:\Users\rain\... 或 C:\ProgramData\... 下的目录
        // 却稳定返回 2(ERROR_FILE_NOT_FOUND) —— 而目录**确实进了回收站**：逐次核对过
        // 回收站 $I 记录，每一次 rc=2 的调用都留下了一条时间戳对应的记录。怀疑是实时
        // 防护/索引服务在目录刚建好时短暂持有句柄，SHFileOperation 事后报「找不到」，
        // 但移动其实已经完成。
        //
        // 所以别拿 rc 当判据 —— 拿它当判据的后果是：明明删成功了，却弹一个「删除失败」
        // 的框，还顺带跳过缩略图清理。**东西没了就是删掉了**。
        //
        // 曾经想过查回收站条目数，来区分「进了回收站」和「回收站装不下被永久删除」。
        // **别再这么干**：SHQueryRecycleBinW 要遍历整个回收站，本机（25764 个条目 /
        // 26 GB）实测**单次约 7 秒**，前后各查一次就是 14 秒，删除会像卡死。而且回收站
        // 装不下时 Windows 的降级删除本来就不是我们能拦住的。
        if (!QFileInfo::exists(info.absoluteFilePath()))
            return true;

        if (attempt < kMaxAttempts)
            QThread::msleep(kRetryDelayMs);
    }

    // 重试过还是没删干净。这时**不能**说「什么都没删」—— 上面那条路径最可能的结果
    // 就是「文件全没了、只剩空目录」，得让调用方和用户都知道这个区别。
    if (!directoryHasContent(info.absoluteFilePath()))
        return fail(QStringLiteral("目录内容已删除，但空目录被占用，没能删掉它"));

    if (aborted)
        return fail(QStringLiteral("操作被中断，未删除任何内容"));
    if (rc == 0)
        return fail(QStringLiteral("没有删除任何内容"));
    return fail(describeShellError(rc));
}

} // namespace fbswin

#include "platform/windows/shellfileops.h"

#include <QDir>
#include <QFileInfo>
#include <QString>

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

    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = buffer.c_str();
    // ALLOWUNDO 才是「进回收站」；NOCONFIRMATION 是因为确认已经由本程序的对话框
    // 做过一次了，再弹一个系统的只会让人以为要删两次。SILENT/NOERRORUI 关掉进度条
    // 与系统错误框 —— 失败原因由我们转达，风格才和界面一致。
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

    const int rc = SHFileOperationW(&op);
    if (rc != 0)
        return fail(describeShellError(rc));
    // rc==0 也可能什么都没删（用户在系统层面取消了），要单独看这个标志。
    if (op.fAnyOperationsAborted)
        return fail(QStringLiteral("操作被中断，未删除任何内容"));

    return true;
}

} // namespace fbswin

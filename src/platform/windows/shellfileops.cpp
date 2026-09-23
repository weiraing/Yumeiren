#include "platform/windows/shellfileops.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QString>
#include <QThread>

#include <string>

// mingw 的头文件里可能已定义 NOMINMAX，无条件 define 会换来一条 redefined 警告
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>

namespace {


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


bool directoryHasContent(const QString &dir)
{
    QDirIterator it(dir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    return it.hasNext();
}

} // namespace

namespace winhelper {

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
    // 驱动器根目录不该整盘丢进回收站；真发生是调用方算错路径，宁可报错
    if (QDir(info.absoluteFilePath()).isRoot())
        return fail(QStringLiteral("拒绝删除驱动器根目录"));

    std::wstring buffer =
        QDir::toNativeSeparators(info.absoluteFilePath()).toStdWString();
    buffer.push_back(L'\0');
    buffer.push_back(L'\0');

    constexpr int kMaxAttempts = 3;
    constexpr int kRetryDelayMs = 150;
    int rc = 0;
    bool aborted = false;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = buffer.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

        rc = SHFileOperationW(&op);
        aborted = op.fAnyOperationsAborted != FALSE;

        if (!QFileInfo::exists(info.absoluteFilePath()))
            return true;

        if (attempt < kMaxAttempts)
            QThread::msleep(kRetryDelayMs);
    }

    // 重试后仍未删净：最可能是"文件全没了、只剩空目录"，措辞不能写成"什么都没删"
    if (!directoryHasContent(info.absoluteFilePath()))
        return fail(QStringLiteral("目录内容已删除，但空目录被占用，没能删掉它"));

    if (aborted)
        return fail(QStringLiteral("操作被中断，未删除任何内容"));
    if (rc == 0)
        return fail(QStringLiteral("没有删除任何内容"));
    return fail(describeShellError(rc));
}

} // namespace winhelper

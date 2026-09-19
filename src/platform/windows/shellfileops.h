#ifndef SHELLFILEOPS_H
#define SHELLFILEOPS_H

#include <QtCore/qglobal.h>

class QString;

namespace fbswin {

// 把文件或目录整体移入回收站(可撤销)，成功返回 true。

bool moveToRecycleBin(const QString &absolutePath, QString *error);

} // namespace fbswin

#endif // SHELLFILEOPS_H

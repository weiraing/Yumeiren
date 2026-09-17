#ifndef CUBISMRUNTIME_H
#define CUBISMRUNTIME_H

#include <QString>

namespace kanban {

struct KanbanGlHost;

// 仅供 Cubism 后端使用；SDK 类型和进程级状态不进入头文件。
namespace cubismruntime {

bool initializeFramework(QString *outError);
void logInfo(const QString &text);
void logWarn(const QString &text);
void logDebug(const QString &text);

// 必须在创建渲染器前调用；generation 为宿主上下文的世代号。
void syncShaderCache(quint64 generation);

// 只归还本作用域借用的上下文，不打断宿主正在进行的绘制。
class GlScope final
{
public:
    GlScope(KanbanGlHost *host, bool *glewReady);
    ~GlScope();
    GlScope(const GlScope &) = delete;
    GlScope &operator=(const GlScope &) = delete;

    bool ok() const { return m_ok; }

private:
    KanbanGlHost *m_host = nullptr;
    bool m_owned = false;
    bool m_ok = false;
};

} // namespace cubismruntime
} // namespace kanban

#endif // CUBISMRUNTIME_H

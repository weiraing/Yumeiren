// 「离屏 GL 宿主」——把一对 QOpenGLContext/QOffscreenSurface 包装成 KanbanGlHost，
// 于是渲染器察觉不到自己不在 QOpenGLWidget 里。
//
// 为什么单独抽成一个头文件：tools/kanbanprobe 的渲染自检与模型预览图生成进程
// (ModelThumbJob) 都需要它，两边必须给出**逐位一致**的渲染结果，各写一份迟早漂移。
//
// 刻意没有重写 glContextGeneration()：默认返回 0，渲染器据此认定「没有上下文」，
// syncCubismShaderCache() 直接返回，不会去动那份**进程级**的 Cubism 着色器缓存。
// 本类只用于「本进程唯一的那个上下文」，不承担换上下文的责任。
#ifndef KANBANOFFSCREENGLHOST_H
#define KANBANOFFSCREENGLHOST_H

#include "kanban/KanbanRenderer.h"

#include <QOpenGLContext>
#include <QOffscreenSurface>

namespace kanban {

class OffscreenGlHost : public KanbanGlHost
{
public:
    OffscreenGlHost(QOpenGLContext *context, QOffscreenSurface *surface, const QSize &pixels)
        : m_context(context), m_surface(surface), m_pixels(pixels)
    {
    }

    bool glHostReady() const override { return m_context && m_context->isValid(); }
    bool glIsCurrent() const override { return QOpenGLContext::currentContext() == m_context; }
    bool glMakeCurrent() override { return m_context->makeCurrent(m_surface); }
    void glDoneCurrent() override { m_context->doneCurrent(); }
    QSize glPixelSize() const override { return m_pixels; }

private:
    QOpenGLContext *m_context = nullptr;
    QOffscreenSurface *m_surface = nullptr;
    QSize m_pixels;
};

} // namespace kanban

#endif // KANBANOFFSCREENGLHOST_H

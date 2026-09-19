// 软件绘制视图实现。
#include "kanban/KanbanSoftwareView.h"

#include "kanban/KanbanRenderer.h"

#include <QPainter>
#include <QResizeEvent>

namespace kanban {

KanbanSoftwareView::KanbanSoftwareView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("KanbanSoftwareView"));
    // 透明性由顶层窗口的 WA_TranslucentBackground 提供，这里只要不画背景填充
    // (默认 autoFillBackground=false 即满足)。
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
}

void KanbanSoftwareView::setRenderer(KanbanRenderer *renderer)
{
    m_renderer = renderer;
    if (m_renderer) {
        m_renderer->resize(width(), height(), float(devicePixelRatioF()));
    }
}

void KanbanSoftwareView::requestFrame()
{
    update();
}

void KanbanSoftwareView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    if (!m_renderer || m_renderer->usesOpenGL()) {
        return;
    }
    QPainter painter(this);
    m_renderer->paint(&painter, QSize(width(), height()));
}

void KanbanSoftwareView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_renderer) {
        m_renderer->resize(width(), height(), float(devicePixelRatioF()));
    }
}

} // namespace kanban

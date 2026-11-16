#include "library_ui/SymbolPreview.h"

#include "library_ui/LibraryUiCommon.h"

#include <QPainter>
#include <QPainterPath>

namespace lvd {

SymbolPreview::SymbolPreview(QWidget *parent) : QWidget(parent)
{
    m_views.append(View{});
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void SymbolPreview::setSymbol(const Symbol &symbol)
{
    m_symbol = symbol;
    m_hasSymbol = true;
    update();
}

void SymbolPreview::clear()
{
    m_symbol = Symbol();
    m_hasSymbol = false;
    update();
}

void SymbolPreview::setViews(const QList<View> &views)
{
    m_views = views.isEmpty() ? QList<View>{View{}} : views;
    update();
}

void SymbolPreview::setShowPinNames(bool show)
{
    m_showPinNames = show;
    update();
}

void SymbolPreview::setMaximumScale(double scale)
{
    m_maximumScale = scale;
    update();
}

void SymbolPreview::setPlaceholderText(const QString &text)
{
    m_placeholder = text;
    update();
}

QSize SymbolPreview::sizeHint() const
{
    return QSize(160 * static_cast<int>(m_views.size()), 150);
}

QSize SymbolPreview::minimumSizeHint() const
{
    return QSize(60 * static_cast<int>(m_views.size()), 70);
}

void SymbolPreview::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const SchematicStyle style = styleFor(this);

    const QRectF area = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath frame;
    frame.addRoundedRect(area, 4, 4);
    painter.fillPath(frame, style.background);
    painter.setPen(QPen(palette().color(QPalette::Mid), 1));
    painter.drawPath(frame);

    if (!m_hasSymbol) {
        painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        painter.drawText(area, Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
        return;
    }

    const int count = static_cast<int>(m_views.size());
    const double cellWidth = area.width() / count;
    const QFontMetrics metrics(font());
    for (int i = 0; i < count; ++i) {
        const View &view = m_views.at(i);
        QRectF cell(area.left() + i * cellWidth, area.top(), cellWidth, area.height());
        if (i > 0) {
            painter.setPen(QPen(palette().color(QPalette::Midlight), 1, Qt::DotLine));
            painter.drawLine(QPointF(cell.left(), cell.top() + 6), QPointF(cell.left(), cell.bottom() - 6));
        }
        if (!view.caption.isEmpty()) {
            const QRectF captionRect(cell.left(), cell.bottom() - metrics.height() - 4, cell.width(), metrics.height());
            painter.setPen(style.value);
            painter.drawText(captionRect, Qt::AlignCenter, view.caption);
            cell.setBottom(captionRect.top());
        }
        paintSymbolFitted(painter, m_symbol, cell.adjusted(6, 6, -6, -6), style, view.orientation, m_showPinNames,
                          m_maximumScale);
    }
}

} // namespace lvd

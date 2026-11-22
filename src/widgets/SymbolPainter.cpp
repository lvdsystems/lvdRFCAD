#include "widgets/SymbolPainter.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QTransform>

#include <cmath>

namespace lvd {

namespace {

QPointF directionVector(PinDirection direction)
{
    switch (direction) {
    case PinDirection::Left: return QPointF(-1, 0);
    case PinDirection::Right: return QPointF(1, 0);
    case PinDirection::Up: return QPointF(0, -1);
    case PinDirection::Down: return QPointF(0, 1);
    }
    return {};
}

QRectF circleRect(QPoint center, int radius)
{
    return QRectF(center.x() - radius, center.y() - radius, 2.0 * radius, 2.0 * radius);
}

} // namespace

QTransform symbolTransform(QPointF position, const Orientation &orientation)
{
    // Same mapping as Orientation::map: mirror x, then rotate counter-clockwise on screen.
    const QPoint ex = orientation.map(QPoint(1, 0));
    const QPoint ey = orientation.map(QPoint(0, 1));
    return QTransform(ex.x(), ex.y(), ey.x(), ey.y(), position.x(), position.y());
}

void SymbolPainter::paint(QPainter &painter, const Symbol &symbol, const Options &options) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor outline = options.missing ? m_style.missing : (options.selected ? m_style.selection : m_style.symbol);
    QPen pen(outline, m_style.lineWidth);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCosmetic(false);

    const auto brushFor = [&](Fill fill) {
        switch (fill) {
        case Fill::Outline: return QBrush(outline);
        case Fill::Background: return QBrush(m_style.symbolFill);
        case Fill::None: break;
        }
        return QBrush(Qt::NoBrush);
    };

    for (const SymbolGraphic &graphic : symbol.graphics) {
        painter.setPen(pen);
        switch (graphic.type) {
        case SymbolGraphic::Type::Line: {
            QPolygonF line;
            for (const QPoint &point : graphic.points)
                line.append(point);
            painter.drawPolyline(line);
            break;
        }
        case SymbolGraphic::Type::Polygon: {
            QPolygonF polygon;
            for (const QPoint &point : graphic.points)
                polygon.append(point);
            painter.setBrush(brushFor(graphic.fill));
            painter.drawPolygon(polygon);
            break;
        }
        case SymbolGraphic::Type::Rect:
            painter.setBrush(brushFor(graphic.fill));
            painter.drawRect(QRectF(QPointF(graphic.from), QPointF(graphic.to)).normalized());
            break;
        case SymbolGraphic::Type::Circle:
            painter.setBrush(brushFor(graphic.fill));
            painter.drawEllipse(circleRect(graphic.center, graphic.radius));
            break;
        case SymbolGraphic::Type::Arc: {
            // Angles are counter-clockwise on screen, as Qt's drawArc expects.
            painter.setBrush(Qt::NoBrush);
            painter.drawArc(circleRect(graphic.center, graphic.radius), qRound(graphic.startAngle * 16),
                            qRound(graphic.spanAngle * 16));
            break;
        }
        case SymbolGraphic::Type::Text:
            paintText(painter, graphic.at, graphic.text, graphic.size, graphic.anchor,
                      options.selected ? m_style.selection : m_style.pinName, options.orientation);
            break;
        }
    }

    // Pins: a line from the connection point toward the body.
    QPen pinPen(options.missing ? m_style.missing : (options.selected ? m_style.selection : m_style.pin), m_style.lineWidth);
    pinPen.setCapStyle(Qt::FlatCap);
    for (const SymbolPin &pin : symbol.pins) {
        const QPointF start = pin.at;
        const QPointF end = start + directionVector(pin.direction) * pin.length;
        painter.setPen(pinPen);
        if (pin.length > 0)
            painter.drawLine(start, end);
        if (options.unconnectedPins.contains(pin.pin)) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(m_style.warning, 1.0));
            painter.drawEllipse(start, m_style.pinDotRadius * 1.6, m_style.pinDotRadius * 1.6);
        }
        if (options.showPinNames) {
            const QPointF labelAt = end + directionVector(pin.direction) * 3;
            const bool horizontal = pin.direction == PinDirection::Left || pin.direction == PinDirection::Right;
            TextAnchor anchor = TextAnchor::Center;
            if (horizontal)
                anchor = pin.direction == PinDirection::Right ? TextAnchor::Left : TextAnchor::Right;
            paintText(painter, labelAt, pin.pin, 6, anchor, m_style.pinName, options.orientation);
        }
    }
    painter.restore();
}

void SymbolPainter::paintText(QPainter &painter, QPointF at, const QString &text, double size, TextAnchor anchor,
                              const QColor &color, const Orientation &orientation) const
{
    if (text.isEmpty())
        return;
    painter.save();
    // Undo the symbol orientation around the anchor point so text reads normally.
    const QTransform symbolToSheet = symbolTransform(QPointF(), orientation);
    const QTransform upright = symbolToSheet.inverted();
    painter.translate(at);
    painter.setTransform(upright, true);

    // Anchors refer to the text as it appears on screen. A mirrored or
    // rotated symbol swaps its left and right sides.
    TextAnchor effective = anchor;
    if (anchor != TextAnchor::Center) {
        const QPoint mapped = orientation.map(QPoint(anchor == TextAnchor::Left ? 1 : -1, 0));
        if (mapped.x() != 0)
            effective = mapped.x() > 0 ? TextAnchor::Left : TextAnchor::Right;
        else
            effective = TextAnchor::Center; // vertical: centre on the point
    }

    // Draw at a fixed point size and scale, so text size follows the drawing units.
    const QFont font = m_style.font(10);
    // Metrics of the device painted on: a printer or PDF writer has another
    // resolution than the screen, and the point-size font scales with it.
    const QFontMetricsF metrics = painter.device() ? QFontMetricsF(font, painter.device()) : QFontMetricsF(font);
    const double scale = size / metrics.height() * 1.25;
    painter.scale(scale, scale);
    painter.setFont(font);
    painter.setPen(color);
    const double width = metrics.horizontalAdvance(text);
    double x = 0;
    if (effective == TextAnchor::Center)
        x = -width / 2;
    else if (effective == TextAnchor::Right)
        x = -width;
    const double y = (metrics.ascent() - metrics.descent()) / 2; // vertically centred on the point
    painter.drawText(QPointF(x, y), text);
    painter.restore();
}

QRectF SymbolPainter::boundingRect(const Symbol &symbol)
{
    QRectF rect = QRectF(symbol.boundingRect());
    for (const SymbolPin &pin : symbol.pins) {
        const QPointF end = QPointF(pin.at) + directionVector(pin.direction) * pin.length;
        rect = rect.united(QRectF(end, QSizeF(0.1, 0.1)));
    }
    return rect.adjusted(-2, -2, 2, 2);
}

QPainterPath SymbolPainter::shape(const Symbol &symbol)
{
    QPainterPath path;
    path.addRect(boundingRect(symbol));
    return path;
}

void SymbolPainter::paintPreview(QPainter &painter, const Symbol &symbol, const QRectF &target) const
{
    const QRectF bounds = boundingRect(symbol);
    if (bounds.isEmpty() || target.isEmpty())
        return;
    const double scale = std::min(target.width() / bounds.width(), target.height() / bounds.height()) * 0.9;
    painter.save();
    painter.translate(target.center());
    painter.scale(scale, scale);
    painter.translate(-bounds.center());
    paint(painter, symbol);
    painter.restore();
}

} // namespace lvd

#pragma once

#include "core/ComponentDefinition.h"
#include "widgets/SchematicStyle.h"

#include <QRectF>
#include <QSet>
#include <QString>

class QPainter;
class QPainterPath;

namespace lvd {

// Draws component symbols. Coordinates are symbol coordinates; the caller
// sets the painter transform for position, zoom and orientation (see
// symbolTransform). Text stays upright and readable whatever the orientation.
class SymbolPainter
{
public:
    explicit SymbolPainter(const SchematicStyle &style) : m_style(style) {}

    struct Options {
        Orientation orientation;          // used to keep text upright
        bool selected = false;
        bool missing = false;             // draw in the missing colour
        QSet<QString> unconnectedPins;    // pins drawn with an open marker
        bool showPinNames = false;
    };

    // Graphics and pins.
    void paint(QPainter &painter, const Symbol &symbol, const Options &options) const;
    void paint(QPainter &painter, const Symbol &symbol) const { paint(painter, symbol, Options{}); }

    // Text at a symbol point, kept upright under the given orientation.
    void paintText(QPainter &painter, QPointF at, const QString &text, double size, TextAnchor anchor,
                   const QColor &color, const Orientation &orientation) const;

    // Area covered by the symbol, including pins, in symbol coordinates.
    static QRectF boundingRect(const Symbol &symbol);
    // Outline for hit testing, in symbol coordinates.
    static QPainterPath shape(const Symbol &symbol);

    // Draws a symbol scaled to fit a rectangle, for previews and icons.
    void paintPreview(QPainter &painter, const Symbol &symbol, const QRectF &target) const;

private:
    SchematicStyle m_style;
};

// Painter transform from symbol to sheet coordinates for a placed component.
QTransform symbolTransform(QPointF position, const Orientation &orientation);

} // namespace lvd

#pragma once

#include <QColor>
#include <QFont>

class QPalette;

namespace lvd {

// Colours and sizes for drawing schematics and symbols, in both light and
// dark themes. Sizes are in drawing units (the grid is 10 units).
struct SchematicStyle {
    QColor background;
    QColor grid;
    QColor symbol;        // symbol outlines
    QColor symbolFill;    // Fill::Background
    QColor pin;
    QColor pinName;
    QColor wire;
    QColor junction;
    QColor label;
    QColor text;
    QColor reference;
    QColor value;
    QColor selection;
    QColor highlight;     // highlighted net
    QColor error;
    QColor warning;
    QColor missing;       // components whose definition is missing

    double lineWidth = 1.5;
    double wireWidth = 1.5;
    double junctionRadius = 3.0;
    double pinDotRadius = 1.5; // unconnected pin marker
    double referenceTextSize = 9;
    double valueTextSize = 8;

    QFont font(double size) const;

    // Chooses the light or dark variant from the palette's window colour.
    static SchematicStyle fromPalette(const QPalette &palette);
    static SchematicStyle light();
    static SchematicStyle dark();
};

} // namespace lvd

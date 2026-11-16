#pragma once

#include <QColor>

#include <array>

class QPalette;

namespace lvd {

// Colours of a plot, derived from the application palette so plots follow the
// light or dark theme.
struct PlotTheme {
    bool dark = false;
    QColor surround;   // outside the plot area
    QColor background; // plot area
    QColor text;
    QColor mutedText;
    QColor axis;
    QColor grid;
    QColor minorGrid;
    QColor smithGrid;
    QColor admittanceGrid;
    QColor crosshair;
    QColor rubberBand;
    QColor auxiliary; // default colour of caller circles
    QColor tableBackground;

    // Colour-blind-safe categorical palette: the same eight hues stepped for
    // the light and the dark surface.
    static constexpr int kSeriesCount = 8;
    std::array<QColor, kSeriesCount> series;

    QColor seriesColor(int slot) const { return series[static_cast<std::size_t>(slot % kSeriesCount)]; }

    static PlotTheme fromPalette(const QPalette &palette);
};

} // namespace lvd

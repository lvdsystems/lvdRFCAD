#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

namespace lvd {

// Continuous colour maps for colour-map plots, 3D patterns and element maps.
// Viridis is perceptually uniform and colour-blind safe (the default); Jet is
// offered because many antenna engineers expect it; Phase is cyclic, for
// phase in degrees; Gray is for printing.
enum class ColorMap { Viridis, Jet, Phase, Gray };

// t in [0, 1]; values outside are clamped. NaN gives an invalid colour.
QColor colorMapColor(ColorMap map, double t);

QString colorMapName(ColorMap map);
QStringList colorMapNames();
ColorMap colorMapFromName(const QString &name); // unknown names give Viridis

} // namespace lvd

#include "plot/ColorMaps.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace lvd {

namespace {

struct Stop {
    double r, g, b;
};

// Viridis at t = 0, 0.1, ..., 1 (matplotlib's table).
constexpr std::array<Stop, 11> kViridis = {{
    {0.267, 0.005, 0.329},
    {0.283, 0.141, 0.458},
    {0.254, 0.265, 0.530},
    {0.207, 0.372, 0.553},
    {0.164, 0.471, 0.558},
    {0.128, 0.567, 0.551},
    {0.135, 0.659, 0.518},
    {0.267, 0.749, 0.441},
    {0.478, 0.821, 0.318},
    {0.741, 0.873, 0.150},
    {0.993, 0.906, 0.144},
}};

constexpr std::array<Stop, 9> kJet = {{
    {0.0, 0.0, 0.5},
    {0.0, 0.0, 1.0},
    {0.0, 0.5, 1.0},
    {0.0, 1.0, 1.0},
    {0.5, 1.0, 0.5},
    {1.0, 1.0, 0.0},
    {1.0, 0.5, 0.0},
    {1.0, 0.0, 0.0},
    {0.5, 0.0, 0.0},
}};

// Cyclic: the first and last stops are equal.
constexpr std::array<Stop, 7> kPhase = {{
    {0.85, 0.20, 0.25},
    {0.90, 0.65, 0.15},
    {0.45, 0.80, 0.30},
    {0.15, 0.70, 0.75},
    {0.25, 0.35, 0.85},
    {0.70, 0.30, 0.80},
    {0.85, 0.20, 0.25},
}};

template<std::size_t N>
QColor interpolate(const std::array<Stop, N> &stops, double t)
{
    const double position = t * double(N - 1);
    const std::size_t i = std::min<std::size_t>(std::size_t(position), N - 2);
    const double f = position - double(i);
    const Stop &a = stops[i];
    const Stop &b = stops[i + 1];
    return QColor::fromRgbF(float(a.r + (b.r - a.r) * f), float(a.g + (b.g - a.g) * f), float(a.b + (b.b - a.b) * f));
}

} // namespace

QColor colorMapColor(ColorMap map, double t)
{
    if (std::isnan(t))
        return {};
    t = std::clamp(t, 0.0, 1.0);
    switch (map) {
    case ColorMap::Viridis:
        return interpolate(kViridis, t);
    case ColorMap::Jet:
        return interpolate(kJet, t);
    case ColorMap::Phase:
        return interpolate(kPhase, t);
    case ColorMap::Gray: {
        const float v = float(0.08 + 0.87 * t);
        return QColor::fromRgbF(v, v, v);
    }
    }
    return {};
}

QString colorMapName(ColorMap map)
{
    switch (map) {
    case ColorMap::Viridis:
        return QStringLiteral("viridis");
    case ColorMap::Jet:
        return QStringLiteral("jet");
    case ColorMap::Phase:
        return QStringLiteral("phase");
    case ColorMap::Gray:
        return QStringLiteral("gray");
    }
    return QStringLiteral("viridis");
}

QStringList colorMapNames()
{
    return {QStringLiteral("viridis"), QStringLiteral("jet"), QStringLiteral("phase"), QStringLiteral("gray")};
}

ColorMap colorMapFromName(const QString &name)
{
    for (ColorMap map : {ColorMap::Viridis, ColorMap::Jet, ColorMap::Phase, ColorMap::Gray})
        if (colorMapName(map) == name.toLower())
            return map;
    return ColorMap::Viridis;
}

} // namespace lvd

#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace lvd {

// IEC 60063 preferred number series for component values (requirement RF-13).
enum class ESeries { E3, E6, E12, E24, E48, E96, E192 };

enum class Rounding { Nearest, Up, Down };

// The mantissas of a series in one decade, from 1.0 to below 10.
const std::vector<double> &seriesMantissas(ESeries series);

// The series value closest to `value` (geometrically), or the next one above
// or below. Non-positive or non-finite values are returned unchanged.
double snapToSeries(double value, ESeries series, Rounding rounding = Rounding::Nearest);

// The neighbours of a value in the series: the largest series value <= value
// and the smallest >= value.
std::pair<double, double> seriesNeighbours(double value, ESeries series);

QString seriesName(ESeries series);
std::optional<ESeries> seriesFromName(const QString &name); // "E24", case-insensitive
std::vector<ESeries> allSeries();

} // namespace lvd

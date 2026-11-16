#include "core/StandardValues.h"

#include <cmath>

namespace lvd {

namespace {

// E3 to E24 have historical values that differ from the rounded geometric
// series; E48 and above follow it to three digits, with one exception in E192.
const std::vector<double> E24 = {1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                                 3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1};

std::vector<double> everyNth(const std::vector<double> &values, size_t step)
{
    std::vector<double> result;
    for (size_t i = 0; i < values.size(); i += step)
        result.push_back(values[i]);
    return result;
}

std::vector<double> geometric(int count)
{
    std::vector<double> result;
    for (int i = 0; i < count; ++i) {
        const double value = std::pow(10.0, static_cast<double>(i) / count);
        result.push_back(std::round(value * 100.0) / 100.0);
    }
    if (count == 192) {
        // IEC 60063: 9.20 instead of the rounded 9.19.
        for (double &value : result) {
            if (std::abs(value - 9.19) < 1e-9)
                value = 9.20;
        }
    }
    return result;
}

} // namespace

const std::vector<double> &seriesMantissas(ESeries series)
{
    static const std::vector<double> e3 = everyNth(E24, 8);
    static const std::vector<double> e6 = everyNth(E24, 4);
    static const std::vector<double> e12 = everyNth(E24, 2);
    static const std::vector<double> e48 = geometric(48);
    static const std::vector<double> e96 = geometric(96);
    static const std::vector<double> e192 = geometric(192);
    switch (series) {
    case ESeries::E3: return e3;
    case ESeries::E6: return e6;
    case ESeries::E12: return e12;
    case ESeries::E24: return E24;
    case ESeries::E48: return e48;
    case ESeries::E96: return e96;
    case ESeries::E192: return e192;
    }
    return E24;
}

std::pair<double, double> seriesNeighbours(double value, ESeries series)
{
    if (!(value > 0) || !std::isfinite(value))
        return {value, value};
    const std::vector<double> &mantissas = seriesMantissas(series);
    const double decade = std::floor(std::log10(value));
    const double scale = std::pow(10.0, decade);
    // Tolerance so that a value already in the series maps to itself.
    const double mantissa = value / scale;
    double below = mantissas.back() / 10.0, above = 10.0;
    for (double m : mantissas) {
        if (m <= mantissa * (1 + 1e-9))
            below = m;
        if (m >= mantissa * (1 - 1e-9)) {
            above = m;
            break;
        }
    }
    return {below * scale, above * scale};
}

double snapToSeries(double value, ESeries series, Rounding rounding)
{
    if (!(value > 0) || !std::isfinite(value))
        return value;
    const auto [below, above] = seriesNeighbours(value, series);
    switch (rounding) {
    case Rounding::Up: return above;
    case Rounding::Down: return below;
    case Rounding::Nearest: break;
    }
    // Geometric distance: compare ratios.
    return value / below <= above / value ? below : above;
}

QString seriesName(ESeries series)
{
    switch (series) {
    case ESeries::E3: return QStringLiteral("E3");
    case ESeries::E6: return QStringLiteral("E6");
    case ESeries::E12: return QStringLiteral("E12");
    case ESeries::E24: return QStringLiteral("E24");
    case ESeries::E48: return QStringLiteral("E48");
    case ESeries::E96: return QStringLiteral("E96");
    case ESeries::E192: return QStringLiteral("E192");
    }
    return {};
}

std::optional<ESeries> seriesFromName(const QString &name)
{
    for (ESeries series : allSeries()) {
        if (seriesName(series).compare(name.trimmed(), Qt::CaseInsensitive) == 0)
            return series;
    }
    return std::nullopt;
}

std::vector<ESeries> allSeries()
{
    return {ESeries::E3, ESeries::E6, ESeries::E12, ESeries::E24, ESeries::E48, ESeries::E96, ESeries::E192};
}

} // namespace lvd

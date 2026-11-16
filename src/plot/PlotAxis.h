#pragma once

#include <QString>

#include <cmath>
#include <limits>

#include <vector>

namespace lvd {

// Tick positions for one axis, in data units, sorted ascending.
struct AxisTicks {
    std::vector<double> major;
    std::vector<double> minor;
    double step = 0; // major step of a linear axis; 0 for decade ticks
};

// "Nice" linear ticks (steps of 1, 2 or 5 times a power of ten) covering
// [lo, hi] with about targetCount major ticks.
AxisTicks linearTicks(double lo, double hi, int targetCount);

// Ticks of a logarithmic axis (lo > 0): decades, with 2 and 5 added when the
// range is short, and linear ticks when it spans less than a decade.
AxisTicks logTicks(double lo, double hi, int targetCount);

// True for units that take SI prefixes (Hz, s, V, A, W, F, H, ohm, S, m).
// dB, dBm, degrees, percent and unitless values are printed without prefix.
bool unitTakesPrefix(const QString &unit);

// Display form of a unit ("ohm" -> the ohm sign, "deg" -> the degree sign).
QString displayUnit(const QString &unit);

// Tick label: an SI prefix chosen per value for prefixable units ("500 MHz",
// "1.5 GHz"), decimals derived from the tick step. step <= 0 prints up to
// three significant digits (log axes). Non-prefixable units print the number
// only; the unit then belongs in the axis title.
QString formatTick(double value, double step, const QString &unit);

// Read-out of a single value with up to `digits` significant digits and its
// unit: "1.2345 GHz", "-3.01 dB", "50 ohm".
QString formatValue(double value, const QString &unit, int digits = 5);

// Maps between data values and pixels along one axis, linear or logarithmic.
class AxisScale
{
public:
    AxisScale() = default;
    AxisScale(double min, double max, bool log) : m_min(min), m_max(max), m_log(log) {}

    double min() const { return m_min; }
    double max() const { return m_max; }
    bool isLog() const { return m_log; }
    void setRange(double min, double max) { m_min = min; m_max = max; }
    void setLog(bool log) { m_log = log; }

    // Pixel range: p0 is where min lands, p1 where max lands (p1 < p0 for y).
    void setPixels(double p0, double p1) { m_p0 = p0; m_p1 = p1; }
    double p0() const { return m_p0; }
    double p1() const { return m_p1; }

    // NaN for values that cannot be shown (<= 0 on a log axis).
    double toPixel(double value) const;
    double fromPixel(double pixel) const;

    // Value in the axis' internal linear space (log10 for log axes).
    double toLinear(double value) const;
    double fromLinear(double t) const;

    // Precomputed value-to-pixel mapping for drawing many points.
    struct Mapper {
        double t0 = 0;
        double k = 1;
        double p0 = 0;
        bool log = false;
        double operator()(double v) const
        {
            if (log)
                return v > 0 ? p0 + (std::log10(v) - t0) * k : std::numeric_limits<double>::quiet_NaN();
            return p0 + (v - t0) * k;
        }
    };
    Mapper mapper() const
    {
        const double t0 = toLinear(m_min);
        const double t1 = toLinear(m_max);
        return {t0, t1 != t0 ? (m_p1 - m_p0) / (t1 - t0) : 0.0, m_p0, m_log};
    }

    // Zooms by factor (< 1 zooms in) keeping `value` at its place.
    void zoom(double factor, double value);
    // Shifts the range by a pixel distance.
    void pan(double pixels);

private:
    double m_min = 0;
    double m_max = 1;
    bool m_log = false;
    double m_p0 = 0;
    double m_p1 = 1;
};

} // namespace lvd

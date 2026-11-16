#include "plot/PlotAxis.h"

#include <QChar>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lvd {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double niceStep(double raw)
{
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    const double normalized = raw / magnitude;
    double nice = 10;
    if (normalized <= 1.0 + 1e-9)
        nice = 1;
    else if (normalized <= 2.0 + 1e-9)
        nice = 2;
    else if (normalized <= 5.0 + 1e-9)
        nice = 5;
    return nice * magnitude;
}

// Engineering exponent (multiple of 3) for |value|, limited to f..T.
int engineeringExponent(double value)
{
    if (value == 0 || !std::isfinite(value))
        return 0;
    const int exponent = static_cast<int>(std::floor(std::log10(std::abs(value)) / 3.0 + 1e-9)) * 3;
    return std::clamp(exponent, -15, 12);
}

QString prefixFor(int exponent)
{
    switch (exponent) {
    case -15: return QStringLiteral("f");
    case -12: return QStringLiteral("p");
    case -9: return QStringLiteral("n");
    case -6: return QString(QChar(0x00B5));
    case -3: return QStringLiteral("m");
    case 3: return QStringLiteral("k");
    case 6: return QStringLiteral("M");
    case 9: return QStringLiteral("G");
    case 12: return QStringLiteral("T");
    default: return {};
    }
}

QString trimZeros(QString text)
{
    if (text.contains(QLatin1Char('.')) && !text.contains(QLatin1Char('e'))) {
        while (text.endsWith(QLatin1Char('0')))
            text.chop(1);
        if (text.endsWith(QLatin1Char('.')))
            text.chop(1);
    }
    if (text == QLatin1String("-0"))
        text = QStringLiteral("0");
    return text;
}

QString withUnit(const QString &number, const QString &prefix, const QString &unit)
{
    const QString shown = displayUnit(unit);
    if (shown.isEmpty() && prefix.isEmpty())
        return number;
    if (shown == QString(QChar(0x00B0)) && prefix.isEmpty())
        return number + shown;
    return number + QLatin1Char(' ') + prefix + shown;
}

int decimalsForStep(double step)
{
    return std::clamp(static_cast<int>(std::ceil(-std::log10(step) - 1e-9)), 0, 9);
}

int decimalsForDigits(double value, int digits)
{
    if (value == 0)
        return 0;
    return std::clamp(digits - 1 - static_cast<int>(std::floor(std::log10(std::abs(value)))), 0, 9);
}

} // namespace

AxisTicks linearTicks(double lo, double hi, int targetCount)
{
    AxisTicks ticks;
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo)
        return ticks;
    targetCount = std::max(1, targetCount);
    const double step = niceStep((hi - lo) / targetCount);
    if (!(step > 0))
        return ticks;
    ticks.step = step;
    const double eps = step * 1e-6;
    const auto first = static_cast<long long>(std::ceil((lo - eps) / step));
    const auto last = static_cast<long long>(std::floor((hi + eps) / step));
    for (long long k = first; k <= last && ticks.major.size() < 1000; ++k) {
        const double v = static_cast<double>(k) * step;
        ticks.major.push_back(std::abs(v) < eps ? 0.0 : v);
    }
    // Minor ticks: quarters of a 2-step, fifths otherwise.
    const double normalized = step / std::pow(10.0, std::floor(std::log10(step) + 1e-9));
    const int divisions = std::abs(normalized - 2.0) < 1e-6 ? 4 : 5;
    const double minorStep = step / divisions;
    const auto minorFirst = static_cast<long long>(std::ceil((lo - eps) / minorStep));
    const auto minorLast = static_cast<long long>(std::floor((hi + eps) / minorStep));
    for (long long k = minorFirst; k <= minorLast && ticks.minor.size() < 5000; ++k) {
        if (k % divisions != 0)
            ticks.minor.push_back(static_cast<double>(k) * minorStep);
    }
    return ticks;
}

AxisTicks logTicks(double lo, double hi, int targetCount)
{
    AxisTicks ticks;
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo <= 0 || hi <= lo)
        return ticks;
    targetCount = std::max(1, targetCount);
    const double decades = std::log10(hi / lo);
    if (decades < 1.0)
        return linearTicks(lo, hi, targetCount);

    const double eps = 1e-9;
    const int d0 = static_cast<int>(std::floor(std::log10(lo) + eps));
    const int d1 = static_cast<int>(std::ceil(std::log10(hi) - eps));
    const int stride = std::max(1, static_cast<int>(std::ceil(decades / targetCount)));
    const auto inRange = [&](double v) { return v >= lo * (1 - eps) && v <= hi * (1 + eps); };

    std::vector<double> decadesInRange;
    for (int d = d0; d <= d1; ++d) {
        const double v = std::pow(10.0, d);
        if (inRange(v))
            decadesInRange.push_back(v);
    }
    const bool addTwoFive = stride == 1 && 3 * decades <= targetCount;
    for (int d = d0; d <= d1; ++d) {
        const double decade = std::pow(10.0, d);
        if (((d % stride) + stride) % stride == 0 && inRange(decade))
            ticks.major.push_back(decade);
        for (int m = 2; m <= 9; ++m) {
            const double v = m * decade;
            if (!inRange(v))
                continue;
            if (addTwoFive && (m == 2 || m == 5))
                ticks.major.push_back(v);
            else if (stride == 1)
                ticks.minor.push_back(v);
        }
        if (stride > 1 && ((d % stride) + stride) % stride != 0 && inRange(decade))
            ticks.minor.push_back(decade);
    }
    std::sort(ticks.major.begin(), ticks.major.end());
    return ticks;
}

bool unitTakesPrefix(const QString &unit)
{
    static const QStringList units = {QStringLiteral("Hz"), QStringLiteral("s"), QStringLiteral("V"),
                                      QStringLiteral("A"), QStringLiteral("W"), QStringLiteral("F"),
                                      QStringLiteral("H"), QStringLiteral("ohm"), QString(QChar(0x03A9)),
                                      QString(QChar(0x2126)), QStringLiteral("S"), QStringLiteral("m")};
    return units.contains(unit);
}

QString displayUnit(const QString &unit)
{
    if (unit == QLatin1String("ohm") || unit == QString(QChar(0x2126)))
        return QString(QChar(0x03A9));
    if (unit == QLatin1String("deg"))
        return QString(QChar(0x00B0));
    return unit;
}

QString formatTick(double value, double step, const QString &unit)
{
    if (!std::isfinite(value))
        return QString(QChar(0x2013));
    if (step > 0 && std::abs(value) < step * 1e-6)
        value = 0;
    if (unitTakesPrefix(unit)) {
        const int exponent = engineeringExponent(value);
        const double scale = std::pow(10.0, exponent);
        const double scaled = value / scale;
        const QString number = step > 0 ? QString::number(scaled, 'f', decimalsForStep(step / scale))
                                        : QString::number(scaled, 'f', decimalsForDigits(scaled, 3));
        return withUnit(trimZeros(number), prefixFor(exponent), unit);
    }
    if (std::abs(value) >= 1e6 || (step > 0 && step < 1e-5))
        return QString::number(value, 'g', 4);
    const QString number = step > 0 ? QString::number(value, 'f', decimalsForStep(step))
                                    : QString::number(value, 'f', decimalsForDigits(value, 3));
    return trimZeros(number);
}

QString formatValue(double value, const QString &unit, int digits)
{
    if (!std::isfinite(value))
        return QString(QChar(0x2013));
    digits = std::clamp(digits, 1, 12);
    if (unitTakesPrefix(unit)) {
        const int exponent = engineeringExponent(value);
        const double scale = std::pow(10.0, exponent);
        const double scaled = value / scale;
        return withUnit(trimZeros(QString::number(scaled, 'f', decimalsForDigits(scaled, digits))),
                        prefixFor(exponent), unit);
    }
    QString number;
    if (value != 0 && (std::abs(value) >= 1e7 || std::abs(value) < 1e-4))
        number = QString::number(value, 'g', digits);
    else
        number = trimZeros(QString::number(value, 'f', std::min(decimalsForDigits(value, digits), 6)));
    return withUnit(number, {}, unit);
}

double AxisScale::toLinear(double value) const
{
    if (!m_log)
        return value;
    return value > 0 ? std::log10(value) : kNaN;
}

double AxisScale::fromLinear(double t) const
{
    return m_log ? std::pow(10.0, t) : t;
}

double AxisScale::toPixel(double value) const
{
    const double t0 = toLinear(m_min);
    const double t1 = toLinear(m_max);
    const double t = toLinear(value);
    if (!(t1 != t0))
        return (m_p0 + m_p1) / 2;
    return m_p0 + (t - t0) / (t1 - t0) * (m_p1 - m_p0);
}

double AxisScale::fromPixel(double pixel) const
{
    const double t0 = toLinear(m_min);
    const double t1 = toLinear(m_max);
    if (m_p1 == m_p0)
        return m_min;
    return fromLinear(t0 + (pixel - m_p0) / (m_p1 - m_p0) * (t1 - t0));
}

void AxisScale::zoom(double factor, double value)
{
    const double t = toLinear(value);
    if (!std::isfinite(t) || !(factor > 0))
        return;
    const double t0 = t + (toLinear(m_min) - t) * factor;
    const double t1 = t + (toLinear(m_max) - t) * factor;
    if (!(t1 > t0) || (t1 - t0) < 1e-12 * std::max(1.0, std::abs(t)))
        return;
    m_min = fromLinear(t0);
    m_max = fromLinear(t1);
}

void AxisScale::pan(double pixels)
{
    if (m_p1 == m_p0)
        return;
    const double t0 = toLinear(m_min);
    const double t1 = toLinear(m_max);
    const double dt = pixels / (m_p1 - m_p0) * (t1 - t0);
    m_min = fromLinear(t0 - dt);
    m_max = fromLinear(t1 - dt);
}

} // namespace lvd

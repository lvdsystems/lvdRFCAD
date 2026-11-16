#include "spice/WaveformMath.h"

#include <algorithm>
#include <cmath>

namespace lvd::spice {

namespace {

template <typename T>
std::optional<T> fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

bool checkTrace(const Waveform &w, QString *error)
{
    QString message;
    if (w.x.size() != w.y.size())
        message = QStringLiteral("the trace has %1 values but a scale of %2 points").arg(w.y.size()).arg(w.x.size());
    else if (w.x.size() < 2)
        message = QStringLiteral("the trace needs at least two points");
    else if (!std::is_sorted(w.x.begin(), w.x.end()))
        message = QStringLiteral("the scale of the trace is not monotonic");
    if (message.isEmpty())
        return true;
    if (error)
        *error = message;
    return false;
}

// Value at xq, which must lie within the scale. At a step (two samples at the
// same x) rightLimit picks the value after the step, otherwise the one before.
Complex interpolate(const Waveform &w, double xq, bool rightLimit)
{
    const auto &x = w.x;
    const std::size_t n = x.size();
    if (rightLimit) {
        const std::size_t i = std::size_t(std::upper_bound(x.begin(), x.end(), xq) - x.begin());
        if (i == 0)
            return w.y.front();
        if (i == n || x[i - 1] == xq)
            return w.y[i - 1];
        const double t = (xq - x[i - 1]) / (x[i] - x[i - 1]);
        return w.y[i - 1] + t * (w.y[i] - w.y[i - 1]);
    }
    const std::size_t i = std::size_t(std::lower_bound(x.begin(), x.end(), xq) - x.begin());
    if (i == n)
        return w.y.back();
    if (x[i] == xq || i == 0)
        return w.y[i];
    const double t = (xq - x[i - 1]) / (x[i] - x[i - 1]);
    return w.y[i - 1] + t * (w.y[i] - w.y[i - 1]);
}

// Real samples of the window, with the interpolated edges.
struct Points {
    std::vector<double> x;
    std::vector<double> y;
};

std::optional<Points> realSlice(const Waveform &w, XWindow window, QString *error, bool needWidth = true)
{
    auto part = slice(w, window, error);
    if (!part)
        return std::nullopt;
    if (needWidth && part->x.back() <= part->x.front())
        return fail<Points>(error, QStringLiteral("the window has zero width"));
    return Points{std::move(part->x), part->real()};
}

std::optional<Extreme> extreme(const Waveform &w, XWindow window, bool largest, QString *error)
{
    const auto p = realSlice(w, window, error, false);
    if (!p)
        return std::nullopt;
    Extreme best{p->x[0], p->y[0]};
    for (std::size_t i = 1; i < p->y.size(); ++i) {
        if (largest ? p->y[i] > best.value : p->y[i] < best.value)
            best = {p->x[i], p->y[i]};
    }
    return best;
}

std::vector<double> crossingsOf(const Points &p, double level, Edge edge)
{
    std::vector<double> result;
    int lastSign = 0;
    double lastX = 0;
    double lastY = 0;
    std::optional<double> touch; // where the trace first reached the level after leaving it
    for (std::size_t i = 0; i < p.x.size(); ++i) {
        const double d = p.y[i] - level;
        const int sign = d > 0 ? 1 : (d < 0 ? -1 : 0);
        if (sign == 0) {
            if (!touch && lastSign != 0)
                touch = p.x[i];
            continue;
        }
        if (lastSign != 0 && sign != lastSign) {
            const bool rising = sign > 0;
            if (edge == Edge::Either || rising == (edge == Edge::Rising)) {
                const double xc = touch ? *touch : lastX + (level - lastY) * (p.x[i] - lastX) / (p.y[i] - lastY);
                result.push_back(xc);
            }
        }
        lastSign = sign;
        lastX = p.x[i];
        lastY = p.y[i];
        touch.reset();
    }
    return result;
}

struct Swing {
    double min;
    double max;
};

std::optional<Swing> swing(const Points &p, QString *error)
{
    const auto [lo, hi] = std::minmax_element(p.y.begin(), p.y.end());
    if (!(*hi > *lo))
        return fail<Swing>(error, QStringLiteral("the trace is flat in the window"));
    return Swing{*lo, *hi};
}

std::optional<double> transition(const Waveform &w, XWindow window, double low, double high, bool rising,
                                 QString *error)
{
    if (!(low > 0 && low < high && high < 1))
        return fail<double>(error, QStringLiteral("the levels must satisfy 0 < low < high < 1"));
    const auto p = realSlice(w, window, error);
    if (!p)
        return std::nullopt;
    const auto s = swing(*p, error);
    if (!s)
        return std::nullopt;
    const double lowLevel = s->min + low * (s->max - s->min);
    const double highLevel = s->min + high * (s->max - s->min);
    const Edge edge = rising ? Edge::Rising : Edge::Falling;
    // A rising edge starts at the low level and ends at the high one; a falling edge the other way round.
    const auto starts = crossingsOf(*p, rising ? lowLevel : highLevel, edge);
    const auto ends = crossingsOf(*p, rising ? highLevel : lowLevel, edge);
    const QString name = rising ? QStringLiteral("rising") : QStringLiteral("falling");
    if (ends.empty())
        return fail<double>(error, QStringLiteral("no %1 edge in the window").arg(name));
    const double end = ends.front();
    std::optional<double> start;
    for (const double x : starts) {
        if (x > end)
            break;
        start = x;
    }
    if (!start)
        return fail<double>(error, QStringLiteral("the %1 edge starts before the window").arg(name));
    return end - *start;
}

struct Periods {
    std::vector<double> rising;
    std::vector<double> falling;
};

std::optional<Periods> periods(const Waveform &w, XWindow window, QString *error)
{
    const auto p = realSlice(w, window, error);
    if (!p)
        return std::nullopt;
    const auto s = swing(*p, error);
    if (!s)
        return std::nullopt;
    const double mid = 0.5 * (s->min + s->max);
    Periods result{crossingsOf(*p, mid, Edge::Rising), crossingsOf(*p, mid, Edge::Falling)};
    if (result.rising.size() < 2)
        return fail<Periods>(error, QStringLiteral("less than one complete period in the window"));
    return result;
}

struct Step {
    Points points;
    double initial;
    double finalValue;
};

std::optional<Step> step(const Waveform &w, XWindow window, QString *error)
{
    auto p = realSlice(w, window, error);
    if (!p)
        return std::nullopt;
    const double initial = p->y.front();
    const double finalValue = p->y.back();
    if (initial == finalValue)
        return fail<Step>(error, QStringLiteral("the initial and final values are equal"));
    return Step{std::move(*p), initial, finalValue};
}

} // namespace

std::vector<double> Waveform::real() const
{
    std::vector<double> result(y.size());
    for (std::size_t i = 0; i < y.size(); ++i)
        result[i] = y[i].real();
    return result;
}

std::vector<double> Waveform::imag() const
{
    std::vector<double> result(y.size());
    for (std::size_t i = 0; i < y.size(); ++i)
        result[i] = y[i].imag();
    return result;
}

Waveform makeWaveform(std::vector<double> x, const std::vector<double> &y, const QString &unit)
{
    Waveform w;
    w.x = std::move(x);
    w.y.assign(y.begin(), y.end());
    w.unit = unit;
    return w;
}

Waveform makeComplexWaveform(std::vector<double> x, const std::vector<double> &re, const std::vector<double> &im,
                             const QString &unit)
{
    Waveform w;
    w.x = std::move(x);
    w.y.resize(std::max(re.size(), im.size()));
    for (std::size_t i = 0; i < w.y.size(); ++i)
        w.y[i] = Complex(i < re.size() ? re[i] : 0.0, i < im.size() ? im[i] : 0.0);
    w.isComplex = true;
    w.unit = unit;
    return w;
}

std::optional<Waveform> slice(const Waveform &w, XWindow window, QString *error)
{
    if (!checkTrace(w, error))
        return std::nullopt;
    if (std::isnan(window.from) || std::isnan(window.to) || window.from > window.to)
        return fail<Waveform>(error, QStringLiteral("the window is empty"));
    const double lo = std::max(window.from, w.x.front());
    const double hi = std::min(window.to, w.x.back());
    if (lo > hi)
        return fail<Waveform>(error, QStringLiteral("the window is outside the trace"));

    Waveform result;
    result.isComplex = w.isComplex;
    result.unit = w.unit;
    result.x.push_back(lo);
    result.y.push_back(interpolate(w, lo, true));
    const auto first = std::upper_bound(w.x.begin(), w.x.end(), lo) - w.x.begin();
    for (auto i = std::size_t(first); i < w.x.size() && w.x[i] < hi; ++i) {
        result.x.push_back(w.x[i]);
        result.y.push_back(w.y[i]);
    }
    if (hi > lo) {
        result.x.push_back(hi);
        result.y.push_back(interpolate(w, hi, false));
    }
    return result;
}

std::optional<double> valueAt(const Waveform &w, double x, QString *error)
{
    if (!checkTrace(w, error))
        return std::nullopt;
    if (!(x >= w.x.front() && x <= w.x.back()))
        return fail<double>(error, QStringLiteral("x is outside the trace"));
    return interpolate(w, x, false).real();
}

std::optional<double> integral(const Waveform &w, XWindow window, QString *error)
{
    const auto p = realSlice(w, window, error, false);
    if (!p)
        return std::nullopt;
    double sum = 0;
    for (std::size_t i = 1; i < p->x.size(); ++i)
        sum += 0.5 * (p->x[i] - p->x[i - 1]) * (p->y[i] + p->y[i - 1]);
    return sum;
}

std::optional<double> mean(const Waveform &w, XWindow window, QString *error)
{
    const auto p = realSlice(w, window, error);
    if (!p)
        return std::nullopt;
    double sum = 0;
    for (std::size_t i = 1; i < p->x.size(); ++i)
        sum += 0.5 * (p->x[i] - p->x[i - 1]) * (p->y[i] + p->y[i - 1]);
    return sum / (p->x.back() - p->x.front());
}

std::optional<double> rms(const Waveform &w, XWindow window, QString *error)
{
    const auto p = realSlice(w, window, error);
    if (!p)
        return std::nullopt;
    double sum = 0;
    for (std::size_t i = 1; i < p->x.size(); ++i) {
        const double a = p->y[i - 1];
        const double b = p->y[i];
        sum += (p->x[i] - p->x[i - 1]) * (a * a + a * b + b * b) / 3.0;
    }
    return std::sqrt(sum / (p->x.back() - p->x.front()));
}

std::optional<Extreme> maximum(const Waveform &w, XWindow window, QString *error)
{
    return extreme(w, window, true, error);
}

std::optional<Extreme> minimum(const Waveform &w, XWindow window, QString *error)
{
    return extreme(w, window, false, error);
}

std::optional<double> peakToPeak(const Waveform &w, XWindow window, QString *error)
{
    const auto hi = maximum(w, window, error);
    const auto lo = hi ? minimum(w, window, error) : std::nullopt;
    if (!lo)
        return std::nullopt;
    return hi->value - lo->value;
}

std::vector<double> crossings(const Waveform &w, double level, Edge edge, XWindow window, QString *error)
{
    const auto p = realSlice(w, window, error, false);
    if (!p)
        return {};
    return crossingsOf(*p, level, edge);
}

std::optional<double> crossing(const Waveform &w, double level, Edge edge, int occurrence, XWindow window,
                               QString *error)
{
    if (occurrence < 1)
        return fail<double>(error, QStringLiteral("the occurrence must be 1 or more"));
    const auto p = realSlice(w, window, error, false);
    if (!p)
        return std::nullopt;
    const auto all = crossingsOf(*p, level, edge);
    if (all.size() < std::size_t(occurrence)) {
        return fail<double>(error, QStringLiteral("only %1 crossing(s) of %2 in the window")
                                       .arg(all.size())
                                       .arg(level));
    }
    return all[std::size_t(occurrence - 1)];
}

std::optional<double> riseTime(const Waveform &w, XWindow window, double low, double high, QString *error)
{
    return transition(w, window, low, high, true, error);
}

std::optional<double> fallTime(const Waveform &w, XWindow window, double low, double high, QString *error)
{
    return transition(w, window, low, high, false, error);
}

std::optional<double> period(const Waveform &w, XWindow window, QString *error)
{
    const auto p = periods(w, window, error);
    if (!p)
        return std::nullopt;
    return (p->rising.back() - p->rising.front()) / double(p->rising.size() - 1);
}

std::optional<double> frequency(const Waveform &w, XWindow window, QString *error)
{
    const auto t = period(w, window, error);
    if (!t)
        return std::nullopt;
    return 1.0 / *t;
}

std::optional<double> dutyCycle(const Waveform &w, XWindow window, QString *error)
{
    const auto p = periods(w, window, error);
    if (!p)
        return std::nullopt;
    // Each complete period runs from a rising crossing to the next; the time
    // above the mid level ends at the first falling crossing in between.
    double high = 0;
    std::size_t f = 0;
    for (std::size_t k = 0; k + 1 < p->rising.size(); ++k) {
        const double start = p->rising[k];
        const double end = p->rising[k + 1];
        while (f < p->falling.size() && p->falling[f] <= start)
            ++f;
        if (f < p->falling.size() && p->falling[f] < end)
            high += p->falling[f] - start;
    }
    return high / (p->rising.back() - p->rising.front());
}

std::optional<double> overshoot(const Waveform &w, XWindow window, QString *error)
{
    const auto s = step(w, window, error);
    if (!s)
        return std::nullopt;
    const double size = s->finalValue - s->initial;
    const auto &y = s->points.y;
    const double beyond = size > 0 ? *std::max_element(y.begin(), y.end()) - s->finalValue
                                   : s->finalValue - *std::min_element(y.begin(), y.end());
    return std::max(0.0, beyond / std::abs(size) * 100.0);
}

std::optional<double> settlingTime(const Waveform &w, double tolerancePercent, XWindow window, QString *error)
{
    if (!(tolerancePercent > 0))
        return fail<double>(error, QStringLiteral("the tolerance must be positive"));
    const auto s = step(w, window, error);
    if (!s)
        return std::nullopt;
    const auto &x = s->points.x;
    const auto &y = s->points.y;
    const double band = std::abs(s->finalValue - s->initial) * tolerancePercent / 100.0;
    // Walk back from the end to the last sample outside the band; the trace
    // enters the band for good on the segment after it.
    std::size_t i = y.size();
    while (i > 0 && std::abs(y[i - 1] - s->finalValue) <= band)
        --i;
    if (i == 0)
        return 0.0;
    const std::size_t out = i - 1;
    if (out + 1 >= y.size())
        return fail<double>(error, QStringLiteral("the trace does not settle in the window"));
    const double edge = y[out] > s->finalValue ? s->finalValue + band : s->finalValue - band;
    const double x0 = x[out];
    const double x1 = x[out + 1];
    const double t = (edge - y[out]) / (y[out + 1] - y[out]);
    return x0 + t * (x1 - x0) - x.front();
}

} // namespace lvd::spice

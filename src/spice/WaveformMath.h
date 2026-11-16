#pragma once

#include <QString>

#include <complex>
#include <limits>
#include <optional>
#include <vector>

namespace lvd::spice {

using Complex = std::complex<double>;

// One trace: values over a shared scale (time, frequency, swept voltage...).
struct Waveform {
    std::vector<double> x;  // scale, non-decreasing; transient steps are non-uniform
    std::vector<Complex> y; // values; imaginary parts are 0 for real data
    bool isComplex = false; // AC/noise results are complex
    QString unit;           // "V", "A", "dB", "deg", "s", "" ...

    std::size_t size() const { return y.size(); }
    std::vector<double> real() const;
    std::vector<double> imag() const;
};

// Builds a trace from separate vectors. The scale and the values must have the
// same length; this is not checked here (the measurements report it).
Waveform makeWaveform(std::vector<double> x, const std::vector<double> &y, const QString &unit = QString());
Waveform makeComplexWaveform(std::vector<double> x, const std::vector<double> &re, const std::vector<double> &im,
                             const QString &unit = QString());

// A range of the scale. The default is the whole trace; a window wider than
// the trace is clipped to it.
struct XWindow {
    double from = -std::numeric_limits<double>::infinity();
    double to = std::numeric_limits<double>::infinity();
};

// All measurements below work on the real part of the trace, treat it as
// piecewise linear between samples (the scale may be non-uniform, and two
// samples at the same x are a step), and return nullopt with *error set
// (when error is not null) if the result does not exist: fewer than two
// samples, scale and values of different lengths, a decreasing scale, a
// window outside the trace, no crossing, a flat trace...

// The part of the trace inside the window, with interpolated samples added at
// the window edges. Complex values are kept.
std::optional<Waveform> slice(const Waveform &w, XWindow window = {}, QString *error = nullptr);

// Real part at x, linearly interpolated; nullopt outside the trace.
std::optional<double> valueAt(const Waveform &w, double x, QString *error = nullptr);

// Averages are weighted by the scale (integral / width), not by sample count,
// so dense time steps around an edge do not bias them. The integrals are exact
// for the piecewise-linear trace (trapezoids for the mean; for the RMS each
// segment contributes (a^2 + ab + b^2)/3 times its width).
std::optional<double> mean(const Waveform &w, XWindow window = {}, QString *error = nullptr);
std::optional<double> rms(const Waveform &w, XWindow window = {}, QString *error = nullptr);
std::optional<double> integral(const Waveform &w, XWindow window = {}, QString *error = nullptr);

struct Extreme {
    double x = 0;
    double value = 0;
};
// The first sample with the largest (smallest) value; the window edges count
// as samples.
std::optional<Extreme> maximum(const Waveform &w, XWindow window = {}, QString *error = nullptr);
std::optional<Extreme> minimum(const Waveform &w, XWindow window = {}, QString *error = nullptr);
std::optional<double> peakToPeak(const Waveform &w, XWindow window = {}, QString *error = nullptr);

enum class Edge { Rising, Falling, Either };

// x of every crossing of level inside the window, interpolated. A trace that
// only touches the level and turns back does not cross it; a trace that stays
// on the level for a while crosses where it first reached it.
std::vector<double> crossings(const Waveform &w, double level, Edge edge = Edge::Either, XWindow window = {},
                              QString *error = nullptr);
// x of the n-th (1-based) crossing of level after window.from.
std::optional<double> crossing(const Waveform &w, double level, Edge edge = Edge::Either, int occurrence = 1,
                               XWindow window = {}, QString *error = nullptr);

// Rise (fall) time between the low and high fractions of the swing, by default
// 10-90 %. The levels are relative to the minimum and maximum inside the
// window (not to the initial and final values), so overshoot widens the swing;
// narrow the window to the edge to measure it differently. The time runs from
// the last low (high) crossing before the first high (low) crossing, which
// ignores ringing at the start. Requires 0 < low < high < 1.
std::optional<double> riseTime(const Waveform &w, XWindow window = {}, double low = 0.1, double high = 0.9,
                               QString *error = nullptr);
std::optional<double> fallTime(const Waveform &w, XWindow window = {}, double low = 0.1, double high = 0.9,
                               QString *error = nullptr);

// Average over the complete periods inside the window, from the rising
// crossings of the mid level (max + min) / 2; needs at least two crossings.
std::optional<double> period(const Waveform &w, XWindow window = {}, QString *error = nullptr);
std::optional<double> frequency(const Waveform &w, XWindow window = {}, QString *error = nullptr);
// Fraction (0..1) of the complete periods spent above the mid level.
std::optional<double> dutyCycle(const Waveform &w, XWindow window = {}, QString *error = nullptr);

// Step response measures. The initial value is the value at the window start
// and the final value the value at the window end, so the window must end
// after the trace has settled.
// Overshoot in percent of the step size beyond the final value (0 if the
// trace never goes past it).
std::optional<double> overshoot(const Waveform &w, XWindow window = {}, QString *error = nullptr);
// Time from the window start until the trace last enters, and then stays
// within, final value +- tolerancePercent of the step size.
std::optional<double> settlingTime(const Waveform &w, double tolerancePercent = 2, XWindow window = {},
                                   QString *error = nullptr);

} // namespace lvd::spice

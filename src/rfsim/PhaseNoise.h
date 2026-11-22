#pragma once

#include "core/Netlist.h"

#include <QByteArray>
#include <QString>

#include <cmath>
#include <functional>
#include <optional>
#include <vector>

namespace lvd::rf {

// Oscillator phase noise for the system budget (docs/libraries.md, blocks
// `lo`, `vco` and `pll`).
//
// A profile is single-sideband phase noise L(f) in dBc/Hz at offsets f in Hz,
// interpolated linearly in dB over log10(f) between the points and held
// constant beyond the first and the last point. Every segment is therefore a
// power law, which the integration below follows exactly.

struct PhaseNoiseProfile {
    std::vector<double> offsets; // Hz, increasing, > 0
    std::vector<double> levels;  // dBc/Hz

    bool empty() const { return offsets.empty(); }
    // L(f) in dBc/Hz; -inf for an empty profile.
    double level(double offset) const;
    // The same profile raised by `db` (a multiplier by N: 20 log10 N).
    PhaseNoiseProfile shifted(double db) const;
};

// Text file of two columns, offset in Hz and L(f) in dBc/Hz. `#` or `!`
// starts a comment; values are separated by spaces, tabs, commas or
// semicolons. Points are sorted by offset; offsets must be positive and
// distinct.
std::optional<PhaseNoiseProfile> parsePhaseNoise(const QByteArray &text, QString *error = nullptr);
std::optional<PhaseNoiseProfile> readPhaseNoise(const QString &path, QString *error = nullptr);

// Integral of L(f) (linear, 1/Hz) from f1 to f2: single-sideband phase-noise
// power relative to the carrier, in dBc. A flat L gives L + 10 log10(f2 - f1).
double integratePhaseNoiseSsb(const PhaseNoiseProfile &profile, double f1, double f2);

// Integrated phase noise over both sidebands, 10 log10(2 ∫ L df) in dBc, and
// the RMS phase error sqrt(2 ∫ L df) (rad) and jitter for a carrier frequency.
struct IntegratedPhaseNoise {
    double dbc = -INFINITY;          // double-sideband
    double rmsPhaseDegrees = 0;
    double rmsJitterSeconds = 0;
};
IntegratedPhaseNoise integratePhaseNoise(const PhaseNoiseProfile &profile, double f1, double f2, double carrier);

// Free-running oscillator: L = pn at pnOffset, falling 20 dB per decade,
// added in power to a flat floor:
//   L(f) = 10 log10(10^((pn - 20 log10(f / pnOffset)) / 10) + 10^(floor / 10)).
double vcoLevel(double offset, double pn, double pnOffset, double floor);

// Phase-locked loop with a first-order loop shape of bandwidth fLoop:
//   N = fOut / fRef
//   L_in  = max(refPn + 20 log10 N, fom + 10 log10 fRef + 20 log10 N)  (in band, flat)
//   L(f)  = 10 log10(L_in / (1 + (f/fLoop)^2) + L_vco(f) (f/fLoop)^2 / (1 + (f/fLoop)^2))
// with L_vco from vcoLevel(). Levels in dBc/Hz, linear inside the sum. A
// textbook approximation, not a loop simulation.
struct PllSettings {
    double frequency = 0;     // output, Hz
    double reference = 0;     // reference (= phase detector) frequency, Hz
    double referencePn = -150; // dBc/Hz, flat
    double fom = -226;         // normalised in-band floor, dBc/Hz at 1 Hz
    double loopBandwidth = 100e3;
    double vcoPn = -110;       // dBc/Hz at vcoPnOffset
    double vcoPnOffset = 100e3;
    double vcoFloor = -160;
};
double pllLevel(double offset, const PllSettings &pll);

// Tabulated profiles of the models above, 20 points per decade from 0.1 Hz
// to 10 GHz, for the same interpolation and integration as a file.
PhaseNoiseProfile tabulate(const std::function<double(double)> &level);

// The phase-noise profile of an `lo` (its `phase_noise` file, relative
// paths against projectDirectory), `vco` or `pll` component. nullopt
// without phase noise (an `lo` without a file, or another block); *error is
// set when a file cannot be read.
std::optional<PhaseNoiseProfile> oscillatorPhaseNoise(const FlatComponent &component, const QString &projectDirectory,
                                                      QString *error = nullptr);

} // namespace lvd::rf

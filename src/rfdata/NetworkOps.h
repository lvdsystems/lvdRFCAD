#pragma once

#include "rfdata/RfTypes.h"

namespace lvd::rf {

// Operations on multi-frequency networks (requirement RF-4) and two-port
// figures of merit. Functions that can fail return nullopt with *error set.

enum class Extrapolation { Error, Hold };

// Resamples onto new frequencies, interpolating magnitude and unwrapped phase
// of every S-parameter linearly in frequency. Outside the data range:
// Error fails, Hold keeps the end values and reports it in *warning.
// Noise parameters are interpolated the same way (Fmin and Rn linearly,
// gammaOpt in magnitude and phase); noise correlation matrices element-wise.
// Noise parameters have their own frequency grid: with Error, target
// frequencies outside it get no noise parameters (noted in *warning).
// An interval whose phase step exceeds 90 degrees (a curve passing near the
// origin) is interpolated in real and imaginary parts instead.
std::optional<NetworkData> interpolate(const NetworkData &network, const std::vector<double> &frequencies,
                                       Extrapolation extrapolation, QString *error = nullptr,
                                       QString *warning = nullptr);

NetworkData renormalized(const NetworkData &network, const std::vector<double> &z0);

// order[i] is the old port index that becomes new port i (a permutation).
// Swapping the ports of a two-port recomputes its noise parameters.
std::optional<NetworkData> renumberPorts(const NetworkData &network, const std::vector<int> &order,
                                         QString *error = nullptr);

// Two-port cascade a then b. b is interpolated onto a's frequencies; the
// result uses port 1 of a and port 2 of b as references, and the connection
// is made with the actual reference impedances (renormalising as needed).
// Noise: when at least one side has noise data (correlation matrices, or
// noise parameters covering a's frequencies) the result has noise
// correlation matrices; a passive side without noise data counts as thermal
// noise at T0. Noise parameters are added when an input had noise parameters.
std::optional<NetworkData> cascade(const NetworkData &a, const NetworkData &b, QString *error = nullptr);
// Removes known two-port fixtures: result = left^-1 * measured * right^-1.
// Either fixture may be null (std::nullopt) to de-embed one side only. The
// result refers to the inner references of the fixtures and has no noise data.
std::optional<NetworkData> deembed(const NetworkData &measured, const std::optional<NetworkData> &left,
                                   const std::optional<NetworkData> &right, QString *error = nullptr);

// Per-frequency checks. Passive: the largest singular value of S <= 1 + tolerance.
bool isPassive(const CMatrix &s, double tolerance = 1e-9);
bool isReciprocal(const CMatrix &s, double tolerance = 1e-9);
// Indices of frequencies where the check fails.
std::vector<int> nonPassiveFrequencies(const NetworkData &network, double tolerance = 1e-9);
std::vector<int> nonReciprocalFrequencies(const NetworkData &network, double tolerance = 1e-9);

// Two-port stability (Rollett K with |Delta|, and Edwards-Sinsky mu for source and load).
double stabilityK(const CMatrix &s);
Complex stabilityDelta(const CMatrix &s);
double stabilityMu(const CMatrix &s);      // load-side mu
double stabilityMuPrime(const CMatrix &s); // source-side mu'
double maximumAvailableGainDb(const CMatrix &s); // MAG when K > 1, MSG otherwise

// Two-port gains and design circles (G. Gonzalez, "Microwave Transistor
// Amplifiers", 2nd ed., 1997, chapters 2-4; D. M. Pozar, "Microwave
// Engineering", 4th ed., chapter 12). Reflection coefficients refer to the
// two-port's own reference impedance.
// A circle in the reflection-coefficient plane; valid = false when it does
// not exist (a degenerate stability circle that is a straight line, a gain
// above the maximum, a noise figure below NFmin).
struct SmithCircle {
    Complex centre;
    double radius = 0;
    bool valid = false;
};
// Stability circles: the source (load) reflections for which the output
// (input) reflection has magnitude 1.
SmithCircle sourceStabilityCircle(const CMatrix &s);
SmithCircle loadStabilityCircle(const CMatrix &s);
// Available gain GA for a source reflection gammaS, and operating power gain
// GP for a load reflection gammaL (linear, not dB).
double availableGain(const CMatrix &s, Complex gammaS);
double operatingPowerGain(const CMatrix &s, Complex gammaL);
// Constant-GA circle in the source plane and constant-GP circle in the load
// plane, gain in dB.
SmithCircle availableGainCircle(const CMatrix &s, double gainDb);
SmithCircle operatingPowerGainCircle(const CMatrix &s, double gainDb);
// Constant noise-figure circle in the source plane, from the two-port's
// noise parameters (gammaOpt and Rn referred to z0), noise figure in dB.
SmithCircle noiseFigureCircle(const NoiseParameters &noise, double nfDb, double z0);
// Maximum unilateral transducer gain |S21|^2 / ((1 - |S11|^2)(1 - |S22|^2))
// in dB (S12 set to zero); +inf when |S11| or |S22| >= 1.
double maximumUnilateralGainDb(const CMatrix &s);
// Mason's unilateral power gain U in dB, invariant under lossless reciprocal
// embedding; +inf for a unilateral two-port with gain.
double masonUnilateralGainDb(const CMatrix &s);
// Simultaneous conjugate match (source, load) reflections giving MAG; only
// for an unconditionally stable two-port (K > 1, |Delta| < 1).
struct ConjugateMatch {
    Complex gammaS, gammaL;
};
std::optional<ConjugateMatch> simultaneousConjugateMatch(const CMatrix &s);

// Group delay of S(row, col) in seconds, -d(phase)/d(omega) by central
// differences of the unwrapped phase (one-sided at the ends).
std::vector<double> groupDelay(const NetworkData &network, int row, int col);
std::vector<double> unwrappedPhase(const NetworkData &network, int row, int col); // radians
double vswr(Complex reflection);

// Two-port noise: noise wave correlation (kelvin, as NetworkData::noiseCorrelation)
// to and from noise parameters, for reference impedance z0 on both ports.
NoiseParameters noiseParametersFromCorrelation(const CMatrix &s, const CMatrix &correlation, double z0);
CMatrix correlationFromNoiseParameters(const CMatrix &s, const NoiseParameters &noise, double z0);
// Noise figure (dB) of a two-port for a source reflection coefficient gammaS
// referred to z0, from its noise parameters.
double noiseFigureDb(const NoiseParameters &noise, Complex gammaS, double z0);

} // namespace lvd::rf

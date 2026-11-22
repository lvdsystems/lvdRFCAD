#pragma once

#include "rfdata/RfTypes.h"

#include <vector>

namespace lvd::rf {

// Small-signal maths of the behavioural blocks (docs/libraries.md, "Block and
// element types"), shared by the linear models and the system budget. All
// matrices are at the block's own reference impedance, in pin order.

// Return loss in dB to |Gamma|. 0 (the default of the `rl_*` parameters)
// means an ideal match, |Gamma| = 0.
double returnLossToGamma(double returnLossDb);

// value + slope · (f - reference) / 1 GHz. With `nonNegative` (losses, noise
// figures) the result is not below 0. A zero slope returns `value` unchanged.
double sloped(double value, double slopePerGHz, double frequency, double reference, bool nonNegative);

// Largest singular value; a network is passive when it is <= 1.
double largestSingularValue(const CMatrix &s);

// A passive network: when the largest singular value of s exceeds 1, the
// off-diagonal part (the transmissions) is scaled by the largest factor k in
// [0, 1] that makes it 1; the reflections are kept. Unchanged otherwise.
CMatrix limitToPassive(const CMatrix &s);

// Adds port reflections to a block with a zero diagonal: S_ii = j |Gamma_i| ·
// phase, i.e. +90 degrees from `phase` (normally the unit phasor of the
// block's through transmission), the phase of a lossless reciprocal two-port
// with S11 = S22. For passive blocks the result goes through
// limitToPassive(). With every |Gamma_i| = 0, s is returned unchanged.
CMatrix withPortReflections(const CMatrix &s, const std::vector<double> &gamma, Complex phase = 1.0, bool passive = true);

// Switch with `throws` throws (pins com, t1 .. tN): the selected throw
// (1-based) has loss il, every other path isolation; absorptive. With more
// than two throws the matrix goes through limitToPassive(). The hybrids and
// the balun below do too: a poor isolation with a low loss is not passive,
// and then costs transmission.
CMatrix multiThrowSwitch(int throws, int selected, double ilDb, double isolationDb);

// 90 degree hybrid, pins in, out0, out90, iso: in -> out0 is -(3.01 + il +
// imbalance/2) dB at -phase_imbalance/2 degrees, in -> out90 -(3.01 + il -
// imbalance/2) dB at -90 + phase_imbalance/2 degrees; out0 -> iso as
// in -> out90, out90 -> iso as in -> out0 (before the phase imbalance); in-iso
// and out0-out90 isolation. The phase imbalance is a phase shift of -/+
// phase_imbalance/2 at the two output ports (out0, out90; p1, p2; bal_p,
// bal_n), which keeps the block passive.
CMatrix hybrid90(double ilDb, double isolationDb, double imbalanceDb, double phaseImbalanceDegrees);

// 180 degree hybrid (rat-race), pins sum, diff, p1, p2: sum -> p1 and p2 in
// phase, diff -> p1 in phase and -> p2 at 180 degrees; sum-diff and p1-p2
// isolation. Amplitudes as hybrid90 (p1 paths +imbalance/2).
CMatrix hybrid180(double ilDb, double isolationDb, double imbalanceDb, double phaseImbalanceDegrees);

// Balun, pins unbal, bal_p, bal_n: unbal -> bal_p -(3.01 + il + imbalance/2)
// dB at -phase_imbalance/2, unbal -> bal_n -(3.01 + il - imbalance/2) dB at
// 180 + phase_imbalance/2 degrees; bal_p-bal_n isolation; matched.
CMatrix balun(double ilDb, double isolationDb, double imbalanceDb, double phaseImbalanceDegrees);

// Diplexer, pins com, low, high: complementary Butterworth low-pass H_L and
// high-pass H_H of the given order at the crossover frequency
// (|H_L|^2 + |H_H|^2 = 1). The lossless reciprocal 3-port matched at com:
//   S = [[0, H_L, H_H], [H_L, conj(H_H)^2, -conj(H_H H_L)], [H_H, -conj(H_H H_L), conj(H_L)^2]],
// times undb20(-il).
CMatrix diplexer(double frequency, double crossover, int order, double ilDb);

// DC block: a series capacitor of corner frequency fc (|S21|^2 = 1/(1 + (fc/f)^2))
// with the extra loss il on the transmission.
CMatrix dcBlock(double frequency, double fc, double ilDb);

// Digital-to-analogue converter at the start of a transmit chain.
struct DacFigures {
    double sincDb = 0;           // 20 log10 |sinc(f / fs)|, NRZ hold (<= 0)
    double signalDbm = 0;        // full_scale - backoff + sincDb
    double nsdDbmHz = 0;         // noise spectral density at the output
    bool nsdFromQuantisation = false;
    double imageFrequency = 0;   // first image, |fs - f|
    double imageDbm = 0;         // full_scale - backoff + 20 log10 |sinc(image / fs)|
    double noiseTemperature = 0; // K: kT = NSD
};
// nsdDbmHz = 0 (the default of `nsd`): from quantisation,
// full_scale - 6.02 bits - 1.76 - 10 log10(fs / 2).
DacFigures dacFigures(double frequency, double fs, double bits, double fullScaleDbm, double backoffDb, double nsdDbmHz);
// 20 log10 |sin(pi x) / (pi x)| with x = f / fs; 0 at f = 0, -inf at multiples of fs.
double sincDb(double frequency, double fs);

} // namespace lvd::rf

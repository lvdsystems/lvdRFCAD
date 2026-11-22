#pragma once

#include "rfdata/LineModels.h"

namespace lvd::rf {

// Microstrip discontinuities (docs/libraries.md, rf-circuit elements): closed
// formulas for their equivalent circuits. All lengths in metres, frequencies
// in Hz, capacitances in farads, inductances in henries. Formulas and
// references are in Discontinuities.cpp. The line properties (impedance and
// effective permittivity with dispersion) come from microstrip().

// Substrate and metal of the microstrip around a discontinuity.
struct MicrostripSubstrate {
    double h = 0, t = 0, er = 1, tand = 0, sigma = 0, rough = 0;
    MicrostripGeometry line(double w) const { return {w, h, t, er, tand, sigma, rough}; }
};

// Kirschning-Jansen-Koster open-end length extension dl (metres) of a
// microstrip of width w, from its effective permittivity eeff (at the
// frequency of interest).
double openEndLengthExtension(double w, double h, double er, double eeff);
// Open end: shunt capacitance C = dl sqrt(eeff)/(c0 Z0) at the frequency.
std::optional<double> openEndCapacitance(const MicrostripSubstrate &substrate, double w, double frequency,
                                         QString *error = nullptr);

// Series gap s between strips of widths w1 and w2: a pi network of shunt
// c1 (port 1), series cs, shunt c2 (port 2).
struct GapCircuit {
    double c1 = 0, cs = 0, c2 = 0;
};
std::optional<GapCircuit> microstripGap(const MicrostripSubstrate &substrate, double w1, double w2, double s,
                                        double frequency, QString *error = nullptr);

// Step in width from w1 (port 1) to w2 (port 2): series l1, shunt c, series l2.
struct StepCircuit {
    double l1 = 0, c = 0, l2 = 0;
};
std::optional<StepCircuit> microstripStep(const MicrostripSubstrate &substrate, double w1, double w2, double frequency,
                                          QString *error = nullptr);

// 90 degree bend of a strip of width w, unmitred or optimally mitred: a T of
// series l, shunt c, series l (reference planes at the inner edges of the
// adjoining lines, as in Qucs MCORN/MMBEND). Frequency independent.
struct BendCircuit {
    double l = 0, c = 0;
};
std::optional<BendCircuit> microstripBend(const MicrostripSubstrate &substrate, double w, bool mitred,
                                          QString *error = nullptr);

// Hammerstad T-junction: main arms of widths w1 (port 1) and w2 (port 2),
// side arm of width w3 (port 3). The S-matrix (reference z0 on all ports)
// includes the short arm lines from the reference planes to the junction.
std::optional<CMatrix> microstripTee(const MicrostripSubstrate &substrate, double w1, double w2, double w3,
                                     double frequency, double z0, QString *error = nullptr);
// The equivalent circuit behind microstripTee, for tests and inspection.
struct TeeCircuit {
    double la = 0, lb = 0, l3 = 0; // arm line lengths (m), may be negative
    double ta2 = 1, tb2 = 1;       // squared turns ratios of the main-arm transformers
    double bt = 0;                 // shunt susceptance (S)
    LineProperties lineA, lineB, line3;
};
std::optional<TeeCircuit> microstripTeeCircuit(const MicrostripSubstrate &substrate, double w1, double w2, double w3,
                                               double frequency, QString *error = nullptr);

// Via hole to ground (Goldfarb-Pucel inductance, plated-barrel resistance
// with skin effect): impedance of a via of diameter d through height h,
// plating thickness t. sigma = 0 or t = 0 gives a lossless via.
std::optional<Complex> viaImpedance(double d, double h, double t, double sigma, double frequency,
                                    QString *error = nullptr);

// Connects the two-port `line` (port 1 outside, port 2 inside) to port k of
// the N-port s; all referred to the same impedance.
CMatrix connectToPort(const CMatrix &s, int k, const CMatrix &line);

} // namespace lvd::rf

#pragma once

#include "rfdata/RfTypes.h"

namespace lvd::rf {

// Physical transmission line models (docs/libraries.md, rf-circuit elements).
// All lengths in metres, frequencies in Hz.

// Line properties at one frequency: characteristic impedance (complex for a
// lossy line) and propagation constant gamma = alpha + j beta per metre.
struct LineProperties {
    Complex zc;
    Complex gamma;
    double epsilonEffective = 1; // effective relative permittivity (quasi-static plus dispersion)
    double alphaConductor = 0;   // Np/m
    double alphaDielectric = 0;  // Np/m
};

// rough: rms surface roughness of the conductors in metres (0 = smooth). It
// scales the conductor loss by the Hammerstad-Jensen factor (see
// roughnessFactor).
struct MicrostripGeometry {
    double w = 0, h = 0, t = 0, er = 1, tand = 0, sigma = 0, rough = 0;
};
struct StriplineGeometry {
    double w = 0, b = 0, t = 0, er = 1, tand = 0, sigma = 0, rough = 0;
};
struct CoaxGeometry {
    double din = 0, dout = 0, er = 1, tand = 0, sigma = 0;
};
struct CpwGeometry {
    double w = 0, s = 0, h = 0, t = 0, er = 1, tand = 0, sigma = 0;
    bool backed = false;
    double rough = 0;
};
// Two identical parallel microstrips of width w, edge-to-edge gap s.
struct CoupledMicrostripGeometry {
    double w = 0, s = 0, h = 0, t = 0, er = 1, tand = 0, sigma = 0, rough = 0;
};
struct CoupledLineProperties {
    LineProperties even, odd;
};

// Each returns nullopt with *error set for invalid geometry (non-positive
// sizes, er < 1, and so on). Frequency 0 is allowed (DC values, no loss).
// sigma = 0 means a perfect conductor. The CPW conductor loss needs t > 0
// (it is zero for t = 0). Formulas and references are in LineModels.cpp.
// Microstrip: Hammerstad-Jensen with thickness correction, Kirschning-Jansen dispersion.
std::optional<LineProperties> microstrip(const MicrostripGeometry &geometry, double frequency, QString *error = nullptr);
std::optional<LineProperties> stripline(const StriplineGeometry &geometry, double frequency, QString *error = nullptr);
std::optional<LineProperties> coax(const CoaxGeometry &geometry, double frequency, QString *error = nullptr);
std::optional<LineProperties> cpw(const CpwGeometry &geometry, double frequency, QString *error = nullptr);
// Coupled microstrip: Kirschning-Jansen even and odd modes with dispersion.
std::optional<CoupledLineProperties> coupledMicrostrip(const CoupledMicrostripGeometry &geometry, double frequency,
                                                       QString *error = nullptr);

// Hammerstad-Jensen roughness correction of the conductor loss,
// K = 1 + (2/pi) atan(1.4 (rough/delta)^2), delta the skin depth. 1 for a
// smooth (rough = 0) or perfect (sigma = 0) conductor; tends to 2.
double roughnessFactor(double rough, double frequency, double sigma);

// ABCD matrix of a uniform line section of length l.
CMatrix lineAbcd(Complex zc, Complex gamma, double length);

} // namespace lvd::rf

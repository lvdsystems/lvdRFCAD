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

struct MicrostripGeometry {
    double w = 0, h = 0, t = 0, er = 1, tand = 0, sigma = 0;
};
struct StriplineGeometry {
    double w = 0, b = 0, t = 0, er = 1, tand = 0, sigma = 0;
};
struct CoaxGeometry {
    double din = 0, dout = 0, er = 1, tand = 0, sigma = 0;
};
struct CpwGeometry {
    double w = 0, s = 0, h = 0, t = 0, er = 1, tand = 0, sigma = 0;
    bool backed = false;
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

// ABCD matrix of a uniform line section of length l.
CMatrix lineAbcd(Complex zc, Complex gamma, double length);

} // namespace lvd::rf

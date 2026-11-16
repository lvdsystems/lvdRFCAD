#pragma once

#include "rfdata/RfTypes.h"

namespace lvd::rf {

// Conversions between network representations at one frequency. Reference
// impedances are real, per port; waves are power waves (identical to
// pseudo-waves for real references). Functions return nullopt when the
// target representation does not exist (for example Z of an ideal thru).

std::optional<CMatrix> sToZ(const CMatrix &s, const std::vector<double> &z0);
CMatrix zToS(const CMatrix &z, const std::vector<double> &z0);
std::optional<CMatrix> sToY(const CMatrix &s, const std::vector<double> &z0);
CMatrix yToS(const CMatrix &y, const std::vector<double> &z0);

// Two-ports only. ABCD with V1 = A V2 + B I2, I1 = C V2 + D I2 (I2 flowing out of port 2).
std::optional<CMatrix> sToAbcd(const CMatrix &s, double z01, double z02);
CMatrix abcdToS(const CMatrix &abcd, double z01, double z02);
// Scattering transfer matrix, [b1; a1] = T [a2; b2]. Two-ports only.
std::optional<CMatrix> sToT(const CMatrix &s);
CMatrix tToS(const CMatrix &t);

// Changes the reference impedances of S from z0Old to z0New.
CMatrix renormalize(const CMatrix &s, const std::vector<double> &z0Old, const std::vector<double> &z0New);
// The matrix T with c_new = T c_old for noise waves under the same change of
// reference; the correlation transforms as T C T^H.
CMatrix noiseRenormalization(const CMatrix &s, const std::vector<double> &z0Old, const std::vector<double> &z0New);

// Convenience
Complex reflectionFromImpedance(Complex z, double z0);
Complex impedanceFromReflection(Complex gamma, double z0);
double db20(Complex value); // 20 log10 |value|
double db10(double value);
Complex fromDbPhase(double db, double phaseDegrees); // magnitude from dB (20 log), phase in degrees

} // namespace lvd::rf

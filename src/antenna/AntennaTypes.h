#pragma once

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

// Common types of the antenna library (requirements AN-1 to AN-6).
//
// Conventions, used everywhere in src/antenna:
// - Angles in the API are degrees; internal maths uses radians.
// - Spherical coordinates: theta from +z, phi from +x towards +y. A planar
//   array lies in the xy-plane and radiates towards +z (broadside).
// - Time convention exp(+j w t); a wave leaving the origin is exp(-j k r),
//   so an element at p contributes exp(+j k r_hat . p) to the far field.
// - Far fields are normalised so that |E_theta|^2 + |E_phi|^2 is the
//   directivity (linear) of an element, or the gain of an array weighted
//   with sqrt(power).
// - Circular polarisation follows IEEE: RHCP = (E_theta + j E_phi) / sqrt 2
//   in the (theta_hat, phi_hat) basis, LHCP = (E_theta - j E_phi) / sqrt 2.
// - Linear co- and cross-polarisation follow Ludwig's third definition.

namespace lvd::ant {

using cplx = std::complex<double>;
using Vec3 = Eigen::Vector3d;
using Mat3 = Eigen::Matrix3d;

inline constexpr double kSpeedOfLight = 299792458.0;
inline constexpr double kPi = std::numbers::pi;
inline constexpr double kT0 = 290.0; // reference noise temperature, K

inline double toRadians(double degrees) { return degrees * kPi / 180.0; }
inline double toDegrees(double radians) { return radians * 180.0 / kPi; }
inline double wavelength(double frequency) { return kSpeedOfLight / frequency; }
inline double powerDb(double linear) { return linear > 0 ? 10.0 * std::log10(linear) : -400.0; }
inline double fromDb(double db) { return std::pow(10.0, db / 10.0); }
inline double amplitudeFromDb(double db) { return std::pow(10.0, db / 20.0); }

// Far-field components at one direction.
struct Field {
    cplx eTheta;
    cplx ePhi;

    double power() const { return std::norm(eTheta) + std::norm(ePhi); }
    Field &operator+=(const Field &o)
    {
        eTheta += o.eTheta;
        ePhi += o.ePhi;
        return *this;
    }
    Field operator*(cplx s) const { return {eTheta * s, ePhi * s}; }
};

// Unit vectors; theta and phi in radians.
inline Vec3 radialUnit(double theta, double phi)
{
    return {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
}
inline Vec3 thetaUnit(double theta, double phi)
{
    return {std::cos(theta) * std::cos(phi), std::cos(theta) * std::sin(phi), -std::sin(theta)};
}
inline Vec3 phiUnit(double phi) { return {-std::sin(phi), std::cos(phi), 0.0}; }

// Direction angles (radians) of a unit vector; phi in (-pi, pi].
inline void directionAngles(const Vec3 &r, double *theta, double *phi)
{
    *theta = std::acos(std::clamp(r.z(), -1.0, 1.0));
    *phi = (std::abs(r.x()) < 1e-15 && std::abs(r.y()) < 1e-15) ? 0.0 : std::atan2(r.y(), r.x());
}

// Polarisation components of a field (see the conventions above). phi in radians.
inline cplx ludwig3X(const Field &f, double phi) { return f.eTheta * std::cos(phi) - f.ePhi * std::sin(phi); }
inline cplx ludwig3Y(const Field &f, double phi) { return f.eTheta * std::sin(phi) + f.ePhi * std::cos(phi); }
inline cplx rhcp(const Field &f) { return (f.eTheta + cplx(0, 1) * f.ePhi) / std::numbers::sqrt2; }
inline cplx lhcp(const Field &f) { return (f.eTheta - cplx(0, 1) * f.ePhi) / std::numbers::sqrt2; }

// Axial ratio in dB (0 for circular, large for linear), from the CP components.
inline double axialRatioDb(const Field &f)
{
    const double r = std::abs(rhcp(f));
    const double l = std::abs(lhcp(f));
    const double num = r + l;
    const double den = std::abs(r - l);
    if (num <= 0)
        return 0.0;
    if (den <= num * 1e-5) // linear polarisation: capped at 100 dB
        return 100.0;
    return std::min(20.0 * std::log10(num / den), 100.0);
}

} // namespace lvd::ant

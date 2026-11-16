#include "rfdata/NetworkMath.h"

#include <cmath>
#include <limits>

namespace lvd::rf {

namespace {

// Condition check: a matrix whose LU has a (near) zero pivot is treated as singular.
std::optional<CMatrix> inverse(const CMatrix &m)
{
    const Eigen::FullPivLU<CMatrix> lu(m);
    if (!lu.isInvertible())
        return std::nullopt;
    return lu.inverse();
}

Eigen::VectorXd sqrtZ0(const std::vector<double> &z0)
{
    Eigen::VectorXd q(static_cast<Eigen::Index>(z0.size()));
    for (size_t i = 0; i < z0.size(); ++i)
        q(static_cast<Eigen::Index>(i)) = std::sqrt(z0[i]);
    return q;
}

} // namespace

QString NetworkData::validate() const
{
    const int n = portCount();
    if (n < 1)
        return QStringLiteral("the network has no ports");
    if (s.size() != frequencies.size())
        return QStringLiteral("%1 frequencies but %2 S-matrices").arg(frequencies.size()).arg(s.size());
    for (size_t i = 0; i < frequencies.size(); ++i) {
        if (!std::isfinite(frequencies[i]) || frequencies[i] < 0)
            return QStringLiteral("invalid frequency %1").arg(frequencies[i]);
        if (i > 0 && frequencies[i] <= frequencies[i - 1])
            return QStringLiteral("frequencies are not strictly increasing at %1 Hz").arg(frequencies[i]);
        if (s[i].rows() != n || s[i].cols() != n)
            return QStringLiteral("S-matrix %1 is not %2 x %2").arg(i).arg(n);
    }
    for (double z : z0) {
        if (!(z > 0) || !std::isfinite(z))
            return QStringLiteral("invalid reference impedance %1").arg(z);
    }
    if (noise.size() != noiseFrequencies.size())
        return QStringLiteral("noise parameters and noise frequencies differ in number");
    if (!noise.empty() && n != 2)
        return QStringLiteral("noise parameters require a two-port");
    if (!noiseCorrelation.empty() && noiseCorrelation.size() != frequencies.size())
        return QStringLiteral("noise correlation matrices and frequencies differ in number");
    return {};
}

std::optional<CMatrix> sToZ(const CMatrix &s, const std::vector<double> &z0)
{
    const Eigen::Index n = s.rows();
    const CMatrix identity = CMatrix::Identity(n, n);
    const auto inv = inverse(identity - s);
    if (!inv)
        return std::nullopt;
    const CMatrix z = (identity + s) * *inv; // normalised
    const Eigen::VectorXd q = sqrtZ0(z0);
    return q.asDiagonal() * z * q.asDiagonal();
}

CMatrix zToS(const CMatrix &z, const std::vector<double> &z0)
{
    const Eigen::Index n = z.rows();
    const CMatrix identity = CMatrix::Identity(n, n);
    const Eigen::VectorXd qInv = sqrtZ0(z0).cwiseInverse();
    const CMatrix zn = qInv.asDiagonal() * z * qInv.asDiagonal();
    return (zn - identity) * (zn + identity).fullPivLu().inverse();
}

std::optional<CMatrix> sToY(const CMatrix &s, const std::vector<double> &z0)
{
    const Eigen::Index n = s.rows();
    const CMatrix identity = CMatrix::Identity(n, n);
    const auto inv = inverse(identity + s);
    if (!inv)
        return std::nullopt;
    const CMatrix y = (identity - s) * *inv; // normalised
    const Eigen::VectorXd qInv = sqrtZ0(z0).cwiseInverse();
    return qInv.asDiagonal() * y * qInv.asDiagonal();
}

CMatrix yToS(const CMatrix &y, const std::vector<double> &z0)
{
    const Eigen::Index n = y.rows();
    const CMatrix identity = CMatrix::Identity(n, n);
    const Eigen::VectorXd q = sqrtZ0(z0);
    const CMatrix yn = q.asDiagonal() * y * q.asDiagonal();
    return (identity - yn) * (identity + yn).fullPivLu().inverse();
}

// Frickey, "Conversions between S, Z, Y, h, ABCD and T parameters which are
// valid for complex source and load impedances", IEEE T-MTT 1994, with real
// reference impedances.
std::optional<CMatrix> sToAbcd(const CMatrix &s, double z01, double z02)
{
    const Complex s11 = s(0, 0), s12 = s(0, 1), s21 = s(1, 0), s22 = s(1, 1);
    if (std::abs(s21) < 1e-300)
        return std::nullopt;
    const Complex den = 2.0 * s21 * std::sqrt(z01 * z02);
    CMatrix abcd(2, 2);
    abcd(0, 0) = ((z01 + s11 * z01) * (1.0 - s22) + s12 * s21 * z01) / den;
    abcd(0, 1) = ((z01 + s11 * z01) * (z02 + s22 * z02) - s12 * s21 * z01 * z02) / den;
    abcd(1, 0) = ((1.0 - s11) * (1.0 - s22) - s12 * s21) / den;
    abcd(1, 1) = ((1.0 - s11) * (z02 + s22 * z02) + s12 * s21 * z02) / den;
    return abcd;
}

CMatrix abcdToS(const CMatrix &abcd, double z01, double z02)
{
    const Complex a = abcd(0, 0), b = abcd(0, 1), c = abcd(1, 0), d = abcd(1, 1);
    const Complex den = a * z02 + b + c * z01 * z02 + d * z01;
    const double root = std::sqrt(z01 * z02);
    CMatrix s(2, 2);
    s(0, 0) = (a * z02 + b - c * z01 * z02 - d * z01) / den;
    s(0, 1) = 2.0 * (a * d - b * c) * root / den;
    s(1, 0) = 2.0 * root / den;
    s(1, 1) = (-a * z02 + b - c * z01 * z02 + d * z01) / den;
    return s;
}

std::optional<CMatrix> sToT(const CMatrix &s)
{
    const Complex s11 = s(0, 0), s12 = s(0, 1), s21 = s(1, 0), s22 = s(1, 1);
    if (std::abs(s21) < 1e-300)
        return std::nullopt;
    CMatrix t(2, 2);
    t(0, 0) = -(s11 * s22 - s12 * s21) / s21;
    t(0, 1) = s11 / s21;
    t(1, 0) = -s22 / s21;
    t(1, 1) = 1.0 / s21;
    return t;
}

CMatrix tToS(const CMatrix &t)
{
    const Complex t11 = t(0, 0), t12 = t(0, 1), t21 = t(1, 0), t22 = t(1, 1);
    CMatrix s(2, 2);
    s(0, 0) = t12 / t22;
    s(0, 1) = (t11 * t22 - t12 * t21) / t22;
    s(1, 0) = 1.0 / t22;
    s(1, 1) = -t21 / t22;
    return s;
}

// With a = (V + Z I)/(2 sqrt Z) and b = (V - Z I)/(2 sqrt Z):
//   a' = K (a - G b),  b' = K (b - G a),
//   G = (Z' - Z)/(Z' + Z),  K = (Z + Z')/(2 sqrt(Z Z')).
// Hence S' = K (S - G)(I - G S)^-1 K^-1.
CMatrix renormalize(const CMatrix &s, const std::vector<double> &z0Old, const std::vector<double> &z0New)
{
    const Eigen::Index n = s.rows();
    Eigen::VectorXcd g(n), k(n);
    for (Eigen::Index i = 0; i < n; ++i) {
        const double zo = z0Old[static_cast<size_t>(i)], zn = z0New[static_cast<size_t>(i)];
        g(i) = (zn - zo) / (zn + zo);
        k(i) = (zo + zn) / (2.0 * std::sqrt(zo * zn));
    }
    const CMatrix identity = CMatrix::Identity(n, n);
    const CMatrix gm = g.asDiagonal();
    return k.asDiagonal() * (s - gm) * (identity - gm * s).fullPivLu().inverse() * k.cwiseInverse().asDiagonal();
}

// b = S a + c. Substituting the new waves gives c' = K [(S - G)(I - G S)^-1 G + I] c.
CMatrix noiseRenormalization(const CMatrix &s, const std::vector<double> &z0Old, const std::vector<double> &z0New)
{
    const Eigen::Index n = s.rows();
    Eigen::VectorXcd g(n), k(n);
    for (Eigen::Index i = 0; i < n; ++i) {
        const double zo = z0Old[static_cast<size_t>(i)], zn = z0New[static_cast<size_t>(i)];
        g(i) = (zn - zo) / (zn + zo);
        k(i) = (zo + zn) / (2.0 * std::sqrt(zo * zn));
    }
    const CMatrix identity = CMatrix::Identity(n, n);
    const CMatrix gm = g.asDiagonal();
    return k.asDiagonal() * ((s - gm) * (identity - gm * s).fullPivLu().inverse() * gm + identity);
}

Complex reflectionFromImpedance(Complex z, double z0)
{
    if (!std::isfinite(z.real()) || !std::isfinite(z.imag()))
        return 1.0;
    return (z - z0) / (z + z0);
}

Complex impedanceFromReflection(Complex gamma, double z0)
{
    const Complex den = 1.0 - gamma;
    if (std::abs(den) < 1e-300)
        return Complex(std::numeric_limits<double>::infinity(), 0);
    return z0 * (1.0 + gamma) / den;
}

double db20(Complex value)
{
    const double magnitude = std::abs(value);
    return magnitude > 0 ? 20.0 * std::log10(magnitude) : -std::numeric_limits<double>::infinity();
}

double db10(double value)
{
    return value > 0 ? 10.0 * std::log10(value) : -std::numeric_limits<double>::infinity();
}

Complex fromDbPhase(double db, double phaseDegrees)
{
    return std::polar(std::pow(10.0, db / 20.0), phaseDegrees * Pi / 180.0);
}

} // namespace lvd::rf

#include "rfdata/Discontinuities.h"

#include <algorithm>
#include <cmath>

namespace lvd::rf {

namespace {

constexpr double Mu0 = 1.25663706127e-6;                               // H/m (CODATA 2022)
constexpr double Epsilon0 = 1.0 / (Mu0 * SpeedOfLight * SpeedOfLight); // F/m
const double Eta0 = std::sqrt(Mu0 / Epsilon0);                         // ohm

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool positive(double value)
{
    return value > 0 && std::isfinite(value);
}

// Impedance and effective permittivity of a lossless strip of width w (the
// discontinuity formulas are written for the lossless line).
std::optional<LineProperties> losslessLine(const MicrostripSubstrate &substrate, double w, double frequency,
                                           QString *error)
{
    MicrostripGeometry g = substrate.line(w);
    g.tand = 0;
    g.sigma = 0;
    g.rough = 0;
    return microstrip(g, frequency, error);
}

QString checkSubstrate(const MicrostripSubstrate &s)
{
    if (!positive(s.h))
        return QStringLiteral("substrate height must be positive");
    if (!(s.er >= 1) || !std::isfinite(s.er))
        return QStringLiteral("relative permittivity must be at least 1");
    if (!(s.t >= 0))
        return QStringLiteral("strip thickness must not be negative");
    return {};
}

// Per-unit-length inductance of a line, Z0 sqrt(eeff)/c0.
double inductancePerLength(const LineProperties &line)
{
    return line.zc.real() * std::sqrt(line.epsilonEffective) / SpeedOfLight;
}

} // namespace

// M. Kirschning, R. H. Jansen and N. H. L. Koster, "Accurate model for open
// end effect of microstrip lines", Electronics Letters 17(3), 1981,
// pp. 123-125 (u = w/h, eeff at the frequency, as in Qucs MOPEN):
//   Q1 = 0.434907 (eeff^0.81 + 0.26)/(eeff^0.81 - 0.189) (u^0.8544 + 0.236)/(u^0.8544 + 0.87)
//   Q2 = 1 + u^0.371/(2.358 er + 1)
//   Q3 = 1 + 0.5274 atan(0.084 u^(1.9413/Q2)) / eeff^0.9236
//   Q4 = 1 + 0.0377 atan(0.067 u^1.456) (6 - 5 exp(0.036 (1 - er)))
//   Q5 = 1 - 0.218 exp(-7.5 u)
//   dl/h = Q1 Q3 Q5 / Q4.
// Stated accuracy 0.2 % for 0.01 <= u <= 100 and er <= 50.
double openEndLengthExtension(double w, double h, double er, double eeff)
{
    const double u = w / h;
    const double e = std::pow(eeff, 0.81);
    const double v = std::pow(u, 0.8544);
    const double q1 = 0.434907 * (e + 0.26) / (e - 0.189) * (v + 0.236) / (v + 0.87);
    const double q2 = 1.0 + std::pow(u, 0.371) / (2.358 * er + 1.0);
    const double q3 = 1.0 + 0.5274 * std::atan(0.084 * std::pow(u, 1.9413 / q2)) / std::pow(eeff, 0.9236);
    const double q4 = 1.0 + 0.0377 * std::atan(0.067 * std::pow(u, 1.456)) * (6.0 - 5.0 * std::exp(0.036 * (1.0 - er)));
    const double q5 = 1.0 - 0.218 * std::exp(-7.5 * u);
    return h * q1 * q3 * q5 / q4;
}

// The extra length dl stores the energy of a capacitance
// C = dl sqrt(eeff)/(c0 Z0) (eeff and Z0 with dispersion).
std::optional<double> openEndCapacitance(const MicrostripSubstrate &substrate, double w, double frequency, QString *error)
{
    const auto line = losslessLine(substrate, w, frequency, error);
    if (!line)
        return std::nullopt;
    const double dl = openEndLengthExtension(w, substrate.h, substrate.er, line->epsilonEffective);
    return dl * std::sqrt(line->epsilonEffective) / (SpeedOfLight * line->zc.real());
}

// Gap: M. Kirschning, R. H. Jansen and N. H. L. Koster, "Measurement and
// computer-aided modeling of microstrip discontinuities by an improved
// resonator method", IEEE MTT-S Digest 1983, pp. 495-497, and M.
// Kirschning's dissertation (Duisburg, 1984); the form used by Qucs (MGAP).
// With W1 <= W2 (ports swapped otherwise), u1 = W1/h, r = W2/W1, g = s/h:
//   Q5 = 1.23/(1 + 0.12 (r - 1)^0.9)
//   Q1 = 0.04598 (0.03 + u1^Q5)(0.272 + 0.07 er)
//   Q2 = 0.107 (u1 + 9) g^3.23 + 2.09 g^1.05 (1.5 + 0.3 u1)/(1 + 0.6 u1)
//   Q3 = exp(-0.5978 r^1.35) - 0.55,  Q4 = exp(-0.5978 r^-1.35) - 0.55
//   Cs = 500 pF/m h exp(-1.86 g) Q1 (1 + 4.19 (1 - exp(-0.785 sqrt(1/u1) r)))
//   C1 = Cend(W1) (Q2 + Q3)/(Q2 + 1),  C2 = Cend(W2) (Q2 + Q4)/(Q2 + 1)
// with Cend the open-end capacitance above. Stated validity:
// 0.1 <= W/h <= 3, 1 <= W2/W1 <= 3, 6 <= er <= 13, 0.2 <= s/h <= infinity,
// 0.2 GHz <= f <= 18 GHz.
std::optional<GapCircuit> microstripGap(const MicrostripSubstrate &substrate, double w1, double w2, double s,
                                        double frequency, QString *error)
{
    QString problem = checkSubstrate(substrate);
    if (problem.isEmpty() && (!positive(w1) || !positive(w2) || !positive(s)))
        problem = QStringLiteral("widths and gap must be positive");
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Microstrip gap: %1").arg(problem));
        return std::nullopt;
    }
    const bool flip = w2 < w1;
    const double narrow = flip ? w2 : w1, wide = flip ? w1 : w2;
    const auto cNarrow = openEndCapacitance(substrate, narrow, frequency, error);
    const auto cWide = openEndCapacitance(substrate, wide, frequency, error);
    if (!cNarrow || !cWide)
        return std::nullopt;
    const double u1 = narrow / substrate.h, r = wide / narrow, g = s / substrate.h, er = substrate.er;
    const double q5 = 1.23 / (1.0 + 0.12 * std::pow(r - 1.0, 0.9));
    const double q1 = 0.04598 * (0.03 + std::pow(u1, q5)) * (0.272 + 0.07 * er);
    const double q2 = 0.107 * (u1 + 9.0) * std::pow(g, 3.23) + 2.09 * std::pow(g, 1.05) * (1.5 + 0.3 * u1) / (1.0 + 0.6 * u1);
    const double q3 = std::exp(-0.5978 * std::pow(r, 1.35)) - 0.55;
    const double q4 = std::exp(-0.5978 * std::pow(r, -1.35)) - 0.55;
    GapCircuit gap;
    gap.cs = 500e-12 * substrate.h * std::exp(-1.86 * g) * q1
             * (1.0 + 4.19 * (1.0 - std::exp(-0.785 * std::sqrt(1.0 / u1) * r)));
    const double cn = *cNarrow * (q2 + q3) / (q2 + 1.0);
    const double cw = *cWide * (q2 + q4) / (q2 + 1.0);
    gap.c1 = flip ? cw : cn;
    gap.c2 = flip ? cn : cw;
    return gap;
}

// Step in width: K. C. Gupta, R. Garg, I. Bahl and P. Bhartia, "Microstrip
// Lines and Slotlines", 2nd ed., Artech House, 1996 (after Garg and Bahl,
// IEEE T-MTT 26(4), 1978), as in Qucs (MSTEP), with r = Wwide/Wnarrow:
//   Cs = sqrt(W1 W2) [(10.1 log10 er + 2.33) r - 12.6 log10 er - 3.17] pF/m
//        (er <= 10, 1.5 <= r <= 3.5, error < 10 %)
//   Ls = h [40.5 (r - 1) - 75 log10 r + 0.2 (r - 1)^2] nH/m
//        (r <= 5, W2/h = 1, error < 5 %)
// Ls is split between the ports in proportion to the inductance per unit
// length Z0 sqrt(eeff)/c0 of each line. Cs is set to zero where the formula
// turns negative (r below about 1.3), so a step between equal widths
// vanishes.
std::optional<StepCircuit> microstripStep(const MicrostripSubstrate &substrate, double w1, double w2, double frequency,
                                          QString *error)
{
    QString problem = checkSubstrate(substrate);
    if (problem.isEmpty() && (!positive(w1) || !positive(w2)))
        problem = QStringLiteral("widths must be positive");
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Microstrip step: %1").arg(problem));
        return std::nullopt;
    }
    const auto line1 = losslessLine(substrate, w1, frequency, error);
    const auto line2 = losslessLine(substrate, w2, frequency, error);
    if (!line1 || !line2)
        return std::nullopt;
    const double r = std::max(w1, w2) / std::min(w1, w2);
    const double logEr = std::log10(substrate.er);
    const double cs = std::sqrt(w1 * w2) * (r * (10.1 * logEr + 2.33) - 12.6 * logEr - 3.17) * 1e-12;
    const double ls = substrate.h * ((r - 1.0) * (40.5 + 0.2 * (r - 1.0)) - 75.0 * std::log10(r)) * 1e-9;
    const double lw1 = inductancePerLength(*line1), lw2 = inductancePerLength(*line2);
    StepCircuit step;
    step.c = std::max(0.0, cs);
    step.l1 = ls * lw1 / (lw1 + lw2);
    step.l2 = ls * lw2 / (lw1 + lw2);
    return step;
}

// 90 degree bends: Kirschning, Jansen and Koster, IEEE MTT-S Digest 1983
// (reference above), as in Qucs (MCORN, MMBEND), u = W/h:
//   unmitred:     C = W [(10.35 er + 2.5) u + (2.6 er + 5.64)] pF/m,
//                 L = 220 h [1 - 1.35 exp(-0.18 u^1.39)] nH/m
//   50 % mitred:  C = W [(3.93 er + 0.62) u + (7.6 er + 3.80)] pF/m,
//                 L = 440 h [1 - 1.062 exp(-0.177 u^0.947)] nH/m
// in a T of series L, shunt C, series L. Stated validity 0.2 <= u <= 6,
// 2.36 <= er <= 10.4, f h <= 12 GHz mm; precision about 0.3 %.
std::optional<BendCircuit> microstripBend(const MicrostripSubstrate &substrate, double w, bool mitred, QString *error)
{
    QString problem = checkSubstrate(substrate);
    if (problem.isEmpty() && !positive(w))
        problem = QStringLiteral("width must be positive");
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Microstrip bend: %1").arg(problem));
        return std::nullopt;
    }
    const double u = w / substrate.h, er = substrate.er;
    BendCircuit bend;
    if (mitred) {
        bend.c = w * ((3.93 * er + 0.62) * u + (7.6 * er + 3.80)) * 1e-12;
        bend.l = 440.0 * substrate.h * (1.0 - 1.062 * std::exp(-0.177 * std::pow(u, 0.947))) * 1e-9;
    } else {
        bend.c = w * ((10.35 * er + 2.5) * u + (2.6 * er + 5.64)) * 1e-12;
        bend.l = 220.0 * substrate.h * (1.0 - 1.35 * std::exp(-0.18 * std::pow(u, 1.39))) * 1e-9;
    }
    return bend;
}

// T-junction: E. Hammerstad, "Computer-aided design of microstrip couplers
// with accurate discontinuity models", IEEE MTT-S Digest 1981, pp. 54-56, as
// in Qucs (MTEE). Main arms a (port 1) and b (port 2), side arm 2 (port 3);
// Z, eeff of each arm line with dispersion. Equivalent parallel-plate widths
// D = eta0 h/(Z sqrt(eeff)); first higher-mode cut-off fp = 0.4 MHz m Z/h;
// lambda = c0/(sqrt(eeff) f).
//   d_x  = 0.055 D2 Zx/Z2 (1 - 2 Zx/Z2 (f/fp_x)^2),  x = a, b
//   L_x  = W3/2 - d_x
//   R = sqrt(Za Zb)/Z2,  q = f^2/(fpa fpb)
//   d2   = sqrt(Da Db) (0.5 - R (0.05 + 0.7 exp(-1.6 R) + 0.25 R q - 0.17 ln R))
//   L2   = max(W1, W2)/2 - d2
//   Tx^2 = 1 - pi (f/fp_x)^2 ((Zx/Z2)^2/12 + (0.5 - d2/Dx)^2)  (kept above a tiny value)
//   BT   = 5.5 sqrt(Da Db/(lambda_a lambda_b)) (er + 2)/er / (Z2 sqrt(Ta^2 Tb^2))
//          sqrt(da db)/D2 (1 + 0.9 ln R + 4.5 R q - 4.4 exp(-1.3 R) - 20 (Z2/eta0)^2)
// The junction: arm x through a 1:Tx transformer, arm 2 directly, onto one
// node with shunt susceptance BT; the arm lines (lengths L_x, L2, possibly
// negative, lossy like the microstrip) lead from the reference planes to it.
std::optional<TeeCircuit> microstripTeeCircuit(const MicrostripSubstrate &substrate, double w1, double w2, double w3,
                                               double frequency, QString *error)
{
    QString problem = checkSubstrate(substrate);
    if (problem.isEmpty() && (!positive(w1) || !positive(w2) || !positive(w3)))
        problem = QStringLiteral("widths must be positive");
    if (problem.isEmpty() && !positive(frequency))
        problem = QStringLiteral("the model needs a frequency above 0 Hz");
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Microstrip tee: %1").arg(problem));
        return std::nullopt;
    }
    const auto a = losslessLine(substrate, w1, frequency, error);
    const auto b = losslessLine(substrate, w2, frequency, error);
    const auto side = losslessLine(substrate, w3, frequency, error);
    if (!a || !b || !side)
        return std::nullopt;
    const double h = substrate.h, f = frequency;
    const double za = a->zc.real(), zb = b->zc.real(), z2 = side->zc.real();
    const double ea = a->epsilonEffective, eb = b->epsilonEffective;
    const double da = Eta0 / za * h / std::sqrt(ea);
    const double db = Eta0 / zb * h / std::sqrt(eb);
    const double d2w = Eta0 / z2 * h / std::sqrt(side->epsilonEffective);
    const double fpa = 0.4e6 * za / h, fpb = 0.4e6 * zb / h;
    const double lambdaA = SpeedOfLight / std::sqrt(ea) / f, lambdaB = SpeedOfLight / std::sqrt(eb) / f;
    const double dispA = 0.055 * d2w * za / z2 * (1.0 - 2.0 * za / z2 * std::pow(f / fpa, 2));
    const double dispB = 0.055 * d2w * zb / z2 * (1.0 - 2.0 * zb / z2 * std::pow(f / fpb, 2));
    const double r = std::sqrt(za * zb) / z2;
    const double q = f * f / fpa / fpb;
    const double disp2 = std::sqrt(da * db) * (0.5 - r * (0.05 + 0.7 * std::exp(-1.6 * r) + 0.25 * r * q - 0.17 * std::log(r)));
    TeeCircuit tee;
    tee.la = 0.5 * w3 - dispA;
    tee.lb = 0.5 * w3 - dispB;
    tee.l3 = 0.5 * std::max(w1, w2) - disp2;
    constexpr double Tiny = 1e-30;
    tee.ta2 = std::max(Tiny, 1.0 - Pi * std::pow(f / fpa, 2) * (std::pow(za / z2, 2) / 12.0 + std::pow(0.5 - disp2 / da, 2)));
    tee.tb2 = std::max(Tiny, 1.0 - Pi * std::pow(f / fpb, 2) * (std::pow(zb / z2, 2) / 12.0 + std::pow(0.5 - disp2 / db, 2)));
    tee.bt = 5.5 * std::sqrt(da * db / lambdaA / lambdaB) * (substrate.er + 2.0) / substrate.er / z2
             / std::sqrt(tee.ta2 * tee.tb2) * std::sqrt(std::abs(dispA * dispB)) / d2w
             * (1.0 + 0.9 * std::log(r) + 4.5 * r * q - 4.4 * std::exp(-1.3 * r) - 20.0 * std::pow(z2 / Eta0, 2));
    // The arm lines with the substrate's losses.
    const auto lossy = [&](double w) { return microstrip(substrate.line(w), frequency, error); };
    const auto la = lossy(w1), lb = lossy(w2), l3 = lossy(w3);
    if (!la || !lb || !l3)
        return std::nullopt;
    tee.lineA = *la;
    tee.lineB = *lb;
    tee.line3 = *l3;
    return tee;
}

// S of the junction core (as Qucs MTEE), with b = BT z0:
//   n1 = Ta^2 (1 + 1/Tb^2 + j b), n2 = Tb^2 (1 + 1/Ta^2 + j b), n3 = 1/Ta^2 + 1/Tb^2 + j b
//   Sii = (1 - ni)/(1 + ni), S13 = 2 Ta/(1 + n1), S23 = 2 Tb/(1 + n2),
//   S12 = 2/(Ta Tb (1 + j b) + Ta/Tb + Tb/Ta),
// then the arm lines in front of each port.
std::optional<CMatrix> microstripTee(const MicrostripSubstrate &substrate, double w1, double w2, double w3,
                                     double frequency, double z0, QString *error)
{
    const auto tee = microstripTeeCircuit(substrate, w1, w2, w3, frequency, error);
    if (!tee)
        return std::nullopt;
    const Complex jb(0, tee->bt * z0);
    const double ta2 = tee->ta2, tb2 = tee->tb2;
    const Complex n1 = ta2 * (1.0 + 1.0 / tb2 + jb);
    const Complex n2 = tb2 * (1.0 + 1.0 / ta2 + jb);
    const Complex n3 = 1.0 / ta2 + 1.0 / tb2 + jb;
    CMatrix s(3, 3);
    s(0, 0) = (1.0 - n1) / (1.0 + n1);
    s(1, 1) = (1.0 - n2) / (1.0 + n2);
    s(2, 2) = (1.0 - n3) / (1.0 + n3);
    s(0, 2) = s(2, 0) = 2.0 * std::sqrt(ta2) / (1.0 + n1);
    s(1, 2) = s(2, 1) = 2.0 * std::sqrt(tb2) / (1.0 + n2);
    s(0, 1) = s(1, 0) = 2.0 / (std::sqrt(ta2 * tb2) * (1.0 + jb) + std::sqrt(ta2 / tb2) + std::sqrt(tb2 / ta2));
    const auto lineS = [z0](const LineProperties &line, double length) {
        const Complex gl = line.gamma * length;
        const Complex a = std::cosh(gl), b = line.zc * std::sinh(gl), c = std::sinh(gl) / line.zc, d = a;
        const Complex den = a + b / z0 + c * z0 + d;
        CMatrix t(2, 2);
        t << (a + b / z0 - c * z0 - d) / den, 2.0 / den, 2.0 / den, (-a + b / z0 - c * z0 + d) / den;
        return t;
    };
    s = connectToPort(s, 0, lineS(tee->lineA, tee->la));
    s = connectToPort(s, 1, lineS(tee->lineB, tee->lb));
    s = connectToPort(s, 2, lineS(tee->line3, tee->l3));
    return s;
}

// With the line's S = [[t11, t12], [t21, t22]] (port 2 facing port k):
//   S'kk = t11 + t12 t21 Skk/(1 - t22 Skk)
//   S'ik = Sik t21/(1 - t22 Skk),  S'ki = t12 Ski/(1 - t22 Skk)
//   S'ij = Sij + Sik t22 Skj/(1 - t22 Skk)
CMatrix connectToPort(const CMatrix &s, int k, const CMatrix &line)
{
    const Complex t11 = line(0, 0), t12 = line(0, 1), t21 = line(1, 0), t22 = line(1, 1);
    const Complex den = 1.0 - t22 * s(k, k);
    CMatrix out = s;
    for (int i = 0; i < s.rows(); ++i) {
        for (int j = 0; j < s.cols(); ++j) {
            if (i == k && j == k)
                out(i, j) = t11 + t12 * t21 * s(k, k) / den;
            else if (j == k)
                out(i, j) = s(i, k) * t21 / den;
            else if (i == k)
                out(i, j) = t12 * s(k, j) / den;
            else
                out(i, j) = s(i, j) + s(i, k) * t22 * s(k, j) / den;
        }
    }
    return out;
}

// Via hole: M. E. Goldfarb and R. A. Pucel, "Modeling via hole grounds in
// microstrip", IEEE Microwave and Guided Wave Letters 1(6), 1991,
// pp. 135-137, with r = d/2, a = sqrt(r^2 + h^2):
//   L = mu0/(2 pi) [h ln((h + a)/r) + 1.5 (r - a)]
//   R = R_dc sqrt(1 + f/fs), R_dc = h/(sigma pi (r^2 - (r - t)^2)), fs = 1/(pi mu0 sigma t^2)
// (the resistance as in Qucs MVIA). Stated for h < 0.03 lambda0.
std::optional<Complex> viaImpedance(double d, double h, double t, double sigma, double frequency, QString *error)
{
    if (!positive(d) || !positive(h) || !(t >= 0) || !(sigma >= 0) || !(frequency >= 0)) {
        setError(error, QStringLiteral("Via: diameter and height must be positive, thickness and conductivity not negative"));
        return std::nullopt;
    }
    const double r = d / 2.0;
    if (t > r) {
        setError(error, QStringLiteral("Via: the plating is thicker than the via radius"));
        return std::nullopt;
    }
    const double a = std::sqrt(r * r + h * h);
    const double l = Mu0 / (2.0 * Pi) * (h * std::log((h + a) / r) + 1.5 * (r - a));
    double resistance = 0;
    if (sigma > 0 && t > 0) {
        const double rdc = h / (sigma * Pi * (r * r - (r - t) * (r - t)));
        resistance = rdc * std::sqrt(1.0 + frequency * Pi * Mu0 * sigma * t * t);
    }
    return Complex(resistance, 2.0 * Pi * frequency * l);
}

} // namespace lvd::rf

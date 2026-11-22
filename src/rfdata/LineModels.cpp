#include "rfdata/LineModels.h"

#include <cmath>

namespace lvd::rf {

namespace {

constexpr double Mu0 = 1.25663706127e-6;                         // H/m (CODATA 2022)
constexpr double Epsilon0 = 1.0 / (Mu0 * SpeedOfLight * SpeedOfLight); // F/m
const double Eta0 = std::sqrt(Mu0 / Epsilon0);                   // ohm, free-space wave impedance

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool positive(double value)
{
    return value > 0 && std::isfinite(value);
}

bool nonNegative(double value)
{
    return value >= 0 && std::isfinite(value);
}

// Checks shared by all models; returns an error message or an empty string.
QString checkCommon(double er, double tand, double sigma, double frequency)
{
    if (!(er >= 1) || !std::isfinite(er))
        return QStringLiteral("relative permittivity must be at least 1");
    if (!nonNegative(tand))
        return QStringLiteral("loss tangent must not be negative");
    if (!nonNegative(sigma))
        return QStringLiteral("conductivity must not be negative (0 = perfect conductor)");
    if (!nonNegative(frequency))
        return QStringLiteral("frequency must not be negative");
    return {};
}

// Surface resistance of a good conductor, sqrt(pi f mu0 / sigma); sigma = 0
// stands for a perfect conductor.
double surfaceResistance(double frequency, double sigma)
{
    return sigma > 0 ? std::sqrt(Pi * frequency * Mu0 / sigma) : 0.0;
}

QString checkRoughness(double rough)
{
    return nonNegative(rough) ? QString() : QStringLiteral("surface roughness must not be negative");
}

// Complete elliptic integral of the first kind K(k) by the arithmetic-geometric mean.
double ellipticK(double k)
{
    double a = 1.0, b = std::sqrt((1.0 - k) * (1.0 + k));
    for (int i = 0; i < 60 && std::abs(a - b) > 1e-16 * a; ++i) {
        const double next = 0.5 * (a + b);
        b = std::sqrt(a * b);
        a = next;
    }
    return Pi / (2.0 * a);
}

double complementaryModulus(double k)
{
    return std::sqrt((1.0 - k) * (1.0 + k));
}

// K(k) / K'(k), K'(k) = K(sqrt(1 - k^2)).
double ellipticRatio(double k)
{
    return ellipticK(k) / ellipticK(complementaryModulus(k));
}

// Line properties from the characteristic impedance z0 of the lossless line,
// the effective permittivity and the attenuation constants. The line is
// represented by constant per-unit-length R = 2 z0 alphaC, L = z0 beta / w,
// G = 2 alphaD / z0, C = beta / (w z0), so that
//   zc = z0 sqrt((1 - 2j alphaC/beta) / (1 - 2j alphaD/beta)),
//   gamma = j beta sqrt((1 - 2j alphaC/beta)(1 - 2j alphaD/beta)) ~ alphaC + alphaD + j beta.
LineProperties lineFromLossless(double z0, double epsilonEffective, double alphaC, double alphaD, double frequency)
{
    LineProperties p;
    p.epsilonEffective = epsilonEffective;
    p.alphaConductor = alphaC;
    p.alphaDielectric = alphaD;
    const double beta = 2.0 * Pi * frequency * std::sqrt(epsilonEffective) / SpeedOfLight;
    if (beta <= 0) {
        p.zc = z0;
        p.gamma = 0;
        return p;
    }
    const Complex series(1.0, -2.0 * alphaC / beta);
    const Complex shunt(1.0, -2.0 * alphaD / beta);
    p.zc = z0 * std::sqrt(series / shunt);
    p.gamma = Complex(0, beta) * std::sqrt(series * shunt);
    return p;
}

// Dielectric loss of a quasi-TEM line with filling factor
// q = (eeff - 1)/(er - 1): alphaD = pi f/c * er/sqrt(eeff) * q * tand (Np/m).
double quasiTemDielectricLoss(double er, double epsilonEffective, double tand, double frequency)
{
    const double filling = er - 1.0 > 1e-12 ? (epsilonEffective - 1.0) / (er - 1.0) : 1.0;
    return Pi * frequency / SpeedOfLight * er / std::sqrt(epsilonEffective) * filling * tand;
}

// Hammerstad and Jensen: impedance of a zero-thickness strip in air, u = w/h.
double hammerstadZ01(double u)
{
    const double f = 6.0 + (2.0 * Pi - 6.0) * std::exp(-std::pow(30.666 / u, 0.7528));
    return Eta0 / (2.0 * Pi) * std::log(f / u + std::sqrt(1.0 + 4.0 / (u * u)));
}

// Hammerstad and Jensen: effective permittivity of a zero-thickness strip.
double hammerstadEpsilon(double u, double er)
{
    const double a = 1.0 + std::log((std::pow(u, 4) + std::pow(u / 52.0, 2)) / (std::pow(u, 4) + 0.432)) / 49.0
                     + std::log(1.0 + std::pow(u / 18.1, 3)) / 18.7;
    const double b = 0.564 * std::pow((er - 0.9) / (er + 3.0), 0.053);
    return (er + 1.0) / 2.0 + (er - 1.0) / 2.0 * std::pow(1.0 + 10.0 / u, -a * b);
}

// Kirschning and Jansen single-line dispersion (Electronics Letters 18(6),
// 1982): effective permittivity at the normalised frequency fn = f h in
// GHz mm, from the static value eeStatic.
double kirschningEpsilon(double u, double fn, double er, double eeStatic)
{
    const double p1 = 0.27488 + (0.6315 + 0.525 / std::pow(1.0 + 0.0157 * fn, 20)) * u - 0.065683 * std::exp(-8.7513 * u);
    const double p2 = 0.33622 * (1.0 - std::exp(-0.03442 * er));
    const double p3 = 0.0363 * std::exp(-4.6 * u) * (1.0 - std::exp(-std::pow(fn / 38.7, 4.97)));
    const double p4 = 1.0 + 2.751 * (1.0 - std::exp(-std::pow(er / 15.916, 8)));
    const double p = p1 * p2 * std::pow((0.1844 + p3 * p4) * fn, 1.5763);
    return er - (er - eeStatic) / (1.0 + p);
}

// Jansen and Kirschning (AEU 37, 1983) power-current impedance at fn:
// Z(f) = Z(0) (R13/R14)^R17. *exponent receives R17 (the coupled-line
// impedance dispersion reuses it).
double kirschningImpedance(double u, double fn, double er, double eeStatic, double eeF, double z0Static,
                           double *exponent = nullptr)
{
    const double r1 = std::min(0.03891 * std::pow(er, 1.4), 20.0);
    const double r2 = std::min(0.2671 * std::pow(u, 7), 20.0);
    const double r3 = 4.766 * std::exp(-3.228 * std::pow(u, 0.641));
    const double r4 = 0.016 + std::pow(0.0514 * er, 4.524);
    const double r5 = std::pow(fn / 28.843, 12);
    const double r6 = std::min(22.20 * std::pow(u, 1.92), 20.0);
    const double r7 = 1.206 - 0.3144 * std::exp(-r1) * (1.0 - std::exp(-r2));
    const double r8 = 1.0 + 1.275 * (1.0 - std::exp(-0.004625 * r3 * std::pow(er, 1.674) * std::pow(fn / 18.365, 2.745)));
    const double r9 = 5.086 * r4 * r5 / (0.3838 + 0.386 * r4) * std::exp(-r6) / (1.0 + 1.2992 * r5)
                      * std::pow(er - 1.0, 6) / (1.0 + 10.0 * std::pow(er - 1.0, 6));
    const double r10 = 0.00044 * std::pow(er, 2.136) + 0.0184;
    const double r11 = std::pow(fn / 19.47, 6) / (1.0 + 0.0962 * std::pow(fn / 19.47, 6));
    const double r12 = 1.0 / (1.0 + 0.00245 * u * u);
    const double r13 = 0.9408 * std::pow(eeF, r8) - 0.9603;
    const double r14 = (0.9408 - r9) * std::pow(eeStatic, r8) - 0.9603;
    const double r15 = 0.707 * r10 * std::pow(fn / 12.3, 1.097);
    const double r16 = 1.0 + 0.0503 * er * er * r11 * (1.0 - std::exp(-std::pow(u / 15.0, 6)));
    const double r17 = r7 * (1.0 - 1.1241 * r12 / r16 * std::exp(-0.026 * std::pow(fn, 1.15656) - r15));
    if (exponent)
        *exponent = r17;
    return z0Static * std::pow(r13 / r14, r17);
}

// Wheeler's stripline impedance in air with the finite-thickness correction
// (H. A. Wheeler, "Transmission-line properties of a strip line between
// parallel planes", IEEE T-MTT 26(11), 1978; as in B. C. Wadell,
// "Transmission Line Design Handbook", 1991, section 3.5.1).
double wheelerStriplineZ0Air(double w, double b, double t)
{
    double wEffective = w;
    if (t > 0) {
        const double x = t / b;
        const double m = 2.0 / (1.0 + 2.0 / 3.0 * x / (1.0 - x));
        const double dw = x / (Pi * (1.0 - x))
                          * (1.0 - 0.5 * std::log(std::pow(x / (2.0 - x), 2) + std::pow(0.0796 * x / (w / b + 1.1 * x), m)));
        wEffective = w + dw * (b - t);
    }
    const double a = 4.0 / Pi * (b - t) / wEffective;
    return Eta0 / (4.0 * Pi) * std::log(1.0 + a * (2.0 * a + std::sqrt(4.0 * a * a + 6.27)));
}

} // namespace

// E. Hammerstad and O. Jensen, "Accurate models for microstrip computer-aided
// design", IEEE MTT-S Digest 1980, p. 408: the conductor loss of a rough
// surface (rms height rough) relative to a smooth one is
// K = 1 + (2/pi) atan(1.4 (rough/delta)^2), delta = 1/sqrt(pi f mu0 sigma).
// The same factor is used by Qucs (msline) and scikit-rf (MLine, rough).
double roughnessFactor(double rough, double frequency, double sigma)
{
    if (!(rough > 0) || !(sigma > 0) || !(frequency > 0))
        return 1.0;
    const double delta = 1.0 / std::sqrt(Pi * frequency * Mu0 * sigma);
    const double ratio = rough / delta;
    return 1.0 + 2.0 / Pi * std::atan(1.4 * ratio * ratio);
}

// Quasi-static model: E. Hammerstad and O. Jensen, "Accurate models for
// microstrip computer-aided design", IEEE MTT-S Digest 1980, pp. 407-409,
// including their thickness correction (u1 = u + du1, ur = u + dur with
// du1 = t/(pi h) ln(1 + 4e h / (t coth^2 sqrt(6.517 u))) and
// dur = du1 (1 + sech sqrt(er - 1)) / 2; Z0 = Z01(ur)/sqrt(ee(ur)),
// ee = ee(ur) (Z01(u1)/Z01(ur))^2).
// Dispersion: M. Kirschning and R. H. Jansen, Electronics Letters 18(6),
// 1982 (effective permittivity) and R. H. Jansen and M. Kirschning, AEU 37,
// 1983 (power-current impedance), with fn = f h in GHz mm and u = ur, as in
// Qucs and scikit-rf (MLine, model 'hammerstadjensen', disp 'kirschningjansen').
// Conductor loss: alphaC = Rs/(Z0(f) w) exp(-1.2 (Z0(f)/eta0)^0.7) (the
// current-distribution factor of Qucs/scikit-rf) times the roughness factor
// (roughnessFactor). Dielectric loss: quasi-TEM filling-factor formula with ee(f).
std::optional<LineProperties> microstrip(const MicrostripGeometry &g, double frequency, QString *error)
{
    QString problem = checkCommon(g.er, g.tand, g.sigma, frequency);
    if (problem.isEmpty() && (!positive(g.w) || !positive(g.h)))
        problem = QStringLiteral("strip width and substrate height must be positive");
    if (problem.isEmpty() && !nonNegative(g.t))
        problem = QStringLiteral("strip thickness must not be negative");
    if (problem.isEmpty())
        problem = checkRoughness(g.rough);
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Microstrip: %1").arg(problem));
        return std::nullopt;
    }

    const double u = g.w / g.h;
    double du1 = 0;
    if (g.t > 0) {
        const double tn = g.t / g.h;
        du1 = tn / Pi * std::log(1.0 + 4.0 * std::exp(1.0) / tn * std::pow(std::tanh(std::sqrt(6.517 * u)), 2));
    }
    const double dur = du1 * (1.0 + 1.0 / std::cosh(std::sqrt(g.er - 1.0))) / 2.0;
    const double u1 = u + du1, ur = u + dur;
    const double zr = hammerstadZ01(ur), z1 = hammerstadZ01(u1);
    const double er0 = hammerstadEpsilon(ur, g.er);
    const double z0Static = zr / std::sqrt(er0);
    const double eeStatic = er0 * (z1 / zr) * (z1 / zr);

    // Kirschning-Jansen dispersion
    const double fn = frequency * g.h * 1e-6;
    const double ee = kirschningEpsilon(ur, fn, g.er, eeStatic);
    const double z0 = kirschningImpedance(ur, fn, g.er, eeStatic, ee, z0Static);

    const double rs = surfaceResistance(frequency, g.sigma);
    const double alphaC = rs / (z0 * g.w) * std::exp(-1.2 * std::pow(z0 / Eta0, 0.7))
                          * roughnessFactor(g.rough, frequency, g.sigma);
    const double alphaD = quasiTemDielectricLoss(g.er, ee, g.tand, frequency);
    return lineFromLossless(z0, ee, alphaC, alphaD, frequency);
}

// Symmetric coupled microstrip, even and odd modes, u = w/h, g = s/h.
// Quasi-static: M. Kirschning and R. H. Jansen, "Accurate wide-range design
// equations for the frequency-dependent characteristic of parallel coupled
// microstrip lines", IEEE T-MTT 32(1), 1984, pp. 83-90 (with the corrections
// in T-MTT 33(3), 1985, p. 288): even-mode permittivity from Hammerstad-
// Jensen at v = u (20 + g^2)/(10 + g^2) + g exp(-g), odd mode through
// a_o, b_o, c_o, d; Z_e and Z_o from the single-line Z_L(u) and Q1..Q10.
// Strip thickness (only for s > 20 t, as in Qucs): Schneider's width
// increment and Jansen's even/odd split, u_e = W_e/h, u_o = (W_e + dt)/h,
// dt = 2 t h/(s er).
// Dispersion: the same paper, P1..P15 for the permittivities and Q11..Q29
// for the impedances, where P1..P4 and the exponent Q0 = R17 are those of
// the single line (Kirschning-Jansen 1982, Jansen-Kirschning 1983).
// The formula set is the one of Qucs (mscoupled.cpp, model "Kirschning");
// Qucs writes P1 = 0.27488 * (0.6315 + ...) u where the single-line P1 =
// 0.27488 + (0.6315 + ...) u is meant; the latter is used here.
// Losses per mode as for the single line (Hammerstad-Jensen, Qucs):
// alphaC = Rs/(Z_mode w) exp(-1.2 ((Z_e + Z_o)/(2 eta0))^0.7) K_rough,
// alphaD by the filling-factor formula with the mode's permittivity.
// Stated validity: 0.1 <= u <= 10, 0.1 <= g <= 10, 1 <= er <= 18,
// f h <= 25 GHz mm.
std::optional<CoupledLineProperties> coupledMicrostrip(const CoupledMicrostripGeometry &g, double frequency,
                                                       QString *error)
{
    QString problem = checkCommon(g.er, g.tand, g.sigma, frequency);
    if (problem.isEmpty() && (!positive(g.w) || !positive(g.s) || !positive(g.h)))
        problem = QStringLiteral("strip width, gap and substrate height must be positive");
    if (problem.isEmpty() && !nonNegative(g.t))
        problem = QStringLiteral("strip thickness must not be negative");
    if (problem.isEmpty())
        problem = checkRoughness(g.rough);
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Coupled microstrip: %1").arg(problem));
        return std::nullopt;
    }

    const double er = g.er, u = g.w / g.h, gn = g.s / g.h;
    double ue = u, uo = u;
    if (g.t > 0 && g.s > 20.0 * g.t) {
        const double dw = u >= 1.0 / (2.0 * Pi) ? g.t / Pi * (1.0 + std::log(2.0 * g.h / g.t))
                                                : g.t / Pi * (1.0 + std::log(4.0 * Pi * g.w / g.t));
        const double dt = 2.0 * g.t * g.h / (g.s * er);
        const double we = g.w + dw * (1.0 - 0.5 * std::exp(-0.69 * dw / dt));
        ue = we / g.h;
        uo = (we + dt) / g.h;
    }

    // Quasi-static permittivities
    const double v = ue * (20.0 + gn * gn) / (10.0 + gn * gn) + gn * std::exp(-gn);
    const double eeEven = hammerstadEpsilon(v, er);
    const double eeSingleOdd = hammerstadEpsilon(uo, er);
    const double d = 0.593 + 0.694 * std::exp(-0.562 * uo);
    const double bo = 0.747 * er / (0.15 + er);
    const double co = bo - (bo - 0.207) * std::exp(-0.414 * uo);
    const double ao = 0.7287 * (eeSingleOdd - (er + 1.0) / 2.0) * (1.0 - std::exp(-0.179 * uo));
    const double eeOdd = ((er + 1.0) / 2.0 + ao - eeSingleOdd) * std::exp(-co * std::pow(gn, d)) + eeSingleOdd;

    // Quasi-static impedances, from the single line of width w
    const double eeSingle = hammerstadEpsilon(u, er);
    const double zAir = hammerstadZ01(u); // = Z_L sqrt(ee_single)
    const double zSingle = zAir / std::sqrt(eeSingle);
    const double q1 = 0.8695 * std::pow(ue, 0.194);
    const double q2 = 1.0 + 0.7519 * gn + 0.189 * std::pow(gn, 2.31);
    const double q3 = 0.1975 + std::pow(16.6 + std::pow(8.4 / gn, 6), -0.387)
                      + std::log(std::pow(gn, 10) / (1.0 + std::pow(gn / 3.4, 10))) / 241.0;
    const double q4 = q1 / q2 * 2.0 / (std::exp(-gn) * std::pow(ue, q3) + (2.0 - std::exp(-gn)) * std::pow(ue, -q3));
    const double zEven = std::sqrt(eeSingle / eeEven) * zSingle / (1.0 - zAir * q4 / Eta0);
    const double q5 = 1.794 + 1.14 * std::log(1.0 + 0.638 / (gn + 0.517 * std::pow(gn, 2.43)));
    const double q6 = 0.2305 + std::log(std::pow(gn, 10) / (1.0 + std::pow(gn / 5.8, 10))) / 281.3
                      + std::log(1.0 + 0.598 * std::pow(gn, 1.154)) / 5.1;
    const double q7 = (10.0 + 190.0 * gn * gn) / (1.0 + 82.3 * gn * gn * gn);
    const double q8 = std::exp(-6.5 - 0.95 * std::log(gn) - std::pow(gn / 0.15, 5));
    const double q9 = std::log(q7) * (q8 + 1.0 / 16.5);
    const double q10 = (q2 * q4 - q5 * std::exp(std::log(uo) * q6 * std::pow(uo, -q9))) / q2;
    const double zOdd = std::sqrt(eeSingle / eeOdd) * zSingle / (1.0 - zAir * q10 / Eta0);

    // Dispersion of the permittivities
    const double fn = frequency * g.h * 1e-6;
    const double p1 = 0.27488 + (0.6315 + 0.525 / std::pow(1.0 + 0.0157 * fn, 20)) * u - 0.065683 * std::exp(-8.7513 * u);
    const double p2 = 0.33622 * (1.0 - std::exp(-0.03442 * er));
    const double p3 = 0.0363 * std::exp(-4.6 * u) * (1.0 - std::exp(-std::pow(fn / 38.7, 4.97)));
    const double p4 = 1.0 + 2.751 * (1.0 - std::exp(-std::pow(er / 15.916, 8)));
    const double p5 = 0.334 * std::exp(-3.3 * std::pow(er / 15.0, 3)) + 0.746;
    const double p6 = p5 * std::exp(-std::pow(fn / 18.0, 0.368));
    const double p7 = 1.0 + 4.069 * p6 * std::pow(gn, 0.479)
                                * std::exp(-1.347 * std::pow(gn, 0.595) - 0.17 * std::pow(gn, 2.5));
    const double fe = p1 * p2 * std::pow((p3 * p4 + 0.1844 * p7) * fn, 1.5763);
    const double eeEvenF = er - (er - eeEven) / (1.0 + fe);
    const double p8 = 0.7168 * (1.0 + 1.076 / (1.0 + 0.0576 * (er - 1.0)));
    const double p9 = p8 - 0.7913 * (1.0 - std::exp(-std::pow(fn / 20.0, 1.424))) * std::atan(2.481 * std::pow(er / 8.0, 0.946));
    const double p10 = 0.242 * std::pow(er - 1.0, 0.55);
    const double p11 = 0.6366 * (std::exp(-0.3401 * fn) - 1.0) * std::atan(1.263 * std::pow(u / 3.0, 1.629));
    const double p12 = p9 + (1.0 - p9) / (1.0 + 1.183 * std::pow(u, 1.376));
    const double p13 = 1.695 * p10 / (0.414 + 1.605 * p10);
    const double p14 = 0.8928 + 0.1072 * (1.0 - std::exp(-0.42 * std::pow(fn / 20.0, 3.215)));
    const double p15 = std::abs(1.0 - 0.8928 * (1.0 + p11) * std::exp(-p13 * std::pow(gn, 1.092)) * p12 / p14);
    const double fo = p1 * p2 * std::pow((p3 * p4 + 0.1844) * fn * p15, 1.5763);
    const double eeOddF = er - (er - eeOdd) / (1.0 + fo);

    // Dispersion of the even-mode impedance
    const double q11 = 0.893 * (1.0 - 0.3 / (1.0 + 0.7 * (er - 1.0)));
    double x = std::pow(fn / 20.0, 4.91);
    const double q12 = 2.121 * x / (1.0 + q11 * x) * std::exp(-2.87 * gn) * std::pow(gn, 0.902);
    const double q13 = 1.0 + 0.038 * std::pow(er / 8.0, 5.1);
    x = std::pow(er / 15.0, 4);
    const double q14 = 1.0 + 1.203 * x / (1.0 + x);
    const double q15 = 1.887 * std::exp(-1.5 * std::pow(gn, 0.84)) * std::pow(gn, q14)
                       / (1.0 + 0.41 * std::pow(fn / 15.0, 3) * std::pow(u, 2.0 / q13) / (0.125 + std::pow(u, 1.626 / q13)));
    const double q16 = q15 * (1.0 + 9.0 / (1.0 + 0.403 * (er - 1.0) * (er - 1.0)));
    const double q17 = 0.394 * (1.0 - std::exp(-1.47 * std::pow(u / 7.0, 0.672))) * (1.0 - std::exp(-4.25 * std::pow(fn / 20.0, 1.87)));
    const double q18 = 0.61 * (1.0 - std::exp(-2.31 * std::pow(u / 8.0, 1.593))) / (1.0 + 6.544 * std::pow(gn, 4.17));
    const double q19 = 0.21 * std::pow(gn, 4) / (1.0 + 0.18 * std::pow(gn, 4.9)) / (1.0 + 0.1 * u * u)
                       / (1.0 + std::pow(fn / 24.0, 3));
    const double q20 = q19 * (0.09 + 1.0 / (1.0 + 0.1 * std::pow(er - 1.0, 2.7)));
    x = std::pow(u, 2.5);
    const double q21 = std::abs(1.0 - 42.54 * std::pow(gn, 0.133) * std::exp(-0.812 * gn) * x / (1.0 + 0.033 * x));
    double q0 = 0;
    const double eeEvenSingleF = kirschningEpsilon(u, fn, er, eeEven);
    kirschningImpedance(u, fn, er, eeEven, eeEvenSingleF, zEven, &q0);
    const double re = std::pow(fn / 28.843, 12);
    const double qe = 0.016 + std::pow(0.0514 * er * q21, 4.524);
    const double pe = 4.766 * std::exp(-3.228 * std::pow(u, 0.641));
    x = std::pow(er - 1.0, 6);
    const double de = 5.086 * qe * re / (0.3838 + 0.386 * qe) * std::exp(-22.2 * std::pow(u, 1.92)) / (1.0 + 1.2992 * re)
                      * x / (1.0 + 10.0 * x);
    const double ce = 1.0 + 1.275 * (1.0 - std::exp(-0.004625 * pe * std::pow(er, 1.674) * std::pow(fn / 18.365, 2.745)))
                      - q12 + q16 - q17 + q18 + q20;
    const double zEvenF = zEven * std::pow((0.9408 * std::pow(eeEvenSingleF, ce) - 0.9603)
                                               / ((0.9408 - de) * std::pow(eeEven, ce) - 0.9603),
                                           q0);

    // Dispersion of the odd-mode impedance
    const double eeOddSingleF = kirschningEpsilon(u, fn, er, eeOdd);
    const double zOddSingleF = kirschningImpedance(u, fn, er, eeOdd, eeOddSingleF, zOdd);
    const double q29 = 15.16 / (1.0 + 0.196 * (er - 1.0) * (er - 1.0));
    x = (er - 1.0) * (er - 1.0);
    const double q25 = 0.3 * fn * fn / (10.0 + fn * fn) * (1.0 + 2.333 * x / (5.0 + x));
    x = std::pow((er - 1.0) / 13.0, 12);
    const double q26 = 30.0 - 22.2 * x / (1.0 + 3.0 * x) - q29;
    x = std::pow(er - 1.0, 1.5);
    const double q27 = 0.4 * std::pow(gn, 0.84) * (1.0 + 2.5 * x / (5.0 + x));
    x = std::pow(er - 1.0, 3);
    const double q28 = 0.149 * x / (94.5 + 0.038 * x);
    const double q22 = 0.925 * std::pow(fn / q26, 1.536) / (1.0 + 0.3 * std::pow(fn / 30.0, 1.536));
    const double q23 = 1.0 + 0.005 * fn * q27 / (1.0 + 0.812 * std::pow(fn / 15.0, 1.9)) / (1.0 + 0.025 * u * u);
    x = std::pow(u, 0.894);
    const double q24 = 2.506 * q28 * x / (3.575 + x) * std::pow((1.0 + 1.3 * u) * fn / 99.25, 4.29);
    const double zOddF = zOddSingleF + (zOdd * std::pow(eeOddF / eeOdd, q22) - zOddSingleF * q23)
                                           / (1.0 + q24 + std::pow(0.46 * gn, 2.2) * q25);

    const double rs = surfaceResistance(frequency, g.sigma);
    const double currentFactor = std::exp(-1.2 * std::pow((zEvenF + zOddF) / 2.0 / Eta0, 0.7))
                                 * roughnessFactor(g.rough, frequency, g.sigma);
    CoupledLineProperties result;
    result.even = lineFromLossless(zEvenF, eeEvenF, rs / (zEvenF * g.w) * currentFactor,
                                   quasiTemDielectricLoss(er, eeEvenF, g.tand, frequency), frequency);
    result.odd = lineFromLossless(zOddF, eeOddF, rs / (zOddF * g.w) * currentFactor,
                                  quasiTemDielectricLoss(er, eeOddF, g.tand, frequency), frequency);
    if (!(std::isfinite(zEvenF) && std::isfinite(zOddF) && zEvenF > 0 && zOddF > 0)) {
        setError(error, QStringLiteral("Coupled microstrip: the model is outside its range for these dimensions"));
        return std::nullopt;
    }
    return result;
}

// Symmetric stripline (strip centred between ground planes b apart), TEM:
// ee = er. Impedance: Wheeler 1978 with thickness correction (see
// wheelerStriplineZ0Air). Conductor loss by Wheeler's incremental
// inductance rule, alphaC = Rs/(2 Z0 eta) dZ0/dn, where dZ0/dn is the change
// of Z0 when every conductor surface recedes by dn (strip width and
// thickness shrink by 2 dn, ground spacing grows by 2 dn), evaluated
// numerically on the same impedance formula (D. M. Pozar, "Microwave
// Engineering", section 3.8), times the roughness factor (roughnessFactor).
// Dielectric loss: pi f sqrt(er) tand / c.
std::optional<LineProperties> stripline(const StriplineGeometry &g, double frequency, QString *error)
{
    QString problem = checkCommon(g.er, g.tand, g.sigma, frequency);
    if (problem.isEmpty() && (!positive(g.w) || !positive(g.b)))
        problem = QStringLiteral("strip width and ground plane spacing must be positive");
    if (problem.isEmpty() && (!nonNegative(g.t) || g.t >= g.b))
        problem = QStringLiteral("strip thickness must be at least 0 and less than the ground plane spacing");
    if (problem.isEmpty())
        problem = checkRoughness(g.rough);
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Stripline: %1").arg(problem));
        return std::nullopt;
    }
    const double z0Air = wheelerStriplineZ0Air(g.w, g.b, g.t);
    const double z0 = z0Air / std::sqrt(g.er);

    double alphaC = 0;
    const double rs = surfaceResistance(frequency, g.sigma);
    if (rs > 0) {
        double dn = 1e-4 * std::min(g.w, g.b - g.t);
        if (g.t > 0)
            dn = std::min(dn, 1e-4 * g.t);
        auto shifted = [&](double d) {
            return wheelerStriplineZ0Air(g.w - 2.0 * d, g.b + 2.0 * d, g.t > 0 ? g.t - 2.0 * d : 0.0);
        };
        const double derivative = (shifted(dn) - shifted(-dn)) / (2.0 * dn);
        alphaC = rs * std::sqrt(g.er) * derivative / (2.0 * z0Air * Eta0) * roughnessFactor(g.rough, frequency, g.sigma);
    }
    const double alphaD = Pi * frequency * std::sqrt(g.er) * g.tand / SpeedOfLight;
    return lineFromLossless(z0, g.er, alphaC, alphaD, frequency);
}

namespace {

// I1(z)/I0(z) for Re z > 0 by the continued fraction
// I1/I0 = 1/(2/z + 1/(4/z + 1/(6/z + ...))), evaluated backwards; for very
// large |z| the asymptotic expansion I0/I1 = 1 + 1/(2z) + 3/(8z^2) + 3/(8z^3) is used.
Complex besselI1OverI0(Complex z)
{
    if (std::abs(z) > 2000.0)
        return 1.0 / (1.0 + 1.0 / (2.0 * z) + 3.0 / (8.0 * z * z) + 3.0 / (8.0 * z * z * z));
    const int terms = static_cast<int>(std::abs(z)) + 60;
    Complex t = 0;
    for (int n = terms; n >= 1; --n)
        t = 1.0 / (2.0 * n / z + t);
    return t;
}

// K0(z)/K1(z) from the asymptotic expansions of K0 and K1 (accurate for |z|
// above about 10, i.e. a conductor wall many skin depths from the axis).
Complex besselK0OverK1(Complex z)
{
    Complex k0 = 1, k1 = 1, term0 = 1, term1 = 1;
    for (int k = 1; k <= 8; ++k) {
        const double odd = (2.0 * k - 1.0) * (2.0 * k - 1.0);
        term0 *= (0.0 - odd) / (k * 8.0 * z);
        term1 *= (4.0 - odd) / (k * 8.0 * z);
        k0 += term0;
        k1 += term1;
    }
    return k0 / k1;
}

} // namespace

// Coaxial line, TEM. Per unit length (a, b inner and outer radius):
//   L = mu0/(2 pi) ln(b/a),  C = 2 pi eps0 er / ln(b/a),  G = w C tand,
//   conductor impedances with k = sqrt(j w mu0 sigma) and Zs = k/sigma
//   (S. A. Schelkunoff, Bell Syst. Tech. J. 13, 1934, eqs. 65 and 74 with a
//   very thick outer wall):
//     inner rod  Zi = Zs/(2 pi a) I0(ka)/I1(ka),
//     outer tube Zo = Zs/(2 pi b) K0(kb)/K1(kb),
//   which tend to (1 + j) Rs/(2 pi) (1/a + 1/b) when the radii are many skin
//   depths; Z' = Zi + Zo + j w L and Y' = G + j w C, zc = sqrt(Z'/Y'),
//   gamma = sqrt(Z' Y'). The lossless Z0 is
//   eta0/(2 pi sqrt(er)) ln(dout/din) ~ 60/sqrt(er) ln(dout/din).
// alphaConductor = Re(Zi + Zo)/(2 Z0) and alphaDielectric = G Z0/2 =
// pi f sqrt(er) tand/c are the usual low-loss parts of Re(gamma). This is
// the model of scikit-rf's Coaxial (default 'schelkunoff', infinite shield).
std::optional<LineProperties> coax(const CoaxGeometry &g, double frequency, QString *error)
{
    QString problem = checkCommon(g.er, g.tand, g.sigma, frequency);
    if (problem.isEmpty() && (!positive(g.din) || !positive(g.dout) || g.dout <= g.din))
        problem = QStringLiteral("diameters must be positive with the outer larger than the inner");
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Coax: %1").arg(problem));
        return std::nullopt;
    }
    const double a = g.din / 2.0, b = g.dout / 2.0;
    const double logRatio = std::log(b / a);
    const double l = Mu0 / (2.0 * Pi) * logRatio;
    const double c = 2.0 * Pi * Epsilon0 * g.er / logRatio;
    const double z0 = std::sqrt(l / c);
    const double w = 2.0 * Pi * frequency;
    const double gShunt = w * c * g.tand;
    Complex conductors = 0;
    if (g.sigma > 0 && w > 0) {
        const Complex k = std::sqrt(Complex(0, w * Mu0 * g.sigma));
        const Complex zs = k / g.sigma;
        conductors = zs / (2.0 * Pi * a) / besselI1OverI0(k * a) + zs / (2.0 * Pi * b) * besselK0OverK1(k * b);
    }

    LineProperties p;
    p.epsilonEffective = g.er;
    p.alphaConductor = conductors.real() / (2.0 * z0);
    p.alphaDielectric = gShunt * z0 / 2.0;
    if (w <= 0) {
        p.zc = z0;
        p.gamma = 0;
        return p;
    }
    const Complex series = conductors + Complex(0, w * l);
    const Complex shunt(gShunt, w * c);
    p.zc = std::sqrt(series / shunt);
    p.gamma = std::sqrt(series * shunt);
    return p;
}

// Coplanar waveguide: centre strip w, gaps s, substrate height h.
// Quasi-static, conformal mapping (k1 = w/(w + 2s)):
//  - air below the substrate (G. Ghione and C. Naldi, Electronics Letters
//    20(4), 1984): k2 = sinh(pi w/4h)/sinh(pi (w + 2s)/4h),
//    ee = 1 + (er - 1)/2 [K(k2)/K'(k2)] / [K(k1)/K'(k1)], Z0 = eta0/(4 sqrt(ee)) K'(k1)/K(k1);
//  - conductor-backed (Ghione and Naldi, Electronics Letters 19(18), 1983):
//    k3 = tanh(pi w/4h)/tanh(pi (w + 2s)/4h), q1 = K(k1)/K'(k1), q3 = K(k3)/K'(k3),
//    ee = (q1 + er q3)/(q1 + q3), Z0 = eta0/(2 sqrt(ee)) / (q1 + q3).
// Thickness (K. C. Gupta et al., "Microstrip Lines and Slotlines", 2nd ed.,
// 1996, eqs. 7.98-7.100): d = 1.25 t/pi (1 + ln(4 pi w/t)),
// ke = k1 + (1 - k1^2) d/(2s) replaces k1 in Z0, and
// ee_t = ee - 0.7 (ee - 1) t/s / (q1 + 0.7 t/s).
// Dispersion: M. Y. Frankel et al., IEEE T-MTT 39(6), 1991,
// sqrt(ee(f)) = sqrt(ee) + (sqrt(er) - sqrt(ee)) / (1 + G (f/fTE)^-1.8),
// fTE = c/(4 h sqrt(er - 1)), Z0(f) = Z0 sqrt(ee)/sqrt(ee(f)).
// Conductor loss (needs t > 0; G. Ghione, IEEE T-MTT 41(9), 1993, after
// Owyang and Wu): alphaC = Rs sqrt(ee(f)) / (4 eta0 K(k1) K'(k1) (1 - k1^2))
// [(pi + ln(n a))/a + (pi + ln(n b))/b], a = w/2, b = a + s,
// n = 8 pi (1 - k1)/(t (1 + k1)). This is the model set of scikit-rf's CPW
// (whose 2.2 release evaluates K(k1) K'(k1) in the loss with scipy's
// parameter-based ellipk applied to the modulus, giving 20-25 % less loss).
// The conductor loss is multiplied by the roughness factor (roughnessFactor).
std::optional<LineProperties> cpw(const CpwGeometry &g, double frequency, QString *error)
{
    QString problem = checkCommon(g.er, g.tand, g.sigma, frequency);
    if (problem.isEmpty() && (!positive(g.w) || !positive(g.s) || !positive(g.h)))
        problem = QStringLiteral("strip width, gap and substrate height must be positive");
    if (problem.isEmpty() && !nonNegative(g.t))
        problem = QStringLiteral("strip thickness must not be negative");
    if (problem.isEmpty())
        problem = checkRoughness(g.rough);
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("CPW: %1").arg(problem));
        return std::nullopt;
    }

    const double k1 = g.w / (g.w + 2.0 * g.s);
    const double q1 = ellipticRatio(k1);
    const double arg1 = Pi * g.w / (4.0 * g.h), arg2 = Pi * (g.w + 2.0 * g.s) / (4.0 * g.h);
    double q3 = 0, ee = 1, zr = 0;
    if (g.backed) {
        q3 = ellipticRatio(std::tanh(arg1) / std::tanh(arg2));
        ee = 1.0 + q3 / (q1 + q3) * (g.er - 1.0);
        zr = Eta0 / 2.0 / (q1 + q3);
    } else {
        // sinh ratio written to avoid overflow for thin substrates
        const double k2 = (std::exp(arg1 - arg2) - std::exp(-arg1 - arg2)) / (1.0 - std::exp(-2.0 * arg2));
        ee = 1.0 + (g.er - 1.0) / 2.0 * ellipticRatio(k2) / q1;
        zr = Eta0 / 4.0 / q1;
    }
    if (g.t > 0) {
        const double d = 1.25 * g.t / Pi * (1.0 + std::log(4.0 * Pi * g.w / g.t));
        const double ke = k1 + (1.0 - k1 * k1) * d / (2.0 * g.s);
        if (!(ke > 0 && ke < 1)) {
            setError(error, QStringLiteral("CPW: strip thickness too large for the gap"));
            return std::nullopt;
        }
        const double qe = ellipticRatio(ke);
        zr = g.backed ? Eta0 / 2.0 / (qe + q3) : Eta0 / 4.0 / qe;
        ee -= 0.7 * (ee - 1.0) * g.t / g.s / (q1 + 0.7 * g.t / g.s);
    }
    const double z0Static = zr / std::sqrt(ee);

    double sqrtEe = std::sqrt(ee);
    if (frequency > 0 && g.er > 1) {
        const double fte = SpeedOfLight / 4.0 / (g.h * std::sqrt(g.er - 1.0));
        const double p = std::log(g.w / g.h);
        const double u = 0.54 - (0.64 - 0.015 * p) * p;
        const double v = 0.43 - (0.86 - 0.54 * p) * p;
        const double factor = std::exp(u * std::log(g.w / g.s) + v);
        sqrtEe += (std::sqrt(g.er) - sqrtEe) / (1.0 + factor * std::pow(frequency / fte, -1.8));
    }
    const double eeF = sqrtEe * sqrtEe;
    const double z0 = z0Static * std::sqrt(ee) / sqrtEe;

    double alphaC = 0;
    const double rs = surfaceResistance(frequency, g.sigma);
    if (g.t > 0 && rs > 0) {
        const double n = (1.0 - k1) * 8.0 * Pi / (g.t * (1.0 + k1));
        const double a = g.w / 2.0, b = a + g.s;
        const double sum = (Pi + std::log(n * a)) / a + (Pi + std::log(n * b)) / b;
        alphaC = rs * sqrtEe * sum / (4.0 * Eta0 * ellipticK(k1) * ellipticK(complementaryModulus(k1)) * (1.0 - k1 * k1))
                 * roughnessFactor(g.rough, frequency, g.sigma);
    }
    const double alphaD = quasiTemDielectricLoss(g.er, eeF, g.tand, frequency);
    return lineFromLossless(z0, eeF, alphaC, alphaD, frequency);
}

CMatrix lineAbcd(Complex zc, Complex gamma, double length)
{
    CMatrix abcd(2, 2);
    const Complex gl = gamma * length;
    abcd << std::cosh(gl), zc * std::sinh(gl), std::sinh(gl) / zc, std::cosh(gl);
    return abcd;
}

} // namespace lvd::rf

#include "matching/MatchingSynthesis.h"

#include "core/Expression.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lvd::rf {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kVerifyTolerance = 1e-9;

QString tr(const char *text)
{
    return QCoreApplication::translate("lvd::rf::Matching", text);
}

double degrees(double radians) { return radians * 180.0 / kPi; }
double radians(double deg) { return deg * kPi / 180.0; }

// An angle in [0, pi).
double wrapHalfTurn(double angle)
{
    double a = std::fmod(angle, kPi);
    if (a < 0)
        a += kPi;
    if (a >= kPi)
        a -= kPi;
    return a;
}

bool isFinite(MatchComplex z) { return std::isfinite(z.real()) && std::isfinite(z.imag()); }

// Scale used to decide that a reactance or susceptance is negligible.
struct Scale {
    double ohm = 1;
    double siemens = 1;
};

Scale scaleOf(MatchComplex zs, MatchComplex zl)
{
    Scale s;
    s.ohm = std::abs(zs) + std::abs(zl);
    s.siemens = 1.0 / std::abs(zs) + 1.0 / std::abs(zl);
    return s;
}

// A series reactance X or a shunt susceptance B as an element; nullopt when negligible.
std::optional<MatchElement> reactive(MatchPlacement placement, double value, double omega, const Scale &scale)
{
    const double negligible = 1e-13 * (placement == MatchPlacement::Series ? scale.ohm : scale.siemens);
    if (!std::isfinite(value) || std::abs(value) <= negligible)
        return std::nullopt;
    MatchElement e;
    e.placement = placement;
    if (placement == MatchPlacement::Series) {
        if (value > 0) {
            e.kind = MatchElementKind::Inductor;
            e.value = value / omega;
        } else {
            e.kind = MatchElementKind::Capacitor;
            e.value = -1.0 / (omega * value);
        }
    } else {
        if (value > 0) {
            e.kind = MatchElementKind::Capacitor;
            e.value = value / omega;
        } else {
            e.kind = MatchElementKind::Inductor;
            e.value = -1.0 / (omega * value);
        }
    }
    e.idealValue = e.value;
    return e;
}

MatchElement line(double z0, double thetaRad)
{
    MatchElement e;
    e.placement = MatchPlacement::Series;
    e.kind = MatchElementKind::Line;
    e.z0 = z0;
    e.lengthDeg = degrees(thetaRad);
    return e;
}

// A stub of the given type with susceptance B at f0; nullopt for B = 0 with an open stub.
std::optional<MatchElement> stub(bool open, double susceptance, double z0, const Scale &scale)
{
    MatchElement e;
    e.placement = MatchPlacement::Shunt;
    e.kind = open ? MatchElementKind::OpenStub : MatchElementKind::ShortStub;
    e.z0 = z0;
    const double x = susceptance * z0;
    if (open) {
        if (std::abs(susceptance) <= 1e-13 * scale.siemens)
            return std::nullopt;
        e.lengthDeg = degrees(wrapHalfTurn(std::atan(x)));
    } else {
        e.lengthDeg = degrees(kPi / 2 + std::atan(x)); // in (0, 180)
    }
    return e;
}

// A real square root of a value that may be slightly negative from rounding;
// nullopt when clearly negative.
std::optional<double> safeSqrt(double value, double magnitude)
{
    if (value >= 0)
        return std::sqrt(value);
    if (value > -1e-12 * std::max(magnitude, 1e-300))
        return 0.0;
    return std::nullopt;
}

using Ladder = std::vector<MatchElement>;

void append(Ladder &ladder, const std::optional<MatchElement> &element)
{
    if (element)
        ladder.push_back(*element);
}

// Type A L-section: [series X] - [shunt B] - load. Transforms zl into zt.
// Returns {X, B} pairs (one or two). The shunt susceptance is written for
// the load admittance as given.
std::vector<std::pair<double, double>> sectionShuntAtLoad(MatchComplex zt, MatchComplex zl)
{
    std::vector<std::pair<double, double>> out;
    const MatchComplex yl = 1.0 / zl;
    const double gl = yl.real(), bl = yl.imag();
    const double rt = zt.real(), xt = zt.imag();
    const auto root = safeSqrt(gl / rt - gl * gl, gl * gl);
    if (!root)
        return out;
    for (const double sign : {1.0, -1.0}) {
        const double bp = sign * *root;
        const MatchComplex zn = 1.0 / MatchComplex(gl, bp);
        out.emplace_back(xt - zn.imag(), bp - bl);
        if (*root == 0)
            break;
    }
    return out;
}

// Type B L-section: [shunt B] - [series X] - load. Returns {B, X} pairs.
std::vector<std::pair<double, double>> sectionSeriesAtLoad(MatchComplex zt, MatchComplex zl)
{
    std::vector<std::pair<double, double>> out;
    const MatchComplex yt = 1.0 / zt;
    const double gt = yt.real(), bt = yt.imag();
    const double rl = zl.real(), xl = zl.imag();
    const auto root = safeSqrt(rl / gt - rl * rl, rl * rl);
    if (!root)
        return out;
    for (const double sign : {1.0, -1.0}) {
        const double xp = sign * *root;
        const MatchComplex yn = 1.0 / MatchComplex(rl, xp);
        out.emplace_back(bt - yn.imag(), xp - xl);
        if (*root == 0)
            break;
    }
    return out;
}

QString kindLetter(const MatchElement &e)
{
    switch (e.kind) {
    case MatchElementKind::Capacitor: return QStringLiteral("C");
    case MatchElementKind::Inductor: return QStringLiteral("L");
    case MatchElementKind::Line: return tr("line");
    case MatchElementKind::OpenStub: return tr("open stub");
    case MatchElementKind::ShortStub: return tr("short stub");
    }
    return {};
}

QString summary(const Ladder &ladder)
{
    QStringList parts;
    for (const MatchElement &e : ladder)
        parts << (e.placement == MatchPlacement::Series ? tr("series %1") : tr("shunt %1")).arg(kindLetter(e));
    return parts.join(QStringLiteral(", "));
}

bool sameLadder(const Ladder &a, const Ladder &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const MatchElement &x = a[i], &y = b[i];
        if (x.placement != y.placement || x.kind != y.kind)
            return false;
        const auto close = [](double p, double q) { return std::abs(p - q) <= 1e-9 * std::max(std::abs(p), std::abs(q)); };
        if (x.isLumped() ? !close(x.value, y.value) : (!close(x.z0, y.z0) || !close(x.lengthDeg, y.lengthDeg)))
            return false;
    }
    return true;
}

struct Builder {
    const MatchRequest &request;
    MatchResult &result;
    double omega;
    Scale scale;

    void add(Ladder ladder, const QString &description)
    {
        for (const MatchSolution &existing : result.solutions) {
            if (sameLadder(existing.elements, ladder))
                return;
        }
        MatchSolution s;
        s.topology = request.topology;
        s.description = description;
        s.elements = std::move(ladder);
        s.zs = request.zs;
        s.zl = request.zl;
        s.frequency = request.frequency;
        const MatchComplex target = std::conj(request.zs);
        const MatchComplex zin = inputImpedance(s, request.frequency);
        const double error = std::abs(zin - target) / std::abs(target);
        if (!(error <= kVerifyTolerance)) {
            result.warnings << tr("A candidate network failed verification (relative error %1) and was dropped.")
                                   .arg(error, 0, 'g', 3);
            return;
        }
        result.solutions.push_back(std::move(s));
    }
};

QString formatOhm(double value) { return formatEngineering(value, QStringLiteral("Ω"), 4); }
QString formatDeg(double value) { return formatEngineering(value, QStringLiteral("deg"), 4).replace(QStringLiteral(" deg"), QStringLiteral("°")); }

void synthesizeL(Builder &b)
{
    const MatchComplex zt = std::conj(b.request.zs);
    const MatchComplex zl = b.request.zl;
    if (std::abs(zt - zl) <= 1e-12 * b.scale.ohm) {
        b.add({}, tr("Direct connection: the load is already matched"));
        return;
    }
    for (const auto &[x, sh] : sectionShuntAtLoad(zt, zl)) {
        Ladder l;
        append(l, reactive(MatchPlacement::Series, x, b.omega, b.scale));
        append(l, reactive(MatchPlacement::Shunt, sh, b.omega, b.scale));
        b.add(l, l.size() == 2 ? tr("L-network, shunt at load: %1").arg(summary(l)) : tr("Single element: %1").arg(summary(l)));
    }
    for (const auto &[sh, x] : sectionSeriesAtLoad(zt, zl)) {
        Ladder l;
        append(l, reactive(MatchPlacement::Shunt, sh, b.omega, b.scale));
        append(l, reactive(MatchPlacement::Series, x, b.omega, b.scale));
        b.add(l, l.size() == 2 ? tr("L-network, series at load: %1").arg(summary(l)) : tr("Single element: %1").arg(summary(l)));
    }
    if (b.result.solutions.empty())
        b.result.error = tr("No L-network solution exists for these impedances.");
}

void synthesizePi(Builder &b)
{
    const MatchComplex zt = std::conj(b.request.zs);
    const MatchComplex zl = b.request.zl;
    const double q = b.request.q;
    const double qmin = minimumPiQ(b.request.zs, zl);
    if (!(q > 0) || q < qmin * (1 - 1e-12)) {
        b.result.error = tr("The loaded Q %1 is below the minimum %2 for a Pi-network (the L-network Q).")
                             .arg(q, 0, 'g', 4)
                             .arg(qmin, 0, 'g', 4);
        return;
    }
    const double gl = (1.0 / zl).real();
    const MatchComplex yt = 1.0 / zt;
    const double gt = yt.real(), bt = yt.imag();
    const double rpl = 1 / gl, rps = 1 / gt;
    const double rv = std::max(rpl, rps) / (1 + q * q);
    for (const auto &[x2, b2] : sectionShuntAtLoad(MatchComplex(rv, 0), zl)) {
        const auto root = safeSqrt(rv / gt - rv * rv, rv * rv);
        if (!root)
            continue;
        for (const double sign : {1.0, -1.0}) {
            const double x1 = sign * *root;
            const double b1 = bt - (1.0 / MatchComplex(rv, x1)).imag();
            Ladder l;
            append(l, reactive(MatchPlacement::Shunt, b1, b.omega, b.scale));
            append(l, reactive(MatchPlacement::Series, x1 + x2, b.omega, b.scale));
            append(l, reactive(MatchPlacement::Shunt, b2, b.omega, b.scale));
            b.add(l, tr("Pi-network (Q %1): %2").arg(q, 0, 'g', 4).arg(summary(l)));
            if (*root == 0)
                break;
        }
    }
    if (b.result.solutions.empty())
        b.result.error = tr("No Pi-network solution exists for these impedances.");
}

void synthesizeT(Builder &b)
{
    const MatchComplex zt = std::conj(b.request.zs);
    const MatchComplex zl = b.request.zl;
    const double q = b.request.q;
    const double qmin = minimumTQ(b.request.zs, zl);
    if (!(q > 0) || q < qmin * (1 - 1e-12)) {
        b.result.error = tr("The loaded Q %1 is below the minimum %2 for a T-network (the L-network Q).")
                             .arg(q, 0, 'g', 4)
                             .arg(qmin, 0, 'g', 4);
        return;
    }
    const double rv = std::min(zt.real(), zl.real()) * (1 + q * q);
    for (const auto &[bb, x2] : sectionSeriesAtLoad(MatchComplex(rv, 0), zl)) {
        for (const auto &[x1, ba] : sectionShuntAtLoad(zt, MatchComplex(rv, 0))) {
            Ladder l;
            append(l, reactive(MatchPlacement::Series, x1, b.omega, b.scale));
            append(l, reactive(MatchPlacement::Shunt, ba + bb, b.omega, b.scale));
            append(l, reactive(MatchPlacement::Series, x2, b.omega, b.scale));
            b.add(l, tr("T-network (Q %1): %2").arg(q, 0, 'g', 4).arg(summary(l)));
        }
    }
    if (b.result.solutions.empty())
        b.result.error = tr("No T-network solution exists for these impedances.");
}

bool isReal(MatchComplex z) { return std::abs(z.imag()) <= 1e-12 * std::abs(z); }

void synthesizeMultiSection(Builder &b)
{
    const MatchComplex zs = b.request.zs, zl = b.request.zl;
    if (!isReal(zs) || !isReal(zl)) {
        b.result.error = tr("A multi-section ladder needs real source and load impedances.");
        return;
    }
    const int n = b.request.sections;
    if (n < 1 || n > 20) {
        b.result.error = tr("The number of sections must be between 1 and 20.");
        return;
    }
    const double rs = zs.real(), rl = zl.real();
    if (std::abs(rs - rl) <= 1e-12 * (rs + rl)) {
        b.add({}, tr("Direct connection: the load is already matched"));
        return;
    }
    for (const bool lowpass : {true, false}) {
        Ladder ladder;
        bool ok = true;
        for (int k = 0; k < n && ok; ++k) {
            const double ra = rs * std::pow(rl / rs, double(k) / n);
            const double rb = k + 1 == n ? rl : rs * std::pow(rl / rs, double(k + 1) / n);
            const double sign = lowpass ? 1.0 : -1.0;
            if (rb > ra) { // shunt on the high (load) side
                const double g = 1 / rb;
                const auto root = safeSqrt(g / ra - g * g, g * g);
                if (!root) {
                    ok = false;
                    break;
                }
                const double bp = sign * *root;
                const double x = -(1.0 / MatchComplex(g, bp)).imag();
                append(ladder, reactive(MatchPlacement::Series, x, b.omega, b.scale));
                append(ladder, reactive(MatchPlacement::Shunt, bp, b.omega, b.scale));
            } else {
                const double gt = 1 / ra;
                const auto root = safeSqrt(rb / gt - rb * rb, rb * rb);
                if (!root) {
                    ok = false;
                    break;
                }
                const double xp = sign * *root;
                const double bsh = -(1.0 / MatchComplex(rb, xp)).imag();
                append(ladder, reactive(MatchPlacement::Shunt, bsh, b.omega, b.scale));
                append(ladder, reactive(MatchPlacement::Series, xp, b.omega, b.scale));
            }
        }
        if (ok)
            b.add(ladder, (lowpass ? tr("%1-section low-pass L-ladder, step ratio %2")
                                   : tr("%1-section high-pass L-ladder, step ratio %2"))
                              .arg(n)
                              .arg(std::pow(std::max(rs, rl) / std::min(rs, rl), 1.0 / n), 0, 'g', 4));
    }
}

double lineZ0(const MatchRequest &request)
{
    return request.lineZ0 > 0 ? request.lineZ0 : request.zs.real();
}

void synthesizeQuarterWave(Builder &b)
{
    const MatchComplex zt = std::conj(b.request.zs);
    const MatchComplex zl = b.request.zl;
    const double z0 = lineZ0(b.request);

    // Source side: a line from a real impedance point to Zs* (shortest).
    double ra = zt.real();
    std::optional<MatchElement> sourceLine;
    QString sourceText;
    if (!isReal(zt)) {
        const MatchComplex g = (zt - z0) / (zt + z0);
        const double rho = std::abs(g), phi = std::arg(g);
        const double thetaMax = wrapHalfTurn(-phi / 2);
        const double thetaMin = wrapHalfTurn((kPi - phi) / 2);
        if (thetaMax <= thetaMin) {
            ra = z0 * (1 + rho) / (1 - rho);
            sourceLine = line(z0, thetaMax);
        } else {
            ra = z0 * (1 - rho) / (1 + rho);
            sourceLine = line(z0, thetaMin);
        }
        sourceText = tr("; source line %1 from %2").arg(formatDeg(sourceLine->lengthDeg), formatOhm(ra));
    }

    struct LoadSide {
        double r;
        std::optional<MatchElement> element;
        QString text;
    };
    std::vector<LoadSide> options;
    if (isReal(zl)) {
        options.push_back({zl.real(), std::nullopt, QString()});
    } else {
        const MatchComplex g = (zl - z0) / (zl + z0);
        const double rho = std::abs(g), phi = std::arg(g);
        const double rmax = z0 * (1 + rho) / (1 - rho), rmin = z0 * (1 - rho) / (1 + rho);
        const MatchElement toMax = line(z0, wrapHalfTurn(phi / 2));
        const MatchElement toMin = line(z0, wrapHalfTurn((phi - kPi) / 2));
        options.push_back({rmax, toMax, tr("; load line %1 to Rmax %2").arg(formatDeg(toMax.lengthDeg), formatOhm(rmax))});
        options.push_back({rmin, toMin, tr("; load line %1 to Rmin %2").arg(formatDeg(toMin.lengthDeg), formatOhm(rmin))});
        const MatchComplex yl = 1.0 / zl;
        for (const bool open : {true, false}) {
            const auto s = stub(open, -yl.imag(), z0, b.scale);
            if (s)
                options.push_back({1 / yl.real(), s,
                                   (open ? tr("; open stub %1 cancels the load susceptance")
                                         : tr("; short stub %1 cancels the load susceptance"))
                                       .arg(formatDeg(s->lengthDeg))});
        }
    }
    for (const LoadSide &o : options) {
        const double zq = std::sqrt(ra * o.r);
        Ladder l;
        append(l, sourceLine);
        l.push_back(line(zq, kPi / 2));
        append(l, o.element);
        b.add(l, tr("Quarter-wave transformer %1%2%3").arg(formatOhm(zq), o.text, sourceText));
    }
}

void synthesizeSingleStub(Builder &b)
{
    const MatchComplex zt = std::conj(b.request.zs);
    const MatchComplex zl = b.request.zl;
    const double z0 = lineZ0(b.request);
    const MatchComplex yt = 1.0 / zt;
    const MatchComplex y = z0 / zl; // normalised load admittance
    const double a = y.real(), bb = y.imag(), g = yt.real() * z0;
    // Re y(t) = g with t = tan(theta): A t^2 + B t + C = 0
    const double qa = a - g * std::norm(y), qb = 2 * g * bb, qc = a - g;
    std::vector<double> thetas;
    const double tiny = 1e-12 * (std::abs(a) + std::abs(g) * std::norm(y) + std::abs(qb) + 1e-300);
    if (std::abs(qa) <= tiny) {
        thetas.push_back(kPi / 2);
        if (std::abs(qb) > tiny)
            thetas.push_back(wrapHalfTurn(std::atan(-qc / qb)));
    } else {
        const double disc = qb * qb - 4 * qa * qc;
        const auto root = safeSqrt(disc, qb * qb + std::abs(4 * qa * qc));
        if (!root) {
            b.result.error = tr("No single-stub solution exists with a %1 line; try another line impedance.").arg(formatOhm(z0));
            return;
        }
        for (const double sign : {1.0, -1.0}) {
            // Numerically stable roots.
            const double t = sign > 0 ? (-qb + *root) / (2 * qa) : (-qb - *root) / (2 * qa);
            thetas.push_back(wrapHalfTurn(std::atan(t)));
            if (*root == 0)
                break;
        }
    }
    for (const double theta : thetas) {
        std::optional<MatchElement> ln;
        if (theta > 1e-12)
            ln = line(z0, theta);
        const MatchComplex zline = ln ? transformImpedance(*ln, zl, b.request.frequency, b.request.frequency) : zl;
        const double bstub = yt.imag() - (1.0 / zline).imag();
        for (const bool open : {true, false}) {
            Ladder l;
            append(l, stub(open, bstub, z0, b.scale));
            append(l, ln);
            QString text = open ? tr("Single open stub") : tr("Single short stub");
            if (!l.empty() && l.front().placement == MatchPlacement::Shunt)
                text += tr(" %1").arg(formatDeg(l.front().lengthDeg));
            text += tr(", %1 from the load").arg(formatDeg(ln ? ln->lengthDeg : 0.0));
            b.add(l, text);
        }
    }
}

void synthesizeDoubleStub(Builder &b)
{
    const MatchComplex zt = std::conj(b.request.zs);
    const MatchComplex zl = b.request.zl;
    const double z0 = lineZ0(b.request);
    const double d = b.request.stubSpacingDeg;
    if (!(d > 0 && d < 180) || std::abs(d - 180) < 1e-9) {
        b.result.error = tr("The stub spacing must be between 0 and 180 degrees, exclusive.");
        return;
    }
    const double s = std::sin(radians(d)), c = std::cos(radians(d));
    const MatchComplex yt = 1.0 / zt;
    const MatchComplex yl = 1.0 / zl;
    const double a = yl.real() * z0, bl = yl.imag() * z0, g = yt.real() * z0;
    // Re of the normalised admittance after the line equals g:
    // P b^2 + Q b + R = 0 for the normalised susceptance b after stub 1.
    const double p = -g * s * s, q = 2 * g * s * c, r = a - g * c * c - g * a * a * s * s;
    const double disc = q * q - 4 * p * r;
    const auto root = safeSqrt(disc, q * q + std::abs(4 * p * r));
    if (!root) {
        b.result.error = tr("The load is in the forbidden region for a %1 stub spacing with a %2 line; change the spacing.")
                             .arg(formatDeg(d), formatOhm(z0));
        return;
    }
    const MatchElement spacing = line(z0, radians(d));
    for (const double sign : {1.0, -1.0}) {
        const double bn = (-q + sign * *root) / (2 * p);
        const double b1 = (bn - bl) / z0;
        const MatchComplex y1 = MatchComplex(a, bn) / z0;
        const MatchComplex y2 = 1.0 / transformImpedance(spacing, 1.0 / y1, b.request.frequency, b.request.frequency);
        const double b2 = yt.imag() - y2.imag();
        for (const bool open : {true, false}) {
            Ladder l;
            append(l, stub(open, b2, z0, b.scale));
            l.push_back(spacing);
            append(l, stub(open, b1, z0, b.scale));
            b.add(l, (open ? tr("Double open stub, spacing %1") : tr("Double short stub, spacing %1")).arg(formatDeg(d)));
        }
        if (*root == 0)
            break;
    }
}

double thetaAt(const MatchElement &e, double frequency, double f0)
{
    return radians(e.lengthDeg) * frequency / f0;
}

MatchComplex lineTransform(MatchComplex z, double z0, double theta)
{
    const double c = std::cos(theta), s = std::sin(theta);
    const MatchComplex j(0, 1);
    return z0 * (z * c + j * z0 * s) / (z0 * c + j * z * s);
}

// Susceptance (shunt) or reactance (series) of a lumped element or stub at f.
double reactiveValue(const MatchElement &e, double frequency, double f0)
{
    const double omega = 2 * kPi * frequency;
    switch (e.kind) {
    case MatchElementKind::Capacitor:
        return e.placement == MatchPlacement::Series ? -1.0 / (omega * e.value) : omega * e.value;
    case MatchElementKind::Inductor:
        return e.placement == MatchPlacement::Series ? omega * e.value : -1.0 / (omega * e.value);
    case MatchElementKind::OpenStub: return std::tan(thetaAt(e, frequency, f0)) / e.z0;
    case MatchElementKind::ShortStub: return -1.0 / (e.z0 * std::tan(thetaAt(e, frequency, f0)));
    case MatchElementKind::Line: break;
    }
    return 0;
}

MatchComplex fromAdmittance(MatchComplex y)
{
    if (std::abs(y) < 1e-300)
        return MatchComplex(1e300, 0);
    return 1.0 / y;
}

} // namespace

QString matchTopologyName(MatchTopology topology)
{
    switch (topology) {
    case MatchTopology::LNetwork: return tr("L-network");
    case MatchTopology::PiNetwork: return tr("Pi-network");
    case MatchTopology::TNetwork: return tr("T-network");
    case MatchTopology::MultiSection: return tr("Multi-section L-ladder");
    case MatchTopology::QuarterWave: return tr("Quarter-wave transformer");
    case MatchTopology::SingleStub: return tr("Single stub");
    case MatchTopology::DoubleStub: return tr("Double stub");
    }
    return {};
}

double minimumPiQ(MatchComplex zs, MatchComplex zl)
{
    const double rps = 1 / (1.0 / zs).real(), rpl = 1 / (1.0 / zl).real();
    return std::sqrt(std::max(rps, rpl) / std::min(rps, rpl) - 1);
}

double minimumTQ(MatchComplex zs, MatchComplex zl)
{
    const double rs = zs.real(), rl = zl.real();
    return std::sqrt(std::max(rs, rl) / std::min(rs, rl) - 1);
}

MatchComplex transformImpedance(const MatchElement &element, MatchComplex loadSide, double frequency, double f0)
{
    if (element.kind == MatchElementKind::Line)
        return lineTransform(loadSide, element.z0, thetaAt(element, frequency, f0));
    const double v = reactiveValue(element, frequency, f0);
    if (element.placement == MatchPlacement::Series)
        return loadSide + MatchComplex(0, v);
    return fromAdmittance(1.0 / loadSide + MatchComplex(0, v));
}

MatchComplex inputImpedance(const MatchSolution &solution, double frequency)
{
    MatchComplex z = solution.zl;
    for (auto it = solution.elements.rbegin(); it != solution.elements.rend(); ++it)
        z = transformImpedance(*it, z, frequency, solution.frequency);
    return z;
}

MatchComplex inputReflection(const MatchSolution &solution, double frequency)
{
    const MatchComplex zin = inputImpedance(solution, frequency);
    if (!isFinite(zin) || std::abs(zin) > 1e200)
        return 1.0;
    return (zin - std::conj(solution.zs)) / (zin + solution.zs);
}

std::vector<double> reflectionMagnitude(const MatchSolution &solution, const std::vector<double> &frequencies)
{
    std::vector<double> out;
    out.reserve(frequencies.size());
    for (const double f : frequencies)
        out.push_back(std::abs(inputReflection(solution, f)));
    return out;
}

std::vector<TrajectorySegment> impedanceTrajectory(const MatchSolution &solution, int pointsPerElement)
{
    const int n = std::max(1, pointsPerElement);
    const double f0 = solution.frequency;
    std::vector<TrajectorySegment> out;
    MatchComplex z = solution.zl;
    for (int index = static_cast<int>(solution.elements.size()) - 1; index >= 0; --index) {
        const MatchElement &e = solution.elements[static_cast<size_t>(index)];
        TrajectorySegment segment;
        segment.elementIndex = index;
        segment.impedances.reserve(static_cast<size_t>(n) + 1);
        const double v = e.kind == MatchElementKind::Line ? 0.0 : reactiveValue(e, f0, f0);
        for (int i = 0; i <= n; ++i) {
            const double t = double(i) / n;
            MatchComplex p;
            if (e.kind == MatchElementKind::Line)
                p = lineTransform(z, e.z0, t * radians(e.lengthDeg));
            else if (e.placement == MatchPlacement::Series)
                p = z + MatchComplex(0, t * v);
            else
                p = fromAdmittance(1.0 / z + MatchComplex(0, t * v));
            segment.impedances.push_back(p);
        }
        z = transformImpedance(e, z, f0, f0);
        segment.impedances.back() = z;
        out.push_back(std::move(segment));
    }
    return out;
}

void updateFigures(MatchSolution &solution)
{
    const double f0 = solution.frequency;
    // Nodal Q of the network: |X|/R at the internal nodes (between elements),
    // looking toward the load; the terminations are not included.
    double q = 0;
    MatchComplex z = solution.zl;
    for (size_t i = solution.elements.size(); i-- > 1;) {
        z = transformImpedance(solution.elements[i], z, f0, f0);
        if (z.real() > 0 && std::abs(z) < 1e200)
            q = std::max(q, std::abs(z.imag()) / z.real());
    }
    solution.loadedQ = q;
    solution.bandwidthEstimate = q > 0 ? f0 / q : std::numeric_limits<double>::infinity();
    solution.mismatch = std::abs(inputReflection(solution, f0));

    const auto bad = [&](double f) { return std::norm(inputReflection(solution, f)) > 0.5; };
    solution.bandwidth3dB = std::numeric_limits<double>::quiet_NaN();
    if (bad(f0))
        return;
    const auto edge = [&](double direction) -> std::optional<double> {
        const double step = 0.002 * f0;
        double inside = f0;
        for (int i = 1; i <= 1000; ++i) {
            const double f = f0 + direction * step * i;
            if (f <= 0 || f >= 3 * f0)
                return std::nullopt;
            if (bad(f)) {
                double lo = inside, hi = f;
                for (int k = 0; k < 50; ++k) {
                    const double mid = 0.5 * (lo + hi);
                    (bad(mid) ? hi : lo) = mid;
                }
                return 0.5 * (lo + hi);
            }
            inside = f;
        }
        return std::nullopt;
    };
    const auto upper = edge(1.0), lower = edge(-1.0);
    if (upper && lower)
        solution.bandwidth3dB = *upper - *lower;
}

MatchSolution snapSolution(const MatchSolution &solution, ESeries series)
{
    MatchSolution s = solution;
    bool any = false;
    for (MatchElement &e : s.elements) {
        if (!e.isLumped())
            continue;
        e.value = snapToSeries(e.idealValue, series);
        any = true;
    }
    if (any) {
        s.snapped = true;
        s.series = series;
        s.description += tr(" [%1]").arg(seriesName(series));
    }
    updateFigures(s);
    return s;
}

QString describeElement(const MatchElement &e)
{
    const QString place = e.placement == MatchPlacement::Series ? tr("series") : tr("shunt");
    switch (e.kind) {
    case MatchElementKind::Capacitor: return tr("%1 C %2").arg(place, formatEngineering(e.value, QStringLiteral("F"), 4));
    case MatchElementKind::Inductor: return tr("%1 L %2").arg(place, formatEngineering(e.value, QStringLiteral("H"), 4));
    case MatchElementKind::Line:
    case MatchElementKind::OpenStub:
    case MatchElementKind::ShortStub:
        return tr("%1 %2 %3, %4").arg(place, kindLetter(e), formatOhm(e.z0), formatDeg(e.lengthDeg));
    }
    return {};
}

MatchResult synthesizeMatch(const MatchRequest &request)
{
    MatchResult result;
    if (!(request.frequency > 0) || !std::isfinite(request.frequency)) {
        result.error = tr("The design frequency must be a positive number.");
        return result;
    }
    if (!isFinite(request.zs) || !(request.zs.real() > 0)) {
        result.error = tr("The source resistance must be positive.");
        return result;
    }
    if (!isFinite(request.zl) || !(request.zl.real() > 0)) {
        result.error = tr("The load resistance must be positive.");
        return result;
    }
    if (request.lineZ0 < 0 || !std::isfinite(request.lineZ0)) {
        result.error = tr("The line impedance must be positive.");
        return result;
    }
    Builder b{request, result, 2 * kPi * request.frequency, scaleOf(request.zs, request.zl)};
    switch (request.topology) {
    case MatchTopology::LNetwork: synthesizeL(b); break;
    case MatchTopology::PiNetwork: synthesizePi(b); break;
    case MatchTopology::TNetwork: synthesizeT(b); break;
    case MatchTopology::MultiSection: synthesizeMultiSection(b); break;
    case MatchTopology::QuarterWave: synthesizeQuarterWave(b); break;
    case MatchTopology::SingleStub: synthesizeSingleStub(b); break;
    case MatchTopology::DoubleStub: synthesizeDoubleStub(b); break;
    }
    if (!result.error.isEmpty()) {
        result.solutions.clear();
        return result;
    }
    if (result.solutions.empty()) {
        result.error = tr("No %1 solution exists for these impedances.").arg(matchTopologyName(request.topology));
        return result;
    }
    for (MatchSolution &s : result.solutions) {
        if (request.series)
            s = snapSolution(s, *request.series);
        else
            updateFigures(s);
    }
    return result;
}

} // namespace lvd::rf

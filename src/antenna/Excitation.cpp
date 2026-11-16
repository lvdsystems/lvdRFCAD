#include "antenna/Excitation.h"

#include <QJsonArray>

#include <algorithm>
#include <map>
#include <random>

namespace lvd::ant {

namespace {

struct Named {
    int value;
    const char *id;
    const char *name;
};

constexpr Named kTapers[] = {
    {int(TaperKind::Uniform), "uniform", "Uniform"},
    {int(TaperKind::Taylor), "taylor", "Taylor"},
    {int(TaperKind::Chebyshev), "chebyshev", "Chebyshev (Dolph)"},
    {int(TaperKind::Cosine), "cosine", "Cosine^n"},
    {int(TaperKind::CosinePedestal), "cosine-pedestal", "Cosine squared on a pedestal"},
    {int(TaperKind::Hamming), "hamming", "Hamming"},
    {int(TaperKind::Custom), "custom", "Custom (per element)"},
};

constexpr Named kSteering[] = {
    {int(SteeringMode::Phase), "phase", "Phase shifters"},
    {int(SteeringMode::TimeDelay), "ttd", "True time delay per element"},
    {int(SteeringMode::SubarrayTimeDelay), "subarray-ttd", "Time delay per sub-array, phase per element"},
    {int(SteeringMode::None), "none", "None (feed phases only)"},
};

constexpr Named kPatterns[] = {
    {int(BeamPattern::Sum), "sum", "Sum"},
    {int(BeamPattern::DeltaAzimuth), "delta-az", "Difference, azimuth (x)"},
    {int(BeamPattern::DeltaElevation), "delta-el", "Difference, elevation (y)"},
};

template<std::size_t N>
QString idOf(const Named (&table)[N], int value)
{
    for (const Named &n : table)
        if (n.value == value)
            return QString::fromLatin1(n.id);
    return QString::fromLatin1(table[0].id);
}

template<std::size_t N>
QString nameOf(const Named (&table)[N], int value)
{
    for (const Named &n : table)
        if (n.value == value)
            return QString::fromLatin1(n.name);
    return {};
}

template<std::size_t N>
int valueOf(const Named (&table)[N], const QString &id, int fallback)
{
    for (const Named &n : table)
        if (id == QLatin1String(n.id))
            return n.value;
    return fallback;
}

// Zeros of J1, for the circular Taylor distribution.
constexpr double kJ1Zeros[] = {3.8317059702, 7.0155866698, 10.1734681351, 13.3236919363, 16.4706300509,
                               19.6158585105, 22.7600843806, 25.9036720876, 29.0468285349, 32.1896799110,
                               35.3323075501, 38.4747662348, 41.6170942128, 44.7593189977, 47.9014608872};

double wrapDegrees(double d)
{
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0)
        d += 360.0;
    return d - 180.0;
}

QJsonArray toArray(const std::vector<int> &values)
{
    QJsonArray a;
    for (int v : values)
        a.append(v);
    return a;
}

} // namespace

QString taperId(TaperKind kind) { return idOf(kTapers, int(kind)); }
QString taperName(TaperKind kind) { return nameOf(kTapers, int(kind)); }
std::vector<TaperKind> taperKinds()
{
    std::vector<TaperKind> v;
    for (const Named &n : kTapers)
        v.push_back(TaperKind(n.value));
    return v;
}
QString steeringModeId(SteeringMode mode) { return idOf(kSteering, int(mode)); }
QString steeringModeName(SteeringMode mode) { return nameOf(kSteering, int(mode)); }
QString beamPatternId(BeamPattern pattern) { return idOf(kPatterns, int(pattern)); }
QString beamPatternName(BeamPattern pattern) { return nameOf(kPatterns, int(pattern)); }

// ---- JSON

QJsonObject TaperSpec::toJson() const
{
    QJsonObject o{{QStringLiteral("kind"), taperId(kind)}};
    if (kind == TaperKind::Taylor || kind == TaperKind::Chebyshev)
        o.insert(QStringLiteral("sll"), sllDb);
    if (kind == TaperKind::Taylor)
        o.insert(QStringLiteral("nbar"), nbar);
    if (kind == TaperKind::Cosine)
        o.insert(QStringLiteral("exponent"), exponent);
    if (kind == TaperKind::CosinePedestal)
        o.insert(QStringLiteral("pedestal"), pedestalDb);
    return o;
}

TaperSpec TaperSpec::fromJson(const QJsonObject &o)
{
    TaperSpec s;
    s.kind = TaperKind(valueOf(kTapers, o.value(QStringLiteral("kind")).toString(), int(TaperKind::Uniform)));
    s.sllDb = std::clamp(o.value(QStringLiteral("sll")).toDouble(s.sllDb), 10.0, 100.0);
    s.nbar = std::clamp(o.value(QStringLiteral("nbar")).toInt(s.nbar), 1, 15);
    s.exponent = std::clamp(o.value(QStringLiteral("exponent")).toDouble(s.exponent), 0.0, 10.0);
    s.pedestalDb = std::clamp(o.value(QStringLiteral("pedestal")).toDouble(s.pedestalDb), -60.0, 0.0);
    return s;
}

QJsonObject ExcitationSpec::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("taperX"), x.toJson());
    o.insert(QStringLiteral("taperY"), y.toJson());
    if (radial)
        o.insert(QStringLiteral("radial"), true);
    o.insert(QStringLiteral("steerTheta"), steerThetaDeg);
    o.insert(QStringLiteral("steerPhi"), steerPhiDeg);
    o.insert(QStringLiteral("steering"), steeringModeId(steering));
    if (subarrayX > 0 || subarrayY > 0) {
        o.insert(QStringLiteral("subarrayX"), subarrayX);
        o.insert(QStringLiteral("subarrayY"), subarrayY);
        o.insert(QStringLiteral("subarrayTaper"), subarrayTaper);
    }
    if (!extraBeams.empty()) {
        QJsonArray list;
        for (const BeamSpec &b : extraBeams)
            list.append(QJsonObject{{QStringLiteral("theta"), b.thetaDeg},
                                    {QStringLiteral("phi"), b.phiDeg},
                                    {QStringLiteral("level"), b.levelDb},
                                    {QStringLiteral("phase"), b.phaseDeg}});
        o.insert(QStringLiteral("beams"), list);
    }
    if (!nulls.empty()) {
        QJsonArray list;
        for (const DirectionSpec &d : nulls)
            list.append(QJsonObject{{QStringLiteral("theta"), d.thetaDeg}, {QStringLiteral("phi"), d.phiDeg}});
        o.insert(QStringLiteral("nulls"), list);
    }
    if (pattern != BeamPattern::Sum)
        o.insert(QStringLiteral("pattern"), beamPatternId(pattern));
    if (!weights.empty()) {
        QJsonArray list;
        for (const auto &[a, p] : weights)
            list.append(QJsonArray{a, p});
        o.insert(QStringLiteral("weights"), list);
    }
    return o;
}

ExcitationSpec ExcitationSpec::fromJson(const QJsonObject &o)
{
    ExcitationSpec s;
    s.x = TaperSpec::fromJson(o.value(QStringLiteral("taperX")).toObject());
    s.y = TaperSpec::fromJson(o.value(QStringLiteral("taperY")).toObject());
    s.radial = o.value(QStringLiteral("radial")).toBool(false);
    s.steerThetaDeg = std::clamp(o.value(QStringLiteral("steerTheta")).toDouble(0), 0.0, 180.0);
    s.steerPhiDeg = o.value(QStringLiteral("steerPhi")).toDouble(0);
    s.steering = SteeringMode(valueOf(kSteering, o.value(QStringLiteral("steering")).toString(), int(SteeringMode::Phase)));
    s.subarrayX = std::max(0, o.value(QStringLiteral("subarrayX")).toInt(0));
    s.subarrayY = std::max(0, o.value(QStringLiteral("subarrayY")).toInt(0));
    s.subarrayTaper = o.value(QStringLiteral("subarrayTaper")).toBool(false);
    for (const QJsonValue &v : o.value(QStringLiteral("beams")).toArray()) {
        const QJsonObject b = v.toObject();
        s.extraBeams.push_back({b.value(QStringLiteral("theta")).toDouble(), b.value(QStringLiteral("phi")).toDouble(),
                                b.value(QStringLiteral("level")).toDouble(), b.value(QStringLiteral("phase")).toDouble()});
    }
    for (const QJsonValue &v : o.value(QStringLiteral("nulls")).toArray()) {
        const QJsonObject d = v.toObject();
        s.nulls.push_back({d.value(QStringLiteral("theta")).toDouble(), d.value(QStringLiteral("phi")).toDouble()});
    }
    s.pattern = BeamPattern(valueOf(kPatterns, o.value(QStringLiteral("pattern")).toString(), int(BeamPattern::Sum)));
    for (const QJsonValue &v : o.value(QStringLiteral("weights")).toArray()) {
        const QJsonArray pair = v.toArray();
        s.weights.emplace_back(pair.at(0).toDouble(), pair.at(1).toDouble());
    }
    return s;
}

bool AesaSpec::isIdeal() const
{
    return phaseBits == 0 && attenuatorBits == 0 && delayLsbPs == 0 && amplitudeErrorDb == 0 && phaseErrorDeg == 0
           && failedFraction == 0 && failed.empty();
}

QJsonObject AesaSpec::toJson() const
{
    return QJsonObject{{QStringLiteral("phaseBits"), phaseBits},
                       {QStringLiteral("attenuatorBits"), attenuatorBits},
                       {QStringLiteral("attenuatorStep"), attenuatorStepDb},
                       {QStringLiteral("delayLsb"), delayLsbPs},
                       {QStringLiteral("amplitudeError"), amplitudeErrorDb},
                       {QStringLiteral("phaseError"), phaseErrorDeg},
                       {QStringLiteral("failedFraction"), failedFraction},
                       {QStringLiteral("failed"), toArray(failed)},
                       {QStringLiteral("seed"), int(seed)}};
}

AesaSpec AesaSpec::fromJson(const QJsonObject &o)
{
    AesaSpec s;
    s.phaseBits = std::clamp(o.value(QStringLiteral("phaseBits")).toInt(0), 0, 16);
    s.attenuatorBits = std::clamp(o.value(QStringLiteral("attenuatorBits")).toInt(0), 0, 16);
    s.attenuatorStepDb = std::max(0.01, o.value(QStringLiteral("attenuatorStep")).toDouble(0.5));
    s.delayLsbPs = std::max(0.0, o.value(QStringLiteral("delayLsb")).toDouble(0));
    s.amplitudeErrorDb = std::max(0.0, o.value(QStringLiteral("amplitudeError")).toDouble(0));
    s.phaseErrorDeg = std::max(0.0, o.value(QStringLiteral("phaseError")).toDouble(0));
    s.failedFraction = std::clamp(o.value(QStringLiteral("failedFraction")).toDouble(0), 0.0, 1.0);
    for (const QJsonValue &v : o.value(QStringLiteral("failed")).toArray())
        s.failed.push_back(v.toInt());
    s.seed = unsigned(std::max(0, o.value(QStringLiteral("seed")).toInt(1)));
    return s;
}

// ---- Taper functions

double besselJ0(double x)
{
    // Rational and asymptotic approximations (Numerical Recipes), ~1e-8.
    const double ax = std::abs(x);
    if (ax < 8.0) {
        const double y = x * x;
        const double a = 57568490574.0
                         + y * (-13362590354.0 + y * (651619640.7 + y * (-11214424.18 + y * (77392.33017 + y * (-184.9052456)))));
        const double b = 57568490411.0 + y * (1029532985.0 + y * (9494680.718 + y * (59272.64853 + y * (267.8532712 + y))));
        return a / b;
    }
    const double z = 8.0 / ax;
    const double y = z * z;
    const double xx = ax - 0.785398164;
    const double a = 1.0 + y * (-0.1098628627e-2 + y * (0.2734510407e-4 + y * (-0.2073370639e-5 + y * 0.2093887211e-6)));
    const double b = -0.1562499995e-1 + y * (0.1430488765e-3 + y * (-0.6911147651e-5 + y * (0.7621095161e-6 - y * 0.934935152e-7)));
    return std::sqrt(0.636619772 / ax) * (std::cos(xx) * a - z * std::sin(xx) * b);
}

namespace {

// Taylor line-source coefficients F_m, m = 1 .. nbar-1.
std::vector<double> taylorCoefficients(double sllDb, int nbar)
{
    std::vector<double> f;
    if (nbar < 2)
        return f;
    const double r = std::pow(10.0, sllDb / 20.0);
    const double a = std::acosh(r) / kPi;
    const double sigma2 = double(nbar) * nbar / (a * a + (nbar - 0.5) * (nbar - 0.5));
    for (int m = 1; m < nbar; ++m) {
        double num = 1, den = 1;
        for (int n = 1; n < nbar; ++n) {
            num *= 1.0 - double(m) * m / (sigma2 * (a * a + (n - 0.5) * (n - 0.5)));
            if (n != m)
                den *= 1.0 - double(m) * m / (double(n) * n);
        }
        f.push_back(((m % 2) ? 1.0 : -1.0) * num / (2.0 * den));
    }
    return f;
}

} // namespace

double lineTaper(const TaperSpec &spec, double p)
{
    p = std::clamp(p, -1.0, 1.0);
    switch (spec.kind) {
    case TaperKind::Uniform:
    case TaperKind::Custom:
    case TaperKind::Chebyshev: // discrete; handled by the caller
        return 1.0;
    case TaperKind::Taylor: {
        const std::vector<double> f = taylorCoefficients(spec.sllDb, spec.nbar);
        double g = 1.0;
        for (std::size_t m = 0; m < f.size(); ++m)
            g += 2.0 * f[m] * std::cos(double(m + 1) * kPi * p);
        return g;
    }
    case TaperKind::Cosine:
        return std::pow(std::cos(kPi * p / 2), spec.exponent);
    case TaperKind::CosinePedestal: {
        const double ped = amplitudeFromDb(spec.pedestalDb);
        return ped + (1 - ped) * std::pow(std::cos(kPi * p / 2), 2);
    }
    case TaperKind::Hamming:
        return 0.54 + 0.46 * std::cos(kPi * p);
    }
    return 1.0;
}

double circularTaylor(double sllDb, int nbar, double p)
{
    nbar = std::clamp(nbar, 1, int(std::size(kJ1Zeros)));
    if (nbar < 2)
        return 1.0;
    p = std::clamp(p, 0.0, 1.0);
    const double r = std::pow(10.0, sllDb / 20.0);
    const double a = std::acosh(r) / kPi;
    auto mu = [](int n) { return kJ1Zeros[n - 1] / kPi; }; // zeros of J1(pi mu)
    const double sigma = mu(nbar) / std::sqrt(a * a + (nbar - 0.5) * (nbar - 0.5));
    std::vector<double> u(std::size_t(nbar), 0.0);
    for (int n = 1; n < nbar; ++n)
        u[std::size_t(n)] = sigma * std::sqrt(a * a + (n - 0.5) * (n - 0.5));

    double g = 1.0; // m = 0: F(0) J0(0) / J0(0)^2
    for (int m = 1; m < nbar; ++m) {
        const double mm = mu(m);
        const double j0 = besselJ0(kPi * mm);
        double num = 1, den = 1;
        for (int n = 1; n < nbar; ++n) {
            num *= 1.0 - mm * mm / (u[std::size_t(n)] * u[std::size_t(n)]);
            if (n != m)
                den *= 1.0 - mm * mm / (mu(n) * mu(n));
        }
        const double f = -j0 * num / den;
        g += f * besselJ0(kPi * mm * p) / (j0 * j0);
    }
    return g;
}

std::vector<double> dolphChebyshev(int count, double sllDb)
{
    if (count <= 0)
        return {};
    if (count == 1)
        return {1.0};
    // The array polynomial has the Chebyshev pattern's zeros:
    // x0 cos(psi/2) = cos((2k-1) pi / (2(N-1))).
    const int n = count - 1;
    const double r = std::pow(10.0, sllDb / 20.0);
    const double x0 = std::cosh(std::acosh(r) / n);
    std::vector<cplx> poly{1.0};
    for (int k = 1; k <= n; ++k) {
        const double xk = std::cos((2.0 * k - 1) * kPi / (2.0 * n));
        const double psi = 2.0 * std::acos(std::clamp(xk / x0, -1.0, 1.0));
        const cplx zero = std::polar(1.0, psi);
        std::vector<cplx> next(poly.size() + 1, 0.0);
        for (std::size_t i = 0; i < poly.size(); ++i) {
            next[i + 1] += poly[i];
            next[i] -= poly[i] * zero;
        }
        poly = std::move(next);
    }
    std::vector<double> w;
    double peak = 0;
    for (const cplx &c : poly) {
        w.push_back(std::abs(c.real()));
        peak = std::max(peak, std::abs(c.real()));
    }
    for (double &v : w)
        v /= peak;
    return w;
}

// ---- Excitation

ExcitationResult computeExcitation(const ArrayLayout &layout, const ExcitationSpec &spec, const AesaSpec &aesa,
                                   double frequency, double designFrequency, bool applyAesa)
{
    ExcitationResult result;
    const auto &elements = layout.elements;
    const std::size_t count = elements.size();
    result.weights.assign(count, 0.0);
    result.subarray.assign(count, -1);
    result.failed.assign(count, false);
    if (count == 0)
        return result;

    const bool cylinder = layout.kind == LatticeKind::Cylinder;
    const bool linear = layout.kind == LatticeKind::Linear;

    // Taper coordinates: u along x (or the arc), v along y (or the axis).
    std::vector<double> u(count), v(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3 &p = elements[i].position;
        if (cylinder) {
            u[i] = layout.cylinderRadius * std::atan2(p.y(), p.x());
            v[i] = p.z();
        } else {
            u[i] = p.x();
            v[i] = p.y();
        }
    }
    const auto [uMinIt, uMaxIt] = std::minmax_element(u.begin(), u.end());
    const auto [vMinIt, vMaxIt] = std::minmax_element(v.begin(), v.end());
    const double cu = (*uMinIt + *uMaxIt) / 2, cv = (*vMinIt + *vMaxIt) / 2;
    const double hu = (*uMaxIt - *uMinIt + layout.pitchX) / 2;
    const double hv = (*vMaxIt - *vMinIt + layout.pitchY) / 2;

    const bool radial = spec.radial && !cylinder && !linear;
    double radius = 0;
    if (radial) {
        for (std::size_t i = 0; i < count; ++i)
            radius = std::max(radius, std::hypot(u[i] - cu, v[i] - cv));
        radius += std::max(layout.pitchX, layout.pitchY) / 2;
    }

    // Chebyshev weights by the rank of each distinct coordinate.
    auto chebyshevAlong = [&](const std::vector<double> &coord, double pitch, double sllDb) {
        std::vector<double> keys;
        const double quantum = std::max(pitch, 1e-12) * 1e-3;
        for (double c : coord)
            keys.push_back(std::round(c / quantum));
        std::vector<double> unique = keys;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        const std::vector<double> w = dolphChebyshev(int(unique.size()), sllDb);
        std::vector<double> out(coord.size());
        for (std::size_t i = 0; i < coord.size(); ++i)
            out[i] = w[std::size_t(std::lower_bound(unique.begin(), unique.end(), keys[i]) - unique.begin())];
        if (layout.lattice && layout.lattice->basis.size() == 2 && layout.lattice->basis[1].x() != 0)
            result.warnings << QStringLiteral("Chebyshev weights on a staggered lattice are sampled per column");
        return out;
    };

    auto taperAt = [&](double uu, double vv, std::size_t index, const std::vector<double> &chebU, const std::vector<double> &chebV) {
        if (radial) {
            const double p = std::hypot(uu - cu, vv - cv) / radius;
            if (spec.x.kind == TaperKind::Taylor || spec.x.kind == TaperKind::Chebyshev)
                return circularTaylor(spec.x.sllDb, spec.x.kind == TaperKind::Chebyshev ? std::max(spec.x.nbar, 6) : spec.x.nbar, p);
            return lineTaper(spec.x, p);
        }
        double a = spec.x.kind == TaperKind::Chebyshev && index < chebU.size() ? chebU[index] : lineTaper(spec.x, (uu - cu) / hu);
        if (!linear)
            a *= spec.y.kind == TaperKind::Chebyshev && index < chebV.size() ? chebV[index] : lineTaper(spec.y, (vv - cv) / hv);
        return a;
    };

    std::vector<double> chebU, chebV;
    if (!radial && spec.x.kind == TaperKind::Chebyshev)
        chebU = chebyshevAlong(u, layout.pitchX, spec.x.sllDb);
    if (!radial && !linear && spec.y.kind == TaperKind::Chebyshev)
        chebV = chebyshevAlong(v, layout.pitchY, spec.y.sllDb);
    if (radial && spec.x.kind == TaperKind::Chebyshev)
        result.warnings << QStringLiteral("a radial Chebyshev taper is approximated by a circular Taylor taper");

    // Sub-arrays from the lattice indices.
    const bool wantSubarrays = spec.subarrayX > 0 || spec.subarrayY > 0;
    if (wantSubarrays) {
        bool indexed = true;
        int maxColumn = 0;
        for (const ArrayElement &e : elements) {
            indexed = indexed && e.row >= 0 && e.column >= 0;
            maxColumn = std::max(maxColumn, e.column);
        }
        if (!indexed || layout.kind == LatticeKind::Circular || layout.kind == LatticeKind::Custom) {
            result.warnings << QStringLiteral("sub-arrays need a linear, planar or cylinder lattice");
        } else {
            const int sx = std::max(1, spec.subarrayX), sy = std::max(1, spec.subarrayY);
            const int blocksX = maxColumn / sx + 1;
            std::map<int, int> compact;
            for (std::size_t i = 0; i < count; ++i) {
                const int key = (elements[i].row / sy) * blocksX + elements[i].column / sx;
                const auto it = compact.emplace(key, int(compact.size())).first;
                result.subarray[i] = it->second;
            }
            result.subarrayCentres.assign(compact.size(), Vec3::Zero());
            std::vector<int> members(compact.size(), 0);
            for (std::size_t i = 0; i < count; ++i) {
                result.subarrayCentres[std::size_t(result.subarray[i])] += elements[i].position;
                ++members[std::size_t(result.subarray[i])];
            }
            for (std::size_t s = 0; s < members.size(); ++s)
                result.subarrayCentres[s] /= double(members[s]);
        }
    }
    const bool haveSubarrays = !result.subarrayCentres.empty();

    // Amplitude (and custom phase) taper.
    std::vector<cplx> taper(count, 1.0);
    const bool custom = spec.x.kind == TaperKind::Custom;
    if (custom) {
        if (spec.weights.size() == count) {
            // -100 dB or less switches the element off.
            for (std::size_t i = 0; i < count; ++i)
                taper[i] = spec.weights[i].first <= -100 ? cplx(0.0)
                                                         : std::polar(amplitudeFromDb(spec.weights[i].first), toRadians(spec.weights[i].second));
        } else {
            if (!spec.weights.empty())
                result.warnings << QStringLiteral("the custom weights are for %1 elements, the array has %2; using the element table")
                                       .arg(spec.weights.size())
                                       .arg(count);
            for (std::size_t i = 0; i < count; ++i)
                taper[i] = elements[i].amplitudeDb <= -100 ? cplx(0.0)
                                                           : std::polar(amplitudeFromDb(elements[i].amplitudeDb), toRadians(elements[i].phaseDeg));
        }
    } else if (haveSubarrays && spec.subarrayTaper) {
        // Sub-array centres in taper coordinates.
        std::vector<double> su(result.subarrayCentres.size()), sv(result.subarrayCentres.size());
        for (std::size_t s = 0; s < su.size(); ++s) {
            const Vec3 &c = result.subarrayCentres[s];
            su[s] = cylinder ? layout.cylinderRadius * std::atan2(c.y(), c.x()) : c.x();
            sv[s] = cylinder ? c.z() : c.y();
        }
        std::vector<double> cu2, cv2;
        if (!radial && spec.x.kind == TaperKind::Chebyshev)
            cu2 = chebyshevAlong(su, layout.pitchX * std::max(1, spec.subarrayX), spec.x.sllDb);
        if (!radial && !linear && spec.y.kind == TaperKind::Chebyshev)
            cv2 = chebyshevAlong(sv, layout.pitchY * std::max(1, spec.subarrayY), spec.y.sllDb);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t s = std::size_t(result.subarray[i]);
            taper[i] = taperAt(su[s], sv[s], s, cu2, cv2);
        }
    } else {
        for (std::size_t i = 0; i < count; ++i)
            taper[i] = taperAt(u[i], v[i], i, chebU, chebV);
    }

    // Steering of every beam.
    SteeringMode mode = spec.steering;
    if (mode == SteeringMode::SubarrayTimeDelay && !haveSubarrays) {
        result.warnings << QStringLiteral("time delay per sub-array needs sub-arrays; using time delay per element");
        mode = SteeringMode::TimeDelay;
    }
    const double k0 = 2 * kPi * designFrequency / kSpeedOfLight;
    const double k = 2 * kPi * frequency / kSpeedOfLight;
    const double lsb = (applyAesa ? aesa.delayLsbPs : 0.0) * 1e-12;

    // Delay phase (true time delay) and phase-shifter phase of one beam.
    auto steer = [&](const Vec3 &dir, std::vector<double> &delayPhase, std::vector<double> &shifterPhase) {
        delayPhase.assign(count, 0.0);
        shifterPhase.assign(count, 0.0);
        if (mode == SteeringMode::None)
            return;
        if (mode == SteeringMode::Phase) {
            for (std::size_t i = 0; i < count; ++i)
                shifterPhase[i] = -k0 * dir.dot(elements[i].position);
            return;
        }
        std::vector<double> tau(count);
        for (std::size_t i = 0; i < count; ++i) {
            const Vec3 &ref = mode == SteeringMode::TimeDelay ? elements[i].position
                                                               : result.subarrayCentres[std::size_t(result.subarray[i])];
            tau[i] = dir.dot(ref) / kSpeedOfLight;
            if (mode == SteeringMode::SubarrayTimeDelay)
                shifterPhase[i] = -k0 * dir.dot(elements[i].position - ref);
        }
        const double t0 = *std::min_element(tau.begin(), tau.end());
        for (std::size_t i = 0; i < count; ++i) {
            double t = tau[i] - t0;
            if (lsb > 0)
                t = std::round(t / lsb) * lsb;
            delayPhase[i] = -2 * kPi * frequency * t;
        }
    };

    std::vector<double> mainDelay, mainShifter;
    const Vec3 mainDir = radialUnit(toRadians(spec.steerThetaDeg), toRadians(spec.steerPhiDeg));
    steer(mainDir, mainDelay, mainShifter);
    std::vector<cplx> w(count);
    for (std::size_t i = 0; i < count; ++i)
        w[i] = taper[i] * std::polar(1.0, mainDelay[i] + mainShifter[i]);
    for (const BeamSpec &beam : spec.extraBeams) {
        std::vector<double> d, s;
        steer(radialUnit(toRadians(beam.thetaDeg), toRadians(beam.phiDeg)), d, s);
        const cplx g = std::polar(amplitudeFromDb(beam.levelDb), toRadians(beam.phaseDeg));
        for (std::size_t i = 0; i < count; ++i)
            w[i] += g * taper[i] * std::polar(1.0, d[i] + s[i]);
    }

    // Nulls: project the weights onto the space orthogonal to the
    // constraints sum_n w_n exp(j k r_k . p_n) = 0.
    if (!spec.nulls.empty()) {
        if (!layout.identicalOrientation)
            result.warnings << QStringLiteral("nulls are placed in the array factor; rotated elements fill them partly");
        const Eigen::Index n = Eigen::Index(count), kn = Eigen::Index(spec.nulls.size());
        Eigen::MatrixXcd c(kn, n);
        for (Eigen::Index r = 0; r < kn; ++r) {
            const Vec3 dir = radialUnit(toRadians(spec.nulls[std::size_t(r)].thetaDeg), toRadians(spec.nulls[std::size_t(r)].phiDeg));
            for (Eigen::Index col = 0; col < n; ++col)
                c(r, col) = std::polar(1.0, k * dir.dot(elements[std::size_t(col)].position));
        }
        Eigen::VectorXcd wv(n);
        for (Eigen::Index i = 0; i < n; ++i)
            wv(i) = w[std::size_t(i)];
        const Eigen::MatrixXcd gram = c * c.adjoint();
        const Eigen::VectorXcd lambda = gram.completeOrthogonalDecomposition().solve(c * wv);
        wv -= c.adjoint() * lambda;
        for (Eigen::Index i = 0; i < n; ++i)
            w[std::size_t(i)] = wv(i);
    }

    // Monopulse difference patterns: opposite signs on the two halves.
    if (spec.pattern != BeamPattern::Sum) {
        const bool az = spec.pattern == BeamPattern::DeltaAzimuth;
        const double eps = std::max(layout.pitchX, layout.pitchY) * 1e-6;
        for (std::size_t i = 0; i < count; ++i) {
            const double d = az ? u[i] - cu : v[i] - cv;
            w[i] *= std::abs(d) < eps ? 0.0 : (d > 0 ? 1.0 : -1.0);
        }
        if (!az && linear)
            result.warnings << QStringLiteral("a linear array has no elevation difference pattern");
    }

    for (std::size_t i = 0; i < count; ++i)
        if (!elements[i].enabled) {
            w[i] = 0.0;
            result.failed[i] = true;
        }

    if (applyAesa) {
        // Phase shifters: the phase that the delays do not provide.
        if (aesa.phaseBits > 0) {
            const double step = 360.0 / std::pow(2.0, aesa.phaseBits);
            double sum2 = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (std::abs(w[i]) == 0)
                    continue;
                const double residual = toDegrees(std::arg(w[i]) - mainDelay[i]);
                const double q = std::round(residual / step) * step;
                sum2 += std::pow(wrapDegrees(q - residual), 2);
                w[i] = std::polar(std::abs(w[i]), mainDelay[i] + toRadians(q));
            }
            result.rmsPhaseQuantisationDeg = std::sqrt(sum2 / double(count));
        }
        // Attenuators, relative to the strongest element.
        if (aesa.attenuatorBits > 0) {
            double peak = 0;
            for (const cplx &x : w)
                peak = std::max(peak, std::abs(x));
            const double maxAtt = (std::pow(2.0, aesa.attenuatorBits) - 1) * aesa.attenuatorStepDb;
            for (std::size_t i = 0; i < count; ++i) {
                if (peak <= 0 || result.failed[i])
                    continue;
                const double a = std::abs(w[i]);
                const double att = a > 0 ? -20 * std::log10(a / peak) : maxAtt;
                const double q = std::min(std::round(att / aesa.attenuatorStepDb) * aesa.attenuatorStepDb, maxAtt);
                w[i] = std::polar(peak * amplitudeFromDb(-q), a > 0 ? std::arg(w[i]) : 0.0);
            }
        }
        std::mt19937 rng(aesa.seed);
        if (aesa.amplitudeErrorDb > 0 || aesa.phaseErrorDeg > 0) {
            std::normal_distribution<double> amp(0.0, aesa.amplitudeErrorDb);
            std::normal_distribution<double> ph(0.0, aesa.phaseErrorDeg);
            for (cplx &x : w) {
                const double da = aesa.amplitudeErrorDb > 0 ? amp(rng) : 0.0;
                const double dp = aesa.phaseErrorDeg > 0 ? ph(rng) : 0.0;
                x *= std::polar(amplitudeFromDb(da), toRadians(dp));
            }
        }
        if (aesa.failedFraction > 0) {
            std::vector<std::size_t> order(count);
            for (std::size_t i = 0; i < count; ++i)
                order[i] = i;
            std::shuffle(order.begin(), order.end(), rng);
            const std::size_t n = std::size_t(std::lround(aesa.failedFraction * double(count)));
            for (std::size_t i = 0; i < std::min(n, count); ++i)
                result.failed[order[i]] = true;
        }
        for (int index : aesa.failed)
            if (index >= 0 && std::size_t(index) < count)
                result.failed[std::size_t(index)] = true;
        for (std::size_t i = 0; i < count; ++i)
            if (result.failed[i])
                w[i] = 0.0;
    }
    result.weights = std::move(w);
    return result;
}

} // namespace lvd::ant

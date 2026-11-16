#include "antenna/ElementModel.h"

#include <QJsonValue>

#include <algorithm>

namespace lvd::ant {

namespace {

double sinc(double x)
{
    return std::abs(x) < 1e-9 ? 1.0 : std::sin(x) / x;
}

// cos(x) / (1 - (2x/pi)^2): the pattern of a cosine aperture distribution,
// normalised to 1 at x = 0; its removable singularity at x = pi/2 is pi/4.
double cosineAperture(double x)
{
    const double d = 1.0 - std::pow(2.0 * x / kPi, 2);
    if (std::abs(d) < 1e-7)
        return kPi / 4.0;
    return std::cos(x) / d;
}

// x-polarised Ludwig-3 field of a given amplitude.
Field ludwigX(double amplitude, double phi)
{
    return {amplitude * std::cos(phi), -amplitude * std::sin(phi)};
}

struct NamedModel {
    ElementModelKind kind;
    const char *id;
    const char *name;
};

constexpr NamedModel kModels[] = {
    {ElementModelKind::Isotropic, "isotropic", "Isotropic"},
    {ElementModelKind::CosN, "cosn", "cos^n"},
    {ElementModelKind::Dipole, "dipole", "Dipole"},
    {ElementModelKind::DipoleOverGround, "dipole-ground", "Dipole over ground"},
    {ElementModelKind::Monopole, "monopole", "Monopole over ground"},
    {ElementModelKind::Patch, "patch", "Patch (cavity model)"},
    {ElementModelKind::Aperture, "aperture", "Aperture / horn"},
    {ElementModelKind::Imported, "imported", "Imported pattern"},
};

struct NamedPolarisation {
    Polarisation polarisation;
    const char *id;
    const char *name;
};

constexpr NamedPolarisation kPolarisations[] = {
    {Polarisation::LinearX, "x", "Linear x"},
    {Polarisation::LinearY, "y", "Linear y"},
    {Polarisation::Slant45, "slant45", "Slant +45"},
    {Polarisation::SlantMinus45, "slant-45", "Slant -45"},
    {Polarisation::Rhcp, "rhcp", "RHCP"},
    {Polarisation::Lhcp, "lhcp", "LHCP"},
};

} // namespace

// ---- PatternTable

bool PatternTable::isValid() const
{
    if (theta.empty() || phi.empty() || frequencies.empty() || fields.size() != frequencies.size())
        return false;
    for (const auto &f : fields)
        if (f.size() != theta.size() * phi.size())
            return false;
    return true;
}

Field PatternTable::sample(double thetaDeg, double phiDeg, double frequency) const
{
    if (!isValid())
        return {};
    const double tol = 1e-6;
    if (thetaDeg < theta.front() - tol || thetaDeg > theta.back() + tol)
        return {};

    // Theta bracket.
    std::size_t t0 = 0;
    double ft = 0;
    if (theta.size() > 1) {
        const auto it = std::upper_bound(theta.begin(), theta.end(), thetaDeg);
        t0 = it == theta.begin() ? 0 : std::min<std::size_t>(std::size_t(it - theta.begin()) - 1, theta.size() - 2);
        ft = std::clamp((thetaDeg - theta[t0]) / (theta[t0 + 1] - theta[t0]), 0.0, 1.0);
    }
    const std::size_t t1 = theta.size() > 1 ? t0 + 1 : 0;

    // Phi bracket; periodic when the grid closes the circle.
    const std::size_t np = phi.size();
    std::size_t p0 = 0, p1 = 0;
    double fp = 0;
    if (np > 1) {
        const double step = phi[1] - phi[0];
        const bool periodic = phi.back() - phi.front() + step >= 359.9;
        double p = phiDeg;
        if (periodic) {
            p = phi.front() + std::fmod(std::fmod(p - phi.front(), 360.0) + 360.0, 360.0);
            if (p > phi.back()) {
                p0 = np - 1;
                p1 = 0;
                const double span = phi.front() + 360.0 - phi.back();
                fp = span > 0 ? (p - phi.back()) / span : 0.0;
            } else {
                const auto it = std::upper_bound(phi.begin(), phi.end(), p);
                p0 = it == phi.begin() ? 0 : std::min<std::size_t>(std::size_t(it - phi.begin()) - 1, np - 2);
                p1 = p0 + 1;
                fp = std::clamp((p - phi[p0]) / (phi[p1] - phi[p0]), 0.0, 1.0);
            }
        } else {
            // Not periodic: fold into the grid with the symmetry of a field
            // pattern, (theta, phi) == (theta, phi - 360).
            while (p < phi.front() - tol)
                p += 360.0;
            while (p > phi.back() + tol && p - 360.0 >= phi.front() - tol)
                p -= 360.0;
            p = std::clamp(p, phi.front(), phi.back());
            const auto it = std::upper_bound(phi.begin(), phi.end(), p);
            p0 = it == phi.begin() ? 0 : std::min<std::size_t>(std::size_t(it - phi.begin()) - 1, np - 2);
            p1 = p0 + 1;
            fp = std::clamp((p - phi[p0]) / (phi[p1] - phi[p0]), 0.0, 1.0);
        }
    }

    auto bilinear = [&](const std::vector<Field> &grid) {
        auto at = [&](std::size_t it, std::size_t ip) { return grid[it * np + ip]; };
        const Field a = at(t0, p0), b = at(t0, p1), c = at(t1, p0), d = at(t1, p1);
        Field r;
        r.eTheta = (1 - ft) * ((1 - fp) * a.eTheta + fp * b.eTheta) + ft * ((1 - fp) * c.eTheta + fp * d.eTheta);
        r.ePhi = (1 - ft) * ((1 - fp) * a.ePhi + fp * b.ePhi) + ft * ((1 - fp) * c.ePhi + fp * d.ePhi);
        return r;
    };

    if (frequencies.size() == 1 || frequency <= frequencies.front())
        return bilinear(fields.front());
    if (frequency >= frequencies.back())
        return bilinear(fields.back());
    const auto it = std::upper_bound(frequencies.begin(), frequencies.end(), frequency);
    const std::size_t f1 = std::size_t(it - frequencies.begin());
    const std::size_t f0 = f1 - 1;
    const double w = (frequency - frequencies[f0]) / (frequencies[f1] - frequencies[f0]);
    const Field a = bilinear(fields[f0]);
    const Field b = bilinear(fields[f1]);
    return {(1 - w) * a.eTheta + w * b.eTheta, (1 - w) * a.ePhi + w * b.ePhi};
}

// ---- Names

QString elementModelId(ElementModelKind kind)
{
    for (const auto &m : kModels)
        if (m.kind == kind)
            return QString::fromLatin1(m.id);
    return QStringLiteral("isotropic");
}

QString elementModelName(ElementModelKind kind)
{
    for (const auto &m : kModels)
        if (m.kind == kind)
            return QString::fromLatin1(m.name);
    return {};
}

std::vector<ElementModelKind> elementModelKinds()
{
    std::vector<ElementModelKind> kinds;
    for (const auto &m : kModels)
        kinds.push_back(m.kind);
    return kinds;
}

QString polarisationId(Polarisation polarisation)
{
    for (const auto &p : kPolarisations)
        if (p.polarisation == polarisation)
            return QString::fromLatin1(p.id);
    return QStringLiteral("x");
}

QString polarisationName(Polarisation polarisation)
{
    for (const auto &p : kPolarisations)
        if (p.polarisation == polarisation)
            return QString::fromLatin1(p.name);
    return {};
}

std::vector<Polarisation> polarisations()
{
    std::vector<Polarisation> list;
    for (const auto &p : kPolarisations)
        list.push_back(p.polarisation);
    return list;
}

// ---- ElementSpec

QJsonObject ElementSpec::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("model"), elementModelId(model));
    o.insert(QStringLiteral("polarisation"), polarisationId(polarisation));
    o.insert(QStringLiteral("frequency"), frequency);
    o.insert(QStringLiteral("efficiency"), efficiencyDb);
    switch (model) {
    case ElementModelKind::CosN:
        o.insert(QStringLiteral("exponent"), exponent);
        o.insert(QStringLiteral("backLobe"), backLobeDb);
        break;
    case ElementModelKind::Dipole:
    case ElementModelKind::Monopole:
        o.insert(QStringLiteral("length"), length);
        break;
    case ElementModelKind::DipoleOverGround:
        o.insert(QStringLiteral("length"), length);
        o.insert(QStringLiteral("height"), height);
        break;
    case ElementModelKind::Patch:
        o.insert(QStringLiteral("patchWidth"), patchWidth);
        o.insert(QStringLiteral("patchLength"), patchLength);
        o.insert(QStringLiteral("substrateHeight"), substrateHeight);
        o.insert(QStringLiteral("permittivity"), permittivity);
        o.insert(QStringLiteral("backLobe"), backLobeDb);
        break;
    case ElementModelKind::Aperture:
        o.insert(QStringLiteral("apertureA"), apertureA);
        o.insert(QStringLiteral("apertureB"), apertureB);
        o.insert(QStringLiteral("distribution"), cosineDistribution ? QStringLiteral("cosine") : QStringLiteral("uniform"));
        break;
    case ElementModelKind::Imported:
        o.insert(QStringLiteral("patternFile"), patternFile);
        break;
    case ElementModelKind::Isotropic:
        break;
    }
    return o;
}

ElementSpec ElementSpec::fromJson(const QJsonObject &o)
{
    ElementSpec s;
    const QString modelId = o.value(QStringLiteral("model")).toString();
    for (const auto &m : kModels)
        if (modelId == QLatin1String(m.id))
            s.model = m.kind;
    const QString polId = o.value(QStringLiteral("polarisation")).toString();
    for (const auto &p : kPolarisations)
        if (polId == QLatin1String(p.id))
            s.polarisation = p.polarisation;
    s.frequency = o.value(QStringLiteral("frequency")).toDouble(s.frequency);
    s.efficiencyDb = std::min(0.0, o.value(QStringLiteral("efficiency")).toDouble(s.efficiencyDb));
    s.exponent = std::max(0.0, o.value(QStringLiteral("exponent")).toDouble(s.exponent));
    s.backLobeDb = std::min(0.0, o.value(QStringLiteral("backLobe")).toDouble(s.backLobeDb));
    s.length = std::max(0.0, o.value(QStringLiteral("length")).toDouble(s.length));
    s.height = std::max(0.0, o.value(QStringLiteral("height")).toDouble(s.height));
    s.patchWidth = std::max(0.0, o.value(QStringLiteral("patchWidth")).toDouble(s.patchWidth));
    s.patchLength = std::max(0.0, o.value(QStringLiteral("patchLength")).toDouble(s.patchLength));
    s.substrateHeight = std::max(1e-6, o.value(QStringLiteral("substrateHeight")).toDouble(s.substrateHeight));
    s.permittivity = std::max(1.0, o.value(QStringLiteral("permittivity")).toDouble(s.permittivity));
    s.apertureA = std::max(0.0, o.value(QStringLiteral("apertureA")).toDouble(s.apertureA));
    s.apertureB = std::max(0.0, o.value(QStringLiteral("apertureB")).toDouble(s.apertureB));
    s.cosineDistribution = o.value(QStringLiteral("distribution")).toString(QStringLiteral("cosine")) != QLatin1String("uniform");
    s.patternFile = o.value(QStringLiteral("patternFile")).toString();
    if (s.frequency <= 0)
        s.frequency = 10e9;
    return s;
}

ElementSpec::PatchSize ElementSpec::patchSize() const
{
    PatchSize p;
    const double er = permittivity;
    const double h = substrateHeight;
    const double f = frequency;
    p.width = patchWidth > 0 ? patchWidth : kSpeedOfLight / (2 * f) * std::sqrt(2.0 / (er + 1));
    const double w = p.width;
    p.effectivePermittivity = (er + 1) / 2 + (er - 1) / 2 / std::sqrt(1 + 12 * h / w);
    const double ee = p.effectivePermittivity;
    const double dl = 0.412 * h * (ee + 0.3) * (w / h + 0.264) / ((ee - 0.258) * (w / h + 0.8));
    p.length = patchLength > 0 ? patchLength : kSpeedOfLight / (2 * f * std::sqrt(ee)) - 2 * dl;
    p.effectiveLength = p.length + 2 * dl;
    return p;
}

QString ElementSpec::summary() const
{
    const QString pol = polarisationName(polarisation);
    auto mm = [](double m) { return QString::number(m * 1e3, 'g', 3); };
    switch (model) {
    case ElementModelKind::Isotropic:
        return QStringLiteral("Isotropic, %1").arg(pol);
    case ElementModelKind::CosN:
        return QStringLiteral("cos^%1, %2").arg(QString::number(exponent, 'g', 3), pol);
    case ElementModelKind::Dipole:
        return QStringLiteral("Dipole %1 mm, %2").arg(mm(length > 0 ? length : wavelength(frequency) / 2), pol);
    case ElementModelKind::DipoleOverGround:
        return QStringLiteral("Dipole %1 mm at %2 mm over ground, %3")
            .arg(mm(length > 0 ? length : wavelength(frequency) / 2), mm(height > 0 ? height : wavelength(frequency) / 4), pol);
    case ElementModelKind::Monopole:
        return QStringLiteral("Monopole %1 mm").arg(mm(length > 0 ? length : wavelength(frequency) / 4));
    case ElementModelKind::Patch: {
        const PatchSize p = patchSize();
        return QStringLiteral("Patch %1 x %2 mm, er %3, %4")
            .arg(mm(p.width), mm(p.length), QString::number(permittivity, 'g', 3), pol);
    }
    case ElementModelKind::Aperture:
        return QStringLiteral("Aperture %1 x %2 mm, %3")
            .arg(mm(apertureA > 0 ? apertureA : 0.72 * wavelength(frequency)),
                 mm(apertureB > 0 ? apertureB : 0.34 * wavelength(frequency)), pol);
    case ElementModelKind::Imported:
        return QStringLiteral("Imported: %1").arg(patternFile.isEmpty() ? QStringLiteral("(no file)") : patternFile);
    }
    return {};
}

// ---- ElementPattern

ElementPattern::ElementPattern(ElementSpec spec) : m_spec(std::move(spec)) {}

Field ElementPattern::rawX(double theta, double phi, double frequency) const
{
    const ElementSpec &s = m_spec;
    const double k = 2 * kPi * frequency / kSpeedOfLight;
    const double lambda0 = wavelength(s.frequency);
    const double st = std::sin(theta), ct = std::cos(theta);
    const double sp = std::sin(phi), cp = std::cos(phi);
    const bool front = theta < kPi / 2;
    const double floorAmp = amplitudeFromDb(s.backLobeDb);

    switch (s.model) {
    case ElementModelKind::Isotropic:
        return ludwigX(1.0, phi);
    case ElementModelKind::CosN: {
        const double power = front ? std::max(std::pow(ct, s.exponent), floorAmp * floorAmp) : floorAmp * floorAmp;
        return ludwigX(std::sqrt(power), phi);
    }
    case ElementModelKind::Dipole:
    case ElementModelKind::DipoleOverGround: {
        // Current along x; psi is the angle from the dipole axis.
        const double len = s.length > 0 ? s.length : lambda0 / 2;
        const double a = k * len / 2;
        const double cpsi = st * cp;
        const double s2 = 1 - cpsi * cpsi;
        double g;
        if (a < 0.05)
            g = 1.0; // short dipole
        else if (s2 < 1e-9)
            g = a * std::sin(a) / 2;
        else
            g = (std::cos(a * cpsi) - std::cos(a)) / s2;
        Field f{g * ct * cp, -g * sp};
        if (s.model == ElementModelKind::DipoleOverGround) {
            if (!front)
                return {};
            const double h = s.height > 0 ? s.height : lambda0 / 4;
            const cplx ground = cplx(0, 2) * std::sin(k * h * ct);
            f = f * ground;
        }
        return f;
    }
    case ElementModelKind::Monopole: {
        // The horizon (theta = 90 deg) is the monopole's maximum: include it.
        if (theta > kPi / 2 + 1e-9 || st < 1e-9)
            return {};
        const double len = s.length > 0 ? s.length : lambda0 / 4;
        const double b = k * len;
        return {(std::cos(b * ct) - std::cos(b)) / st, 0.0};
    }
    case ElementModelKind::Patch: {
        if (!front)
            return ludwigX(floorAmp, phi);
        const ElementSpec::PatchSize p = s.patchSize();
        const double x = k * s.substrateHeight / 2 * st * cp;
        const double y = k * p.width / 2 * st * sp;
        const double z = k * p.effectiveLength / 2 * st * cp;
        const double f = sinc(x) * sinc(y) * std::cos(z);
        return {f * cp, -f * ct * sp};
    }
    case ElementModelKind::Aperture: {
        const double a = s.apertureA > 0 ? s.apertureA : 0.72 * lambda0;
        const double b = s.apertureB > 0 ? s.apertureB : 0.34 * lambda0;
        const double fx = sinc(k * b / 2 * st * cp);
        const double y = k * a / 2 * st * sp;
        const double fy = s.cosineDistribution ? cosineAperture(y) : sinc(y);
        const double huygens = (1 + ct) / 2;
        const double f = huygens * fx * fy;
        return {f * cp, -f * sp};
    }
    case ElementModelKind::Imported:
        if (s.pattern)
            return s.pattern->sample(toDegrees(theta), toDegrees(phi), frequency);
        return {};
    }
    return {};
}

Field ElementPattern::shape(double theta, double phi, double frequency) const
{
    if (m_spec.model == ElementModelKind::Monopole || m_spec.model == ElementModelKind::Imported)
        return rawX(theta, phi, frequency);
    switch (m_spec.polarisation) {
    case Polarisation::LinearX:
        return rawX(theta, phi, frequency);
    case Polarisation::LinearY:
        return rawX(theta, phi - kPi / 2, frequency);
    case Polarisation::Slant45:
        return rawX(theta, phi - kPi / 4, frequency);
    case Polarisation::SlantMinus45:
        return rawX(theta, phi + kPi / 4, frequency);
    case Polarisation::Rhcp:
    case Polarisation::Lhcp: {
        // Two orthogonal copies fed in quadrature: (x -+ j y) / sqrt 2.
        const Field x = rawX(theta, phi, frequency);
        const Field y = rawX(theta, phi - kPi / 2, frequency);
        const cplx j = m_spec.polarisation == Polarisation::Rhcp ? cplx(0, -1) : cplx(0, 1);
        return {(x.eTheta + j * y.eTheta) / std::numbers::sqrt2, (x.ePhi + j * y.ePhi) / std::numbers::sqrt2};
    }
    }
    return rawX(theta, phi, frequency);
}

double ElementPattern::normalisation(double frequency) const
{
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_scale.find(frequency); it != m_scale.end())
            return it->second;
    }
    // Midpoint rule over the sphere, half-degree cells.
    constexpr int nt = 360, np = 720;
    const double dt = kPi / nt, dp = 2 * kPi / np;
    double total = 0;
    for (int i = 0; i < nt; ++i) {
        const double theta = (i + 0.5) * dt;
        const double weight = std::sin(theta) * dt * dp;
        double row = 0;
        for (int j = 0; j < np; ++j)
            row += shape(theta, -kPi + (j + 0.5) * dp, frequency).power();
        total += row * weight;
    }
    const double scale = total > 0 ? std::sqrt(4 * kPi / total) : 0.0;
    std::lock_guard lock(m_mutex);
    m_scale[frequency] = scale;
    return scale;
}

double ElementPattern::peakDirectivityDbi(double frequency, double *thetaDeg, double *phiDeg) const
{
    const double scale = normalisation(frequency);
    double best = -1, bt = 0, bp = 0;
    for (int i = 0; i <= 360; ++i) {
        const double theta = toRadians(i * 0.5);
        for (int j = 0; j < 720; ++j) {
            const double phi = toRadians(-180.0 + j * 0.5);
            const double p = shape(theta, phi, frequency).power();
            if (p > best) {
                best = p;
                bt = i * 0.5;
                bp = -180.0 + j * 0.5;
            }
            if (i == 0 || i == 360)
                break; // the poles have one direction
        }
    }
    if (thetaDeg)
        *thetaDeg = bt;
    if (phiDeg)
        *phiDeg = bp;
    return powerDb(best * scale * scale);
}

} // namespace lvd::ant

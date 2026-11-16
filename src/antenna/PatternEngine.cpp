#include "antenna/PatternEngine.h"

#include <algorithm>
#include <thread>

namespace lvd::ant {

namespace {

struct NamedComponent {
    PatternComponent component;
    const char *id;
    const char *name;
};

constexpr NamedComponent kComponents[] = {
    {PatternComponent::Total, "total", "Total"},
    {PatternComponent::Co, "co", "Co-polar (Ludwig 3)"},
    {PatternComponent::Cross, "cross", "Cross-polar (Ludwig 3)"},
    {PatternComponent::Rhcp, "rhcp", "RHCP"},
    {PatternComponent::Lhcp, "lhcp", "LHCP"},
    {PatternComponent::Theta, "theta", "E theta"},
    {PatternComponent::Phi, "phi", "E phi"},
};

// Orthonormal tangents at a direction: along theta_hat and phi_hat, or the
// UV axes at the poles.
void tangents(const Vec3 &d, const Mat3 &frame, Vec3 *e1, Vec3 *e2)
{
    double theta, phi;
    directionAngles(d, &theta, &phi);
    if (theta < 1e-6 || theta > kPi - 1e-6) {
        *e1 = frame.col(0);
        *e2 = frame.col(1);
    } else {
        *e1 = thetaUnit(theta, phi);
        *e2 = phiUnit(phi);
    }
}

double angleBetween(const Vec3 &a, const Vec3 &b)
{
    return std::atan2(a.cross(b).norm(), a.dot(b));
}

} // namespace

QString patternComponentId(PatternComponent component)
{
    for (const auto &c : kComponents)
        if (c.component == component)
            return QString::fromLatin1(c.id);
    return QStringLiteral("total");
}

QString patternComponentName(PatternComponent component)
{
    for (const auto &c : kComponents)
        if (c.component == component)
            return QString::fromLatin1(c.name);
    return {};
}

std::vector<PatternComponent> patternComponents()
{
    std::vector<PatternComponent> list;
    for (const auto &c : kComponents)
        list.push_back(c.component);
    return list;
}

double componentPower(const Field &f, double phi, PatternComponent component, CoPolarisation co)
{
    switch (component) {
    case PatternComponent::Total:
        return f.power();
    case PatternComponent::Co:
        return std::norm(co == CoPolarisation::X ? ludwig3X(f, phi) : ludwig3Y(f, phi));
    case PatternComponent::Cross:
        return std::norm(co == CoPolarisation::X ? ludwig3Y(f, phi) : ludwig3X(f, phi));
    case PatternComponent::Rhcp:
        return std::norm(rhcp(f));
    case PatternComponent::Lhcp:
        return std::norm(lhcp(f));
    case PatternComponent::Theta:
        return std::norm(f.eTheta);
    case PatternComponent::Phi:
        return std::norm(f.ePhi);
    }
    return f.power();
}

void parallelFor(std::size_t n, const std::function<void(std::size_t)> &fn, const std::atomic_bool *cancel)
{
    if (n == 0)
        return;
    const std::size_t threads = std::min<std::size_t>(std::max(1u, std::thread::hardware_concurrency()), n);
    if (threads <= 1 || n < 4) {
        for (std::size_t i = 0; i < n; ++i) {
            if (cancel && cancel->load())
                return;
            fn(i);
        }
        return;
    }
    std::atomic_size_t next{0};
    auto worker = [&] {
        while (true) {
            if (cancel && cancel->load())
                return;
            const std::size_t i = next.fetch_add(1);
            if (i >= n)
                return;
            fn(i);
        }
    };
    std::vector<std::thread> pool;
    for (std::size_t t = 1; t < threads; ++t)
        pool.emplace_back(worker);
    worker();
    for (std::thread &t : pool)
        t.join();
}

// ---- PatternEngine

PatternEngine::PatternEngine(const ArrayLayout &layout, std::vector<cplx> weights,
                             std::shared_ptr<const ElementPattern> element, double frequency)
    : m_weights(std::move(weights)), m_element(std::move(element)), m_frequency(frequency)
{
    for (const ArrayElement &e : layout.elements) {
        m_positions.push_back(e.position);
        m_orientations.push_back(e.orientation);
    }
    m_weights.resize(m_positions.size(), 0.0);
    m_identical = layout.identicalOrientation;
    m_k = 2 * kPi * frequency / kSpeedOfLight;
    m_scale = m_element->normalisation(frequency);
    m_weightPower = 0;
    for (const cplx &w : m_weights)
        m_weightPower += std::norm(w);
    m_norm = m_weightPower > 0 ? std::sqrt(m_element->efficiency() / m_weightPower) : 0.0;

    Vec3 centre = Vec3::Zero();
    for (const Vec3 &p : m_positions)
        centre += p;
    if (!m_positions.empty())
        centre /= double(m_positions.size());
    for (const Vec3 &p : m_positions)
        m_extent = std::max(m_extent, (p - centre).norm());

    if (layout.kind == LatticeKind::Cylinder) {
        m_uvFrame.col(0) = Vec3(0, 1, 0);
        m_uvFrame.col(1) = Vec3(0, 0, 1);
        m_uvFrame.col(2) = Vec3(1, 0, 0);
    }
}

Field PatternEngine::sum(const Vec3 &r, double theta, double phi, bool normalised) const
{
    const double scale = m_scale * (normalised ? m_norm : std::sqrt(m_element->efficiency()));
    const std::size_t n = m_positions.size();
    if (m_identical) {
        cplx af = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (m_weights[i] == 0.0)
                continue;
            af += m_weights[i] * std::polar(1.0, m_k * r.dot(m_positions[i]));
        }
        return m_element->shape(theta, phi, m_frequency) * (af * scale);
    }
    // Rotated elements: sum Cartesian field vectors.
    Eigen::Vector3cd total = Eigen::Vector3cd::Zero();
    for (std::size_t i = 0; i < n; ++i) {
        if (m_weights[i] == 0.0)
            continue;
        const Mat3 &rot = m_orientations[i];
        const Vec3 local = rot.transpose() * r;
        double lt, lp;
        directionAngles(local, &lt, &lp);
        const Field f = m_element->shape(lt, lp, m_frequency);
        const Vec3 th = rot * thetaUnit(lt, lp);
        const Vec3 ph = rot * phiUnit(lp);
        const cplx c = m_weights[i] * std::polar(1.0, m_k * r.dot(m_positions[i]));
        total += (th.cast<cplx>() * f.eTheta + ph.cast<cplx>() * f.ePhi) * c;
    }
    const Vec3 th = thetaUnit(theta, phi);
    const Vec3 ph = phiUnit(phi);
    return {total.dot(th.cast<cplx>()) * scale, total.dot(ph.cast<cplx>()) * scale};
}

Field PatternEngine::field(double theta, double phi) const
{
    return sum(radialUnit(theta, phi), theta, phi, true);
}

Field PatternEngine::rawField(double theta, double phi) const
{
    return sum(radialUnit(theta, phi), theta, phi, false);
}

Field PatternEngine::fieldAt(const Vec3 &direction) const
{
    double theta, phi;
    directionAngles(direction, &theta, &phi);
    return sum(radialUnit(theta, phi), theta, phi, true);
}

Vec3 PatternEngine::uvDirection(double u, double v) const
{
    const double w = std::sqrt(std::max(0.0, 1 - u * u - v * v));
    return u * m_uvFrame.col(0) + v * m_uvFrame.col(1) + w * m_uvFrame.col(2);
}

double PatternEngine::estimatedBeamwidth() const
{
    const double lambda = wavelength(m_frequency);
    const double aperture = 2 * m_extent + lambda / 2;
    return std::clamp(0.886 * lambda / aperture, 1e-4, kPi);
}

double PatternEngine::integratedPower(const std::atomic_bool *cancel) const
{
    const double stepDeg = std::clamp(toDegrees(estimatedBeamwidth()) / 5, 0.1, 1.0);
    const int nt = int(std::ceil(180.0 / stepDeg));
    const int np = 2 * nt;
    const double dt = kPi / nt, dp = 2 * kPi / np;
    std::vector<double> rows(std::size_t(nt), 0.0);
    parallelFor(std::size_t(nt), [&](std::size_t i) {
        const double theta = (double(i) + 0.5) * dt;
        double row = 0;
        for (int j = 0; j < np; ++j)
            row += field(theta, -kPi + (j + 0.5) * dp).power();
        rows[i] = row * std::sin(theta) * dt * dp;
    }, cancel);
    if (cancel && cancel->load())
        return NAN;
    double total = 0;
    for (double r : rows)
        total += r;
    const double eta = m_element->efficiency();
    return total / (4 * kPi) / (eta > 0 ? eta : 1.0);
}

// ---- Sampled patterns

PatternCut computePhiCut(const PatternEngine &engine, double phiDeg, double stepDeg, const std::atomic_bool *cancel)
{
    PatternCut cut;
    cut.name = QStringLiteral("phi = %1 deg").arg(phiDeg);
    const int n = int(std::round(360.0 / stepDeg));
    cut.angle.resize(std::size_t(n) + 1);
    cut.theta.resize(cut.angle.size());
    cut.phi.resize(cut.angle.size());
    cut.fields.resize(cut.angle.size());
    parallelFor(cut.angle.size(), [&](std::size_t i) {
        const double a = -180.0 + 360.0 * double(i) / n;
        const double theta = std::abs(a);
        const double phi = a < 0 ? phiDeg + 180.0 : phiDeg;
        cut.angle[i] = a;
        cut.theta[i] = theta;
        cut.phi[i] = phi;
        cut.fields[i] = engine.field(toRadians(theta), toRadians(phi));
    }, cancel);
    return cut;
}

PatternCut computeThetaCut(const PatternEngine &engine, double thetaDeg, double stepDeg, const std::atomic_bool *cancel)
{
    PatternCut cut;
    cut.name = QStringLiteral("theta = %1 deg").arg(thetaDeg);
    const int n = int(std::round(360.0 / stepDeg));
    cut.angle.resize(std::size_t(n) + 1);
    cut.theta.assign(cut.angle.size(), thetaDeg);
    cut.phi.resize(cut.angle.size());
    cut.fields.resize(cut.angle.size());
    parallelFor(cut.angle.size(), [&](std::size_t i) {
        const double phi = -180.0 + 360.0 * double(i) / n;
        cut.angle[i] = phi;
        cut.phi[i] = phi;
        cut.fields[i] = engine.field(toRadians(thetaDeg), toRadians(phi));
    }, cancel);
    return cut;
}

PatternCut computeGreatCircleCut(const PatternEngine &engine, const Vec3 &direction, const Vec3 &towards, double spanDeg,
                                 double stepDeg, const std::atomic_bool *cancel)
{
    PatternCut cut;
    const Vec3 d = direction.normalized();
    const Vec3 t = (towards - towards.dot(d) * d).normalized();
    const int n = int(std::round(2 * spanDeg / stepDeg));
    cut.angle.resize(std::size_t(n) + 1);
    cut.theta.resize(cut.angle.size());
    cut.phi.resize(cut.angle.size());
    cut.fields.resize(cut.angle.size());
    parallelFor(cut.angle.size(), [&](std::size_t i) {
        const double a = -spanDeg + 2 * spanDeg * double(i) / n;
        const Vec3 r = std::cos(toRadians(a)) * d + std::sin(toRadians(a)) * t;
        double theta, phi;
        directionAngles(r, &theta, &phi);
        cut.angle[i] = a;
        cut.theta[i] = toDegrees(theta);
        cut.phi[i] = toDegrees(phi);
        cut.fields[i] = engine.field(theta, phi);
    }, cancel);
    return cut;
}

SphericalGrid computeSphericalGrid(const PatternEngine &engine, double stepDeg, const std::atomic_bool *cancel)
{
    SphericalGrid grid;
    const int nt = int(std::round(180.0 / stepDeg));
    const int np = int(std::round(360.0 / stepDeg));
    for (int i = 0; i <= nt; ++i)
        grid.theta.push_back(180.0 * i / nt);
    for (int j = 0; j <= np; ++j)
        grid.phi.push_back(-180.0 + 360.0 * j / np);
    grid.fields.resize(grid.theta.size() * grid.phi.size());
    parallelFor(grid.theta.size(), [&](std::size_t i) {
        const double theta = toRadians(grid.theta[i]);
        for (std::size_t j = 0; j < grid.phi.size(); ++j)
            grid.fields[i * grid.phi.size() + j] = engine.field(theta, toRadians(grid.phi[j]));
    }, cancel);
    return grid;
}

UvGrid computeUvGrid(const PatternEngine &engine, int size, const std::atomic_bool *cancel)
{
    UvGrid grid;
    grid.size = std::max(size, 2);
    for (int i = 0; i < grid.size; ++i)
        grid.axis.push_back(-1.0 + 2.0 * i / (grid.size - 1));
    const std::size_t total = std::size_t(grid.size) * std::size_t(grid.size);
    grid.fields.resize(total);
    grid.phi.assign(total, 0.0);
    grid.valid.assign(total, false);
    parallelFor(std::size_t(grid.size), [&](std::size_t iv) {
        const double v = grid.axis[iv];
        for (std::size_t iu = 0; iu < std::size_t(grid.size); ++iu) {
            const double u = grid.axis[iu];
            const std::size_t k = iv * std::size_t(grid.size) + iu;
            if (u * u + v * v > 1.0)
                continue;
            double theta, phi;
            directionAngles(engine.uvDirection(u, v), &theta, &phi);
            grid.fields[k] = engine.field(theta, phi);
            grid.phi[k] = phi;
            grid.valid[k] = true;
        }
    }, cancel);
    return grid;
}

// ---- Beam metrics

CutShape analyseCut(const std::vector<double> &angle, const std::vector<double> &db, std::size_t centre)
{
    CutShape shape;
    const std::size_t n = db.size();
    if (n < 3 || centre >= n)
        return shape;
    const double peak = db[centre];
    auto crossing = [&](std::size_t inside, std::size_t outside) {
        const double a = db[inside] - (peak - 3.0), b = db[outside] - (peak - 3.0);
        const double t = a == b ? 0.0 : a / (a - b);
        return angle[inside] + t * (angle[outside] - angle[inside]);
    };
    // Half-power points.
    std::optional<double> left, right;
    for (std::size_t i = centre; i > 0; --i)
        if (db[i - 1] < peak - 3.0) {
            left = crossing(i, i - 1);
            break;
        }
    for (std::size_t i = centre; i + 1 < n; ++i)
        if (db[i + 1] < peak - 3.0) {
            right = crossing(i, i + 1);
            break;
        }
    if (left && right)
        shape.beamwidth = *right - *left;

    // First nulls: the first local minimum on each side.
    std::optional<std::size_t> leftNull, rightNull;
    for (std::size_t i = centre; i > 0; --i)
        if (db[i - 1] > db[i] + 1e-9 && db[i] < peak - 3.0) {
            leftNull = i;
            break;
        }
    for (std::size_t i = centre; i + 1 < n; ++i)
        if (db[i + 1] > db[i] + 1e-9 && db[i] < peak - 3.0) {
            rightNull = i;
            break;
        }
    if (leftNull && rightNull)
        shape.nullWidth = angle[*rightNull] - angle[*leftNull];
    double sll = -INFINITY;
    if (leftNull)
        for (std::size_t i = 0; i < *leftNull; ++i)
            sll = std::max(sll, db[i]);
    if (rightNull)
        for (std::size_t i = *rightNull + 1; i < n; ++i)
            sll = std::max(sll, db[i]);
    if (std::isfinite(sll))
        shape.sideLobeDb = sll - peak;
    return shape;
}

BeamMetrics computeMetrics(const PatternEngine &engine, const MetricsOptions &options, const std::atomic_bool *cancel)
{
    BeamMetrics m;
    if (engine.weightPower() <= 0)
        return m;
    const double bw = engine.estimatedBeamwidth();
    const Vec3 steer = radialUnit(toRadians(options.steerThetaDeg), toRadians(options.steerPhiDeg));
    auto power = [&](const Vec3 &r) { return engine.fieldAt(r).power(); };

    // Candidates: the steering direction and the largest point of a coarse grid.
    std::vector<Vec3> candidates{steer};
    if (options.uv && options.uv->size > 0) {
        double best = -1;
        Vec3 bestDir = steer;
        const UvGrid &uv = *options.uv;
        for (int iv = 0; iv < uv.size; ++iv)
            for (int iu = 0; iu < uv.size; ++iu) {
                const std::size_t k = std::size_t(iv) * std::size_t(uv.size) + std::size_t(iu);
                if (uv.valid[k] && uv.fields[k].power() > best) {
                    best = uv.fields[k].power();
                    bestDir = engine.uvDirection(uv.axis[std::size_t(iu)], uv.axis[std::size_t(iv)]);
                }
            }
        candidates.push_back(bestDir);
    } else {
        const double step = std::clamp(toDegrees(bw) / 2, 1.0, 3.0);
        const SphericalGrid grid = computeSphericalGrid(engine, step, cancel);
        double best = -1;
        Vec3 bestDir = steer;
        for (std::size_t i = 0; i < grid.theta.size(); ++i)
            for (std::size_t j = 0; j < grid.phi.size(); ++j) {
                const double p = grid.fields[i * grid.phi.size() + j].power();
                if (p > best) {
                    best = p;
                    bestDir = radialUnit(toRadians(grid.theta[i]), toRadians(grid.phi[j]));
                }
            }
        candidates.push_back(bestDir);
    }
    if (cancel && cancel->load())
        return m;

    // Pattern search in the tangent plane of each candidate.
    Vec3 peakDir = steer;
    double peakPower = -1;
    for (const Vec3 &start : candidates) {
        double p = 0;
        const Vec3 d = findPeakNear(engine, start, &p);
        // Ties (the cone of a line array) keep the candidate nearest the steering direction.
        if (p > peakPower * (1 + 1e-4)) {
            peakPower = p;
            peakDir = d;
        }
    }
    double pt, pp;
    directionAngles(peakDir, &pt, &pp);
    m.peakGainDbi = powerDb(peakPower);
    m.peakThetaDeg = toDegrees(pt);
    m.peakPhiDeg = toDegrees(pp);
    m.gainAtSteerDbi = powerDb(power(steer));
    m.pointingErrorDeg = toDegrees(angleBetween(peakDir, steer));

    const Field peakField = engine.fieldAt(peakDir);
    const double co = componentPower(peakField, pp, PatternComponent::Co, options.co);
    const double cross = componentPower(peakField, pp, PatternComponent::Cross, options.co);
    m.crossPolDb = co > 0 ? powerDb(cross / co) : NAN;
    m.axialRatioDb = axialRatioDb(peakField);

    // Principal cuts through the peak.
    Vec3 e1, e2;
    tangents(peakDir, engine.uvFrame(), &e1, &e2);
    const double step = std::clamp(toDegrees(bw) / 40, 0.005, 0.25);
    auto analyse = [&](const Vec3 &towards, PatternCut &cut, double *beamwidth, double *nullWidth, double *sll) {
        cut = computeGreatCircleCut(engine, peakDir, towards, 90.0, step, cancel);
        std::vector<double> db(cut.fields.size());
        for (std::size_t i = 0; i < db.size(); ++i)
            db[i] = powerDb(cut.fields[i].power());
        const std::size_t centre = cut.fields.size() / 2;
        const CutShape shape = analyseCut(cut.angle, db, centre);
        *beamwidth = shape.beamwidth;
        *nullWidth = shape.nullWidth;
        *sll = shape.sideLobeDb;
    };
    analyse(e1, m.cut1, &m.beamwidth1Deg, &m.nullWidth1Deg, &m.sideLobe1Db);
    m.cut1.name = QStringLiteral("Beam cut 1");
    analyse(e2, m.cut2, &m.beamwidth2Deg, &m.nullWidth2Deg, &m.sideLobe2Db);
    m.cut2.name = QStringLiteral("Beam cut 2");
    if (cancel && cancel->load())
        return m;

    // Side lobes over the UV plane, outside the main-lobe ellipse.
    if (options.uv) {
        const UvGrid &uv = *options.uv;
        auto halfWidth = [&](double nullWidth, double beamwidth) {
            if (std::isfinite(nullWidth))
                return toRadians(nullWidth) / 2;
            return std::isfinite(beamwidth) ? toRadians(beamwidth) * 1.25 : bw * 1.25;
        };
        const double h1 = halfWidth(m.nullWidth1Deg, m.beamwidth1Deg);
        const double h2 = halfWidth(m.nullWidth2Deg, m.beamwidth2Deg);
        double sll = -INFINITY;
        for (int iv = 0; iv < uv.size; ++iv)
            for (int iu = 0; iu < uv.size; ++iu) {
                const std::size_t k = std::size_t(iv) * std::size_t(uv.size) + std::size_t(iu);
                if (!uv.valid[k])
                    continue;
                const Vec3 r = engine.uvDirection(uv.axis[std::size_t(iu)], uv.axis[std::size_t(iv)]);
                const double along = r.dot(peakDir);
                if (along > 0) {
                    const double a1 = std::atan2(r.dot(e1), along);
                    const double a2 = std::atan2(r.dot(e2), along);
                    if (std::pow(a1 / h1, 2) + std::pow(a2 / h2, 2) < 1.0)
                        continue;
                }
                sll = std::max(sll, uv.fields[k].power());
            }
        if (std::isfinite(sll) && sll > 0)
            m.sideLobeDb = powerDb(sll) - m.peakGainDbi;
    }

    if (options.integrate) {
        const double p = engine.integratedPower(cancel);
        if (std::isfinite(p) && p > 0)
            m.directivityDbi = m.peakGainDbi - powerDb(engine.element().efficiency() * p);
    }
    return m;
}

Vec3 findPeakNear(const PatternEngine &engine, const Vec3 &start, double *gain)
{
    const double bw = engine.estimatedBeamwidth();
    Vec3 d = start.normalized();
    double p = engine.fieldAt(d).power();
    double step = bw / 4;
    while (step > bw / 4000) {
        bool moved = false;
        Vec3 e1, e2;
        tangents(d, engine.uvFrame(), &e1, &e2);
        for (const auto &[a, b] : {std::pair{1.0, 0.0}, {-1.0, 0.0}, {0.0, 1.0}, {0.0, -1.0}}) {
            const Vec3 trial = (d + step * (a * e1 + b * e2)).normalized();
            const double q = engine.fieldAt(trial).power();
            if (q > p) {
                p = q;
                d = trial;
                moved = true;
            }
        }
        if (!moved)
            step /= 2;
    }
    if (gain)
        *gain = p;
    return d;
}

// ---- Grating lobes

GratingLobeInfo gratingLobes(const Lattice &lattice, double frequency, int order)
{
    GratingLobeInfo info;
    const double lambda = wavelength(frequency);
    if (lattice.basis.size() == 1) {
        info.line = true;
        const Eigen::Vector2d a = lattice.basis[0];
        const Eigen::Vector2d b = a / a.squaredNorm();
        for (int m = -order; m <= order; ++m)
            if (m != 0)
                info.centres.push_back(lambda * m * b);
        return info;
    }
    if (lattice.basis.size() < 2)
        return info;
    Eigen::Matrix2d a;
    a.col(0) = lattice.basis[0];
    a.col(1) = lattice.basis[1];
    if (std::abs(a.determinant()) < 1e-18)
        return info;
    const Eigen::Matrix2d b = a.inverse().transpose();
    for (int m = -order; m <= order; ++m)
        for (int n = -order; n <= order; ++n)
            if (m != 0 || n != 0)
                info.centres.push_back(lambda * (m * b.col(0) + n * b.col(1)));
    return info;
}

std::vector<Eigen::Vector2d> visibleGratingLobes(const GratingLobeInfo &info, double u0, double v0)
{
    std::vector<Eigen::Vector2d> visible;
    for (const Eigen::Vector2d &c : info.centres) {
        const Eigen::Vector2d p(u0 + c.x(), v0 + c.y());
        // A lobe exactly at endfire (|p| = 1) is not counted.
        const bool inside = info.line ? std::abs(p.x()) < 1.0 - 1e-9 : p.norm() < 1.0 - 1e-9;
        if (inside)
            visible.push_back(p);
    }
    return visible;
}

double gratingLobeFreeScan(const Lattice &lattice, double frequency, double phiDeg)
{
    const GratingLobeInfo info = gratingLobes(lattice, frequency, 3);
    const double cp = std::cos(toRadians(phiDeg)), sp = std::sin(toRadians(phiDeg));
    constexpr int steps = 9000;
    for (int i = 0; i <= steps; ++i) {
        const double s = double(i) / steps;
        if (!visibleGratingLobes(info, s * cp, s * sp).empty())
            return i == 0 ? 0.0 : toDegrees(std::asin(double(i - 1) / steps));
    }
    return 90.0;
}

} // namespace lvd::ant

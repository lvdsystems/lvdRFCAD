#include "antenna/ArrayGeometry.h"

#include <QJsonArray>

namespace lvd::ant {

namespace {

struct NamedLattice {
    LatticeKind kind;
    const char *id;
    const char *name;
};

constexpr NamedLattice kLattices[] = {
    {LatticeKind::Linear, "linear", "Linear"},
    {LatticeKind::Rectangular, "rectangular", "Planar, rectangular lattice"},
    {LatticeKind::Triangular, "triangular", "Planar, triangular lattice"},
    {LatticeKind::Circular, "circular", "Circular rings"},
    {LatticeKind::Cylinder, "cylinder", "Conformal cylinder"},
    {LatticeKind::Custom, "custom", "Custom table"},
};

double number(const QJsonObject &o, const char *key, double fallback)
{
    return o.value(QLatin1String(key)).toDouble(fallback);
}

} // namespace

QString latticeId(LatticeKind kind)
{
    for (const auto &l : kLattices)
        if (l.kind == kind)
            return QString::fromLatin1(l.id);
    return QStringLiteral("rectangular");
}

QString latticeName(LatticeKind kind)
{
    for (const auto &l : kLattices)
        if (l.kind == kind)
            return QString::fromLatin1(l.name);
    return {};
}

std::vector<LatticeKind> latticeKinds()
{
    std::vector<LatticeKind> kinds;
    for (const auto &l : kLattices)
        kinds.push_back(l.kind);
    return kinds;
}

QJsonObject GeometrySpec::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), latticeId(kind));
    o.insert(QStringLiteral("unit"), unit);
    switch (kind) {
    case LatticeKind::Linear:
        o.insert(QStringLiteral("nx"), nx);
        o.insert(QStringLiteral("dx"), dx);
        break;
    case LatticeKind::Rectangular:
    case LatticeKind::Triangular:
        o.insert(QStringLiteral("nx"), nx);
        o.insert(QStringLiteral("ny"), ny);
        o.insert(QStringLiteral("dx"), dx);
        o.insert(QStringLiteral("dy"), dy);
        o.insert(QStringLiteral("aperture"), aperture);
        o.insert(QStringLiteral("apertureX"), apertureX);
        o.insert(QStringLiteral("apertureY"), apertureY);
        break;
    case LatticeKind::Circular: {
        QJsonArray list;
        for (const RingSpec &r : rings)
            list.append(QJsonObject{{QStringLiteral("radius"), r.radius},
                                    {QStringLiteral("count"), r.count},
                                    {QStringLiteral("rotation"), r.rotationDeg}});
        o.insert(QStringLiteral("rings"), list);
        o.insert(QStringLiteral("centreElement"), centreElement);
        break;
    }
    case LatticeKind::Cylinder:
        o.insert(QStringLiteral("cylinderRadius"), cylinderRadius);
        o.insert(QStringLiteral("columns"), columns);
        o.insert(QStringLiteral("rows"), rows);
        o.insert(QStringLiteral("arc"), arcDeg);
        o.insert(QStringLiteral("dz"), dz);
        break;
    case LatticeKind::Custom: {
        QJsonArray list;
        for (const CustomElement &e : custom) {
            QJsonObject item{{QStringLiteral("x"), e.x}, {QStringLiteral("y"), e.y}, {QStringLiteral("z"), e.z}};
            if (e.nx != 0 || e.ny != 0 || e.nz != 1) {
                item.insert(QStringLiteral("nx"), e.nx);
                item.insert(QStringLiteral("ny"), e.ny);
                item.insert(QStringLiteral("nz"), e.nz);
            }
            if (e.rotationDeg != 0)
                item.insert(QStringLiteral("rotation"), e.rotationDeg);
            if (e.amplitudeDb != 0)
                item.insert(QStringLiteral("amplitude"), e.amplitudeDb);
            if (e.phaseDeg != 0)
                item.insert(QStringLiteral("phase"), e.phaseDeg);
            if (!e.enabled)
                item.insert(QStringLiteral("enabled"), false);
            list.append(item);
        }
        o.insert(QStringLiteral("elements"), list);
        break;
    }
    }
    return o;
}

GeometrySpec GeometrySpec::fromJson(const QJsonObject &o)
{
    GeometrySpec s;
    const QString id = o.value(QStringLiteral("kind")).toString();
    for (const auto &l : kLattices)
        if (id == QLatin1String(l.id))
            s.kind = l.kind;
    s.unit = o.value(QStringLiteral("unit")).toString(s.unit) == QLatin1String("m") ? QStringLiteral("m") : QStringLiteral("lambda");
    s.nx = std::max(1, o.value(QStringLiteral("nx")).toInt(s.nx));
    s.ny = std::max(1, o.value(QStringLiteral("ny")).toInt(s.ny));
    s.dx = std::max(1e-9, number(o, "dx", s.dx));
    s.dy = std::max(1e-9, number(o, "dy", s.dy));
    s.aperture = o.value(QStringLiteral("aperture")).toString(s.aperture);
    s.apertureX = std::max(0.0, number(o, "apertureX", 0));
    s.apertureY = std::max(0.0, number(o, "apertureY", 0));
    if (o.contains(QStringLiteral("rings"))) {
        s.rings.clear();
        for (const QJsonValue &v : o.value(QStringLiteral("rings")).toArray()) {
            const QJsonObject r = v.toObject();
            s.rings.push_back({std::max(0.0, number(r, "radius", 1)), std::max(1, r.value(QStringLiteral("count")).toInt(8)),
                               number(r, "rotation", 0)});
        }
    }
    s.centreElement = o.value(QStringLiteral("centreElement")).toBool(false);
    s.cylinderRadius = std::max(1e-9, number(o, "cylinderRadius", s.cylinderRadius));
    s.columns = std::max(1, o.value(QStringLiteral("columns")).toInt(s.columns));
    s.rows = std::max(1, o.value(QStringLiteral("rows")).toInt(s.rows));
    s.arcDeg = std::clamp(number(o, "arc", s.arcDeg), 0.0, 360.0);
    s.dz = std::max(1e-9, number(o, "dz", s.dz));
    for (const QJsonValue &v : o.value(QStringLiteral("elements")).toArray()) {
        const QJsonObject e = v.toObject();
        CustomElement c;
        c.x = number(e, "x", 0);
        c.y = number(e, "y", 0);
        c.z = number(e, "z", 0);
        c.nx = number(e, "nx", 0);
        c.ny = number(e, "ny", 0);
        c.nz = number(e, "nz", 1);
        c.rotationDeg = number(e, "rotation", 0);
        c.amplitudeDb = number(e, "amplitude", 0);
        c.phaseDeg = number(e, "phase", 0);
        c.enabled = e.value(QStringLiteral("enabled")).toBool(true);
        s.custom.push_back(c);
    }
    return s;
}

Mat3 orientationFromNormal(const Vec3 &normal, double rotationDeg)
{
    Vec3 z = normal.norm() > 1e-12 ? Vec3(normal.normalized()) : Vec3(0, 0, 1);
    Vec3 ref = std::abs(z.x()) > 0.99 ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    Vec3 x = (ref - ref.dot(z) * z).normalized();
    Vec3 y = z.cross(x);
    const double r = toRadians(rotationDeg);
    const Vec3 xr = std::cos(r) * x + std::sin(r) * y;
    const Vec3 yr = -std::sin(r) * x + std::cos(r) * y;
    Mat3 m;
    m.col(0) = xr;
    m.col(1) = yr;
    m.col(2) = z;
    return m;
}

std::vector<CustomElement> toCustomElements(const GeometrySpec &spec, double designFrequency)
{
    if (spec.kind == LatticeKind::Custom)
        return spec.custom;
    const ArrayLayout layout = buildLayout(spec, designFrequency);
    const double unit = spec.unit == QLatin1String("m") ? 1.0 : wavelength(designFrequency);
    std::vector<CustomElement> list;
    for (const ArrayElement &e : layout.elements) {
        CustomElement c;
        c.x = e.position.x() / unit;
        c.y = e.position.y() / unit;
        c.z = e.position.z() / unit;
        const Vec3 n = e.orientation.col(2);
        c.nx = std::abs(n.x()) < 1e-12 ? 0.0 : n.x();
        c.ny = std::abs(n.y()) < 1e-12 ? 0.0 : n.y();
        c.nz = std::abs(n.z()) < 1e-12 ? 0.0 : n.z();
        // Rotation of the element's x axis from the default frame about the normal.
        const Mat3 reference = orientationFromNormal(n, 0);
        const Vec3 x = e.orientation.col(0);
        const double rot = toDegrees(std::atan2(x.dot(reference.col(1)), x.dot(reference.col(0))));
        c.rotationDeg = std::abs(rot) < 1e-9 ? 0.0 : rot;
        c.enabled = e.enabled;
        list.push_back(c);
    }
    return list;
}

ArrayLayout buildLayout(const GeometrySpec &spec, double designFrequency)
{
    ArrayLayout layout;
    const double unit = spec.unit == QLatin1String("m") ? 1.0 : wavelength(designFrequency);
    auto &elements = layout.elements;

    switch (spec.kind) {
    case LatticeKind::Linear: {
        const double dx = spec.dx * unit;
        for (int i = 0; i < spec.nx; ++i) {
            ArrayElement e;
            e.position = Vec3((i - (spec.nx - 1) / 2.0) * dx, 0, 0);
            e.row = 0;
            e.column = i;
            elements.push_back(e);
        }
        layout.lattice = Lattice{{Eigen::Vector2d(dx, 0)}};
        break;
    }
    case LatticeKind::Rectangular:
    case LatticeKind::Triangular: {
        const bool triangular = spec.kind == LatticeKind::Triangular;
        const double dx = spec.dx * unit, dy = spec.dy * unit;
        const double shift = triangular && spec.ny > 1 ? dx / 4 : 0.0; // centres the staggered rows
        for (int j = 0; j < spec.ny; ++j)
            for (int i = 0; i < spec.nx; ++i) {
                ArrayElement e;
                const double offset = triangular && (j % 2 == 1) ? dx / 2 : 0.0;
                e.position = Vec3((i - (spec.nx - 1) / 2.0) * dx + offset - shift, (j - (spec.ny - 1) / 2.0) * dy, 0);
                e.row = j;
                e.column = i;
                elements.push_back(e);
            }
        if (spec.aperture == QLatin1String("circle") || spec.aperture == QLatin1String("ellipse")) {
            double ax = spec.apertureX * unit;
            double ay = (spec.aperture == QLatin1String("circle") ? spec.apertureX : spec.apertureY) * unit;
            if (ax <= 0 || ay <= 0) {
                const double hx = spec.nx * dx / 2, hy = spec.ny * dy / 2;
                if (spec.aperture == QLatin1String("circle"))
                    ax = ay = std::min(hx, hy);
                else {
                    ax = hx;
                    ay = hy;
                }
            }
            std::vector<ArrayElement> kept;
            for (const ArrayElement &e : elements) {
                const double r = std::pow(e.position.x() / ax, 2) + std::pow(e.position.y() / ay, 2);
                if (r <= 1.0 + 1e-9)
                    kept.push_back(e);
            }
            elements = std::move(kept);
        }
        layout.lattice = Lattice{{Eigen::Vector2d(dx, 0), Eigen::Vector2d(triangular ? dx / 2 : 0.0, dy)}};
        break;
    }
    case LatticeKind::Circular: {
        if (spec.centreElement) {
            ArrayElement e;
            elements.push_back(e);
        }
        int ringIndex = 0;
        for (const RingSpec &ring : spec.rings) {
            for (int k = 0; k < ring.count; ++k) {
                const double a = toRadians(ring.rotationDeg) + 2 * kPi * k / ring.count;
                ArrayElement e;
                e.position = Vec3(ring.radius * unit * std::cos(a), ring.radius * unit * std::sin(a), 0);
                e.row = ringIndex;
                e.column = k;
                elements.push_back(e);
            }
            ++ringIndex;
        }
        break;
    }
    case LatticeKind::Cylinder: {
        const double radius = spec.cylinderRadius * unit;
        const double dz = spec.dz * unit;
        const bool full = spec.arcDeg >= 359.999;
        for (int r = 0; r < spec.rows; ++r)
            for (int c = 0; c < spec.columns; ++c) {
                double a;
                if (full)
                    a = 2 * kPi * c / spec.columns;
                else if (spec.columns == 1)
                    a = 0;
                else
                    a = toRadians(-spec.arcDeg / 2 + spec.arcDeg * c / (spec.columns - 1));
                ArrayElement e;
                e.position = Vec3(radius * std::cos(a), radius * std::sin(a), (r - (spec.rows - 1) / 2.0) * dz);
                e.orientation.col(0) = Vec3(-std::sin(a), std::cos(a), 0);
                e.orientation.col(1) = Vec3(0, 0, 1);
                e.orientation.col(2) = Vec3(std::cos(a), std::sin(a), 0);
                e.row = r;
                e.column = c;
                elements.push_back(e);
            }
        layout.identicalOrientation = false;
        break;
    }
    case LatticeKind::Custom: {
        int index = 0;
        for (const CustomElement &c : spec.custom) {
            ArrayElement e;
            e.position = Vec3(c.x, c.y, c.z) * unit;
            e.orientation = orientationFromNormal(Vec3(c.nx, c.ny, c.nz), c.rotationDeg);
            e.enabled = c.enabled;
            e.amplitudeDb = c.amplitudeDb;
            e.phaseDeg = c.phaseDeg;
            e.column = index++;
            if (!e.orientation.isIdentity(1e-9))
                layout.identicalOrientation = false;
            elements.push_back(e);
        }
        if (spec.custom.empty())
            layout.warnings << QStringLiteral("the custom element table is empty");
        break;
    }
    }
    if (elements.empty() && layout.warnings.isEmpty())
        layout.warnings << QStringLiteral("the geometry has no elements");

    layout.kind = spec.kind;
    switch (spec.kind) {
    case LatticeKind::Linear:
    case LatticeKind::Rectangular:
    case LatticeKind::Triangular:
        layout.pitchX = spec.dx * unit;
        layout.pitchY = spec.dy * unit;
        break;
    case LatticeKind::Cylinder:
        layout.cylinderRadius = spec.cylinderRadius * unit;
        layout.pitchX = spec.columns > 1 ? layout.cylinderRadius * toRadians(spec.arcDeg) / (spec.arcDeg >= 359.999 ? spec.columns : spec.columns - 1)
                                         : unit / 2;
        layout.pitchY = spec.dz * unit;
        break;
    case LatticeKind::Circular:
    case LatticeKind::Custom: {
        // Median nearest-neighbour distance.
        std::vector<double> nearest;
        for (std::size_t i = 0; i < elements.size(); ++i) {
            double best = INFINITY;
            for (std::size_t j = 0; j < elements.size(); ++j)
                if (i != j)
                    best = std::min(best, (elements[i].position - elements[j].position).norm());
            if (std::isfinite(best) && best > 0)
                nearest.push_back(best);
        }
        double pitch = unit / 2;
        if (!nearest.empty()) {
            std::nth_element(nearest.begin(), nearest.begin() + long(nearest.size() / 2), nearest.end());
            pitch = nearest[nearest.size() / 2];
        }
        layout.pitchX = layout.pitchY = pitch;
        break;
    }
    }
    return layout;
}

} // namespace lvd::ant

#include "fullwave/FwElement.h"

#include "core/JsonFile.h"
#include "fullwave/StlFile.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <cmath>
#include <numbers>

namespace lvd::fw {

QJsonObject settingsToJson(const FwSettings &s)
{
    QJsonArray ff;
    for (double f : s.farFieldFrequencies)
        ff.append(f);
    return QJsonObject{{QStringLiteral("fStart"), s.fStart},
                       {QStringLiteral("fStop"), s.fStop},
                       {QStringLiteral("cellsPerWavelength"), s.cellsPerWavelength},
                       {QStringLiteral("maxRatio"), s.maxRatio},
                       {QStringLiteral("endCriteria"), s.endCriteriaDb},
                       {QStringLiteral("maxTimesteps"), s.maxTimesteps},
                       {QStringLiteral("boundary"), s.boundary == Boundary::Pml8 ? QStringLiteral("pml") : QStringLiteral("mur")},
                       {QStringLiteral("airMargin"), s.airMarginWavelengths},
                       {QStringLiteral("farFieldStep"), s.farFieldStepDeg},
                       {QStringLiteral("farFieldFrequencies"), ff},
                       {QStringLiteral("frequencyPoints"), s.frequencyPoints}};
}

FwSettings settingsFromJson(const QJsonObject &o)
{
    FwSettings s;
    s.fStart = std::max(1.0, o.value(QStringLiteral("fStart")).toDouble(s.fStart));
    s.fStop = std::max(s.fStart * 1.001, o.value(QStringLiteral("fStop")).toDouble(s.fStop));
    s.cellsPerWavelength = std::clamp(o.value(QStringLiteral("cellsPerWavelength")).toDouble(s.cellsPerWavelength), 5.0, 200.0);
    s.maxRatio = std::clamp(o.value(QStringLiteral("maxRatio")).toDouble(s.maxRatio), 1.05, 3.0);
    s.endCriteriaDb = std::clamp(o.value(QStringLiteral("endCriteria")).toDouble(s.endCriteriaDb), -100.0, -10.0);
    s.maxTimesteps = std::clamp(o.value(QStringLiteral("maxTimesteps")).toInt(s.maxTimesteps), 100, 100000000);
    s.boundary = o.value(QStringLiteral("boundary")).toString() == QLatin1String("pml") ? Boundary::Pml8 : Boundary::Mur;
    s.airMarginWavelengths = std::clamp(o.value(QStringLiteral("airMargin")).toDouble(s.airMarginWavelengths), 0.05, 5.0);
    s.farFieldStepDeg = std::clamp(o.value(QStringLiteral("farFieldStep")).toDouble(s.farFieldStepDeg), 0.5, 15.0);
    for (const QJsonValue &v : o.value(QStringLiteral("farFieldFrequencies")).toArray())
        if (v.toDouble() > 0)
            s.farFieldFrequencies.push_back(v.toDouble());
    s.frequencyPoints = std::clamp(o.value(QStringLiteral("frequencyPoints")).toInt(s.frequencyPoints), 2, 100001);
    return s;
}

QJsonObject FwElementSetup::toJson() const
{
    QJsonObject json;
    for (const auto &[k, v] : values)
        json.insert(k, v);
    QJsonObject o{{QStringLiteral("template"), templateId},
                  {QStringLiteral("values"), json},
                  {QStringLiteral("settings"), settingsToJson(settings)}};
    if (!bodies.empty()) {
        QJsonArray list;
        for (const FwStlBody &b : bodies)
            list.append(QJsonObject{{QStringLiteral("file"), b.file},
                                    {QStringLiteral("scale"), b.scale},
                                    {QStringLiteral("metal"), b.metal},
                                    {QStringLiteral("epsR"), b.epsR},
                                    {QStringLiteral("tanD"), b.tanD},
                                    {QStringLiteral("name"), b.name}});
        o.insert(QStringLiteral("bodies"), list);
    }
    if (isArray())
        o.insert(QStringLiteral("array"), QJsonObject{{QStringLiteral("nx"), arrayNx},
                                                      {QStringLiteral("ny"), arrayNy},
                                                      {QStringLiteral("dx"), arrayDx},
                                                      {QStringLiteral("dy"), arrayDy}});
    return o;
}

FwElementSetup FwElementSetup::fromJson(const QJsonObject &o)
{
    FwElementSetup s = forTemplate(o.value(QStringLiteral("template")).toString(QStringLiteral("patch-probe")));
    const QJsonObject values = o.value(QStringLiteral("values")).toObject();
    for (auto it = values.begin(); it != values.end(); ++it)
        s.values[it.key()] = it.value().toDouble();
    if (o.contains(QStringLiteral("settings")))
        s.settings = settingsFromJson(o.value(QStringLiteral("settings")).toObject());
    for (const QJsonValue &v : o.value(QStringLiteral("bodies")).toArray()) {
        const QJsonObject b = v.toObject();
        FwStlBody body;
        body.file = b.value(QStringLiteral("file")).toString();
        body.scale = b.value(QStringLiteral("scale")).toDouble(1);
        body.metal = b.value(QStringLiteral("metal")).toBool(true);
        body.epsR = std::max(1.0, b.value(QStringLiteral("epsR")).toDouble(1));
        body.tanD = std::max(0.0, b.value(QStringLiteral("tanD")).toDouble(0));
        body.name = b.value(QStringLiteral("name")).toString();
        s.bodies.push_back(body);
    }
    const QJsonObject a = o.value(QStringLiteral("array")).toObject();
    s.arrayNx = std::clamp(a.value(QStringLiteral("nx")).toInt(1), 1, 16);
    s.arrayNy = std::clamp(a.value(QStringLiteral("ny")).toInt(1), 1, 16);
    s.arrayDx = std::max(0.0, a.value(QStringLiteral("dx")).toDouble(0));
    s.arrayDy = std::max(0.0, a.value(QStringLiteral("dy")).toDouble(0));
    return s;
}

FwElementSetup FwElementSetup::forTemplate(const QString &templateId)
{
    FwElementSetup s;
    s.templateId = templateId;
    if (const TemplateInfo *info = findTemplate(templateId)) {
        s.settings.fStart = info->fStart;
        s.settings.fStop = info->fStop;
        s.settings.boundary = info->boundary;
        s.values = templateValues(*info, {});
    }
    return s;
}

FwModel repeatModel(const FwModel &element, int nx, int ny, double dx, double dy)
{
    FwModel out;
    out.materials = element.materials;
    out.warnings = element.warnings;
    const int cx = (nx - 1) / 2, cy = (ny - 1) / 2; // the excited copy
    std::vector<FwPort> centre, others;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const Vec3 off((i - cx) * dx, (j - cy) * dy, 0);
            for (FwPrimitive p : element.primitives) {
                if (!p.transforms.empty() || p.kind == PrimitiveKind::StlFile) {
                    p.transforms.push_back({FwTransform::Kind::Translate, off});
                } else {
                    switch (p.kind) {
                    case PrimitiveKind::Box:
                    case PrimitiveKind::Cylinder:
                        p.p1 += off;
                        p.p2 += off;
                        break;
                    case PrimitiveKind::Polygon:
                    case PrimitiveKind::Extrusion:
                        p.elevation += off[p.normal];
                        for (auto &q : p.points) {
                            q[0] += off[(p.normal + 1) % 3];
                            q[1] += off[(p.normal + 2) % 3];
                        }
                        break;
                    case PrimitiveKind::Wire:
                        for (Vec3 &v : p.path)
                            v += off;
                        break;
                    case PrimitiveKind::StlFile:
                        break;
                    }
                }
                out.primitives.push_back(p);
            }
            for (int axis = 0; axis < 3; ++axis)
                for (double l : element.extraLines[std::size_t(axis)])
                    out.extraLines[std::size_t(axis)].push_back(l + off[axis]);
            for (FwRefinement r : element.refinements) {
                r.lo += off;
                r.hi += off;
                out.refinements.push_back(r);
            }
            const bool excited = i == cx && j == cy;
            for (FwPort port : element.ports) {
                port.start += off;
                port.stop += off;
                port.excite = excited && port.excite;
                (excited ? centre : others).push_back(port);
            }
            if (excited)
                out.farFieldCentre = element.farFieldCentre + off;
        }
    int number = 1;
    for (FwPort &p : centre)
        p.number = number++;
    for (FwPort &p : others)
        p.number = number++;
    out.ports = centre;
    out.ports.insert(out.ports.end(), others.begin(), others.end());
    return out;
}

FwModel buildModel(const FwElementSetup &setup, const QString &baseDirectory, QString *error)
{
    FwModel model = buildTemplate(setup.templateId, setup.values, error);
    if (setup.templateId == QLatin1String("stl")) {
        if (setup.bodies.empty())
            model.warnings << QStringLiteral("no STL bodies: import at least one");
        for (const FwStlBody &b : setup.bodies) {
            const QString path = QDir(baseDirectory).absoluteFilePath(b.file);
            QString readError;
            const auto triangles = readStl(path, b.scale, &readError);
            if (!triangles) {
                setError(error, readError);
                continue;
            }
            FwMaterial mat;
            mat.name = b.name.isEmpty() ? QFileInfo(b.file).completeBaseName() : b.name;
            mat.metal = b.metal;
            mat.epsR = b.epsR;
            mat.tanD = b.tanD;
            mat.color = b.metal ? QColor(200, 160, 60) : QColor(90, 160, 90, 120);
            FwPrimitive p;
            p.kind = PrimitiveKind::StlFile;
            p.material = model.addMaterial(mat);
            p.priority = b.metal ? 10 : 0;
            p.file = QDir::toNativeSeparators(path);
            p.stlScale = b.scale;
            p.triangles = *triangles;
            model.primitives.push_back(p);
        }
    }
    if (setup.isArray())
        model = repeatModel(model, setup.arrayNx, setup.arrayNy, setup.arrayDx, setup.arrayDy);
    return model;
}

void applyResults(ant::ElementSpec &s, const FwResults &results, const QString &elementName)
{
    s.patternFile = simulationPatternFile(elementName);
    s.pattern.reset();
    if (std::isfinite(results.resonance()))
        s.frequency = results.resonance();
    if (!results.radiationEfficiency.empty()) {
        std::size_t best = 0;
        for (std::size_t k = 0; k < results.farFieldFrequencies.size(); ++k)
            if (std::abs(results.farFieldFrequencies[k] - s.frequency) < std::abs(results.farFieldFrequencies[best] - s.frequency))
                best = k;
        const double eta = results.radiationEfficiency[best];
        if (std::isfinite(eta) && eta > 0)
            s.efficiencyDb = std::min(0.0, 10 * std::log10(eta));
    }
}

QString simulationFolder(const QString &projectDirectory, const QString &elementName)
{
    return QDir(projectDirectory).filePath(QStringLiteral("simulations/") + elementName);
}

QString simulationPatternFile(const QString &elementName)
{
    return QStringLiteral("simulations/%1/pattern.csv").arg(elementName);
}

// ---- Triangulation

std::vector<unsigned> triangulate(const std::vector<std::array<double, 2>> &outline)
{
    std::vector<unsigned> out;
    const std::size_t n = outline.size();
    if (n < 3)
        return out;
    // Orientation (shoelace) so that ears are convex corners.
    double area = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto &a = outline[i], &b = outline[(i + 1) % n];
        area += a[0] * b[1] - b[0] * a[1];
    }
    const double sign = area >= 0 ? 1.0 : -1.0;
    std::vector<unsigned> idx(n);
    for (std::size_t i = 0; i < n; ++i)
        idx[i] = unsigned(i);
    auto cross = [&](unsigned a, unsigned b, unsigned c) {
        const auto &p = outline[a], &q = outline[b], &r = outline[c];
        return ((q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])) * sign;
    };
    auto inside = [&](unsigned p, unsigned a, unsigned b, unsigned c) {
        return cross(a, b, p) >= 0 && cross(b, c, p) >= 0 && cross(c, a, p) >= 0;
    };
    std::size_t guard = 0;
    while (idx.size() > 3 && guard++ < 10 * n * n) {
        bool clipped = false;
        for (std::size_t i = 0; i < idx.size(); ++i) {
            const unsigned a = idx[(i + idx.size() - 1) % idx.size()], b = idx[i], c = idx[(i + 1) % idx.size()];
            if (cross(a, b, c) <= 1e-12)
                continue; // reflex or degenerate corner
            bool ear = true;
            for (unsigned p : idx)
                if (p != a && p != b && p != c && inside(p, a, b, c)) {
                    ear = false;
                    break;
                }
            if (!ear)
                continue;
            out.insert(out.end(), {a, b, c});
            idx.erase(idx.begin() + long(i));
            clipped = true;
            break;
        }
        if (!clipped)
            break; // self-intersecting outline: give up on the rest
    }
    if (idx.size() == 3)
        out.insert(out.end(), {idx[0], idx[1], idx[2]});
    return out;
}

namespace {

void addQuad(FwDisplayMesh &m, const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &d)
{
    const unsigned base = unsigned(m.vertices.size());
    m.vertices.insert(m.vertices.end(), {a, b, c, d});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void addBox(FwDisplayMesh &m, const Vec3 &p1, const Vec3 &p2)
{
    const Vec3 lo = p1.cwiseMin(p2), hi = p1.cwiseMax(p2);
    const Vec3 size = hi - lo;
    // A zero-thickness box is a sheet: one quad.
    for (int axis = 0; axis < 3; ++axis)
        if (size[axis] < 1e-9) {
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;
            Vec3 a = lo, b = lo, c = lo, d = lo;
            b[u] = hi[u];
            c[u] = hi[u];
            c[v] = hi[v];
            d[v] = hi[v];
            addQuad(m, a, b, c, d);
            return;
        }
    const Vec3 c[8] = {{lo.x(), lo.y(), lo.z()}, {hi.x(), lo.y(), lo.z()}, {hi.x(), hi.y(), lo.z()}, {lo.x(), hi.y(), lo.z()},
                       {lo.x(), lo.y(), hi.z()}, {hi.x(), lo.y(), hi.z()}, {hi.x(), hi.y(), hi.z()}, {lo.x(), hi.y(), hi.z()}};
    addQuad(m, c[0], c[3], c[2], c[1]);
    addQuad(m, c[4], c[5], c[6], c[7]);
    addQuad(m, c[0], c[1], c[5], c[4]);
    addQuad(m, c[1], c[2], c[6], c[5]);
    addQuad(m, c[2], c[3], c[7], c[6]);
    addQuad(m, c[3], c[0], c[4], c[7]);
}

void addCylinder(FwDisplayMesh &m, const Vec3 &p1, const Vec3 &p2, double r, int segments = 24)
{
    const Vec3 axis = p2 - p1;
    if (axis.norm() < 1e-12 || r <= 0)
        return;
    const Vec3 w = axis.normalized();
    const Vec3 ref = std::abs(w.x()) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
    const Vec3 u = w.cross(ref).normalized(), v = w.cross(u);
    for (int k = 0; k < segments; ++k) {
        const double a0 = 2 * std::numbers::pi * k / segments, a1 = 2 * std::numbers::pi * (k + 1) / segments;
        const Vec3 d0 = r * (std::cos(a0) * u + std::sin(a0) * v), d1 = r * (std::cos(a1) * u + std::sin(a1) * v);
        addQuad(m, p1 + d0, p1 + d1, p2 + d1, p2 + d0);
        const unsigned base = unsigned(m.vertices.size());
        m.vertices.insert(m.vertices.end(), {p1, p1 + d1, p1 + d0, p2, p2 + d0, p2 + d1});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base + 3, base + 4, base + 5});
    }
}

Vec3 planePoint(int normal, double elevation, const std::array<double, 2> &p)
{
    Vec3 v;
    v[normal] = elevation;
    v[(normal + 1) % 3] = p[0];
    v[(normal + 2) % 3] = p[1];
    return v;
}

} // namespace

std::vector<FwDisplayMesh> displayMeshes(const FwModel &model)
{
    std::vector<FwDisplayMesh> meshes(model.materials.size());
    for (std::size_t i = 0; i < model.materials.size(); ++i) {
        meshes[i].material = model.materials[i].name;
        meshes[i].color = model.materials[i].color;
        meshes[i].metal = model.materials[i].metal;
    }
    for (const FwPrimitive &p : model.primitives) {
        if (p.material < 0 || p.material >= int(meshes.size()))
            continue;
        FwDisplayMesh &m = meshes[std::size_t(p.material)];
        const std::size_t firstVertex = m.vertices.size();
        switch (p.kind) {
        case PrimitiveKind::Box:
            addBox(m, p.p1, p.p2);
            break;
        case PrimitiveKind::Cylinder:
            addCylinder(m, p.p1, p.p2, p.radius);
            break;
        case PrimitiveKind::Polygon:
        case PrimitiveKind::Extrusion: {
            const std::vector<unsigned> tri = triangulate(p.points);
            const double top = p.elevation + (p.kind == PrimitiveKind::Extrusion ? p.length : 0.0);
            for (double z : {p.elevation, top}) {
                const unsigned base = unsigned(m.vertices.size());
                for (const auto &q : p.points)
                    m.vertices.push_back(planePoint(p.normal, z, q));
                for (unsigned t : tri)
                    m.indices.push_back(base + t);
                if (p.kind == PrimitiveKind::Polygon)
                    break;
            }
            if (p.kind == PrimitiveKind::Extrusion)
                for (std::size_t i = 0; i < p.points.size(); ++i) {
                    const auto &a = p.points[i], &b = p.points[(i + 1) % p.points.size()];
                    addQuad(m, planePoint(p.normal, p.elevation, a), planePoint(p.normal, p.elevation, b), planePoint(p.normal, top, b),
                            planePoint(p.normal, top, a));
                }
            break;
        }
        case PrimitiveKind::Wire:
            for (std::size_t i = 1; i < p.path.size(); ++i)
                addCylinder(m, p.path[i - 1], p.path[i], std::max(p.radius, 0.2), 8);
            break;
        case PrimitiveKind::StlFile: {
            const unsigned base = unsigned(m.vertices.size());
            m.vertices.insert(m.vertices.end(), p.triangles.begin(), p.triangles.end());
            for (unsigned i = 0; i < unsigned(p.triangles.size()); ++i)
                m.indices.push_back(base + i);
            break;
        }
        }
        if (!p.transforms.empty())
            for (std::size_t i = firstVertex; i < m.vertices.size(); ++i)
                m.vertices[i] = p.transformed(m.vertices[i]);
    }
    // Ports as thin red cylinders.
    FwDisplayMesh ports;
    ports.material = QStringLiteral("ports");
    ports.color = QColor(220, 40, 40);
    ports.metal = false;
    for (const FwPort &port : model.ports)
        addCylinder(ports, port.start, port.stop, 0.4, 10);
    if (!ports.vertices.empty())
        meshes.push_back(ports);
    meshes.erase(std::remove_if(meshes.begin(), meshes.end(), [](const FwDisplayMesh &m) { return m.vertices.empty(); }), meshes.end());
    return meshes;
}

} // namespace lvd::fw

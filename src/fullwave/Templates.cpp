#include "fullwave/Templates.h"

#include "core/JsonFile.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lvd::fw {

namespace {

const QColor kMetalColor(200, 160, 60);
const QColor kSubstrateColor(90, 160, 90, 120);

int metal(FwModel &m, const QString &name)
{
    FwMaterial mat;
    mat.name = name;
    mat.metal = true;
    mat.color = kMetalColor;
    return m.addMaterial(mat);
}

int dielectric(FwModel &m, const QString &name, double epsR, double tanD)
{
    FwMaterial mat;
    mat.name = name;
    mat.metal = false;
    mat.epsR = epsR;
    mat.tanD = tanD;
    mat.color = kSubstrateColor;
    return m.addMaterial(mat);
}

// Lines through a thin layer: at least `cells` cells.
void layerLines(FwModel &m, double z0, double z1, int cells)
{
    for (int k = 0; k <= cells; ++k)
        m.extraLines[2].push_back(z0 + (z1 - z0) * k / cells);
}

void addPort(FwModel &m, const Vec3 &start, const Vec3 &stop, int axis, double r)
{
    FwPort port;
    port.start = start;
    port.stop = stop;
    port.axis = axis;
    port.resistance = r;
    m.ports.push_back(port);
}

FwPrimitive polygon(int material, int normal, double elevation, std::vector<std::array<double, 2>> points, int priority = 10)
{
    FwPrimitive p;
    p.kind = PrimitiveKind::Polygon;
    p.material = material;
    p.normal = normal;
    p.elevation = elevation;
    p.points = std::move(points);
    p.priority = priority;
    return p;
}

FwPrimitive extrusion(int material, int normal, double elevation, double length, std::vector<std::array<double, 2>> points,
                      int priority = 10)
{
    FwPrimitive p = polygon(material, normal, elevation, std::move(points), priority);
    p.kind = PrimitiveKind::Extrusion;
    p.length = length;
    return p;
}

// A ground plane (z = elevation) of size sx by sy with a rectangular hole,
// as four sheets around the hole.
void groundWithSlot(FwModel &m, int material, double sx, double sy, double z, double holeX0, double holeX1, double holeY0,
                    double holeY1)
{
    m.addBox(material, {-sx / 2, -sy / 2, z}, {sx / 2, holeY0, z});
    m.addBox(material, {-sx / 2, holeY1, z}, {sx / 2, sy / 2, z});
    m.addBox(material, {-sx / 2, holeY0, z}, {holeX0, holeY1, z});
    m.addBox(material, {holeX1, holeY0, z}, {sx / 2, holeY1, z});
}

double v(const ParameterValues &p, const char *id)
{
    return p.at(QString::fromLatin1(id));
}

// ---- Patches

FwModel patchProbe(const ParameterValues &p)
{
    FwModel m;
    const double w = v(p, "patchX"), l = v(p, "patchY"), sx = v(p, "substrateX"), sy = v(p, "substrateY"), h = v(p, "height");
    const int sub = dielectric(m, QStringLiteral("substrate"), v(p, "epsR"), v(p, "tanD"));
    const int patch = metal(m, QStringLiteral("patch"));
    const int gnd = metal(m, QStringLiteral("ground"));
    m.addBox(sub, {-sx / 2, -sy / 2, 0}, {sx / 2, sy / 2, h}, 0);
    m.addBox(patch, {-w / 2, -l / 2, h}, {w / 2, l / 2, h}, 10);
    m.primitives.back().edgeThirds = true;
    m.addBox(gnd, {-sx / 2, -sy / 2, 0}, {sx / 2, sy / 2, 0}, 10);
    addPort(m, {v(p, "feedX"), 0, 0}, {v(p, "feedX"), 0, h}, 2, v(p, "resistance"));
    layerLines(m, 0, h, int(v(p, "substrateCells")));
    m.farFieldCentre = {0, 0, 1};
    if (std::abs(v(p, "feedX")) >= w / 2)
        m.warnings << QStringLiteral("the feed lies outside the patch");
    if (sx < w || sy < l)
        m.warnings << QStringLiteral("the substrate is smaller than the patch");
    return m;
}

FwModel patchInset(const ParameterValues &p)
{
    FwModel m;
    const double L = v(p, "patchX"), W = v(p, "patchY"), sx = v(p, "substrateX"), sy = v(p, "substrateY"), h = v(p, "height");
    const double d = v(p, "insetDepth"), gap = v(p, "insetGap"), wl = v(p, "lineWidth");
    const int sub = dielectric(m, QStringLiteral("substrate"), v(p, "epsR"), v(p, "tanD"));
    const int cu = metal(m, QStringLiteral("patch and line"));
    const int gnd = metal(m, QStringLiteral("ground"));
    m.addBox(sub, {-sx / 2, -sy / 2, 0}, {sx / 2, sy / 2, h}, 0);
    m.addBox(gnd, {-sx / 2, -sy / 2, 0}, {sx / 2, sy / 2, 0}, 10);
    // The patch with a notch on its -x edge for the inset feed.
    const double wn = wl / 2 + gap;
    m.primitives.push_back(polygon(cu, 2, h,
                                   {{-L / 2, -W / 2}, {L / 2, -W / 2}, {L / 2, W / 2}, {-L / 2, W / 2}, {-L / 2, wn},
                                    {-L / 2 + d, wn}, {-L / 2 + d, -wn}, {-L / 2, -wn}}));
    // Feed line from the substrate edge into the notch.
    m.addBox(cu, {-sx / 2, -wl / 2, h}, {-L / 2 + d, wl / 2, h}, 10);
    addPort(m, {-sx / 2, -wl / 2, 0}, {-sx / 2, wl / 2, h}, 2, v(p, "resistance"));
    layerLines(m, 0, h, int(v(p, "substrateCells")));
    // The line width needs a few cells.
    m.refinements.push_back({{-sx / 2, -wl, 0}, {-L / 2 + d, wl, h}, wl / 3});
    m.farFieldCentre = {0, 0, h};
    if (d >= L / 2)
        m.warnings << QStringLiteral("the inset is deeper than half the patch");
    return m;
}

FwModel patchAperture(const ParameterValues &p)
{
    FwModel m;
    const double L = v(p, "patchX"), W = v(p, "patchY"), sx = v(p, "substrateX"), sy = v(p, "substrateY");
    const double h1 = v(p, "feedHeight"), h2 = v(p, "patchHeight");
    const double sl = v(p, "slotLength"), sw = v(p, "slotWidth"), wl = v(p, "lineWidth"), stub = v(p, "stubLength");
    const int feedSub = dielectric(m, QStringLiteral("feed substrate"), v(p, "feedEpsR"), v(p, "tanD"));
    const int patchSub = dielectric(m, QStringLiteral("patch substrate"), v(p, "patchEpsR"), v(p, "tanD"));
    const int cu = metal(m, QStringLiteral("patch"));
    const int gnd = metal(m, QStringLiteral("ground"));
    const int line = metal(m, QStringLiteral("feed line"));
    // Layers: feed line (z = 0), feed substrate, ground with a slot (z = h1),
    // patch substrate, patch (z = h1 + h2). The slot is long along y.
    m.addBox(feedSub, {-sx / 2, -sy / 2, 0}, {sx / 2, sy / 2, h1}, 0);
    m.addBox(patchSub, {-sx / 2, -sy / 2, h1}, {sx / 2, sy / 2, h1 + h2}, 0);
    groundWithSlot(m, gnd, sx, sy, h1, -sw / 2, sw / 2, -sl / 2, sl / 2);
    m.addBox(cu, {-L / 2, -W / 2, h1 + h2}, {L / 2, W / 2, h1 + h2}, 10);
    m.primitives.back().edgeThirds = true;
    m.addBox(line, {-sx / 2, -wl / 2, 0}, {sw / 2 + stub, wl / 2, 0}, 10);
    // Port between the line (z = 0) and the ground (z = h1) at the board edge.
    addPort(m, {-sx / 2, -wl / 2, 0}, {-sx / 2, wl / 2, h1}, 2, v(p, "resistance"));
    layerLines(m, 0, h1, 3);
    layerLines(m, h1, h1 + h2, 4);
    m.refinements.push_back({{-sw * 2, -sl / 2 - 1, 0}, {sw * 2, sl / 2 + 1, h1}, std::max(sw / 3, 0.1)});
    m.farFieldCentre = {0, 0, h1 + h2};
    return m;
}

// ---- Wires

FwModel dipole(const ParameterValues &p)
{
    FwModel m;
    const double L = v(p, "length"), w = v(p, "width"), g = v(p, "gap");
    const int cu = metal(m, QStringLiteral("dipole"));
    m.addBox(cu, {-L / 2, -w / 2, 0}, {-g / 2, w / 2, 0}, 10);
    m.addBox(cu, {g / 2, -w / 2, 0}, {L / 2, w / 2, 0}, 10);
    addPort(m, {-g / 2, -w / 2, 0}, {g / 2, w / 2, 0}, 0, v(p, "resistance"));
    m.refinements.push_back({{-g * 2, -w * 2, -w}, {g * 2, w * 2, w}, std::max(std::min(g, w) / 3, 0.05)});
    if (g >= L / 4)
        m.warnings << QStringLiteral("the gap is large compared with the dipole");
    return m;
}

FwModel monopole(const ParameterValues &p)
{
    FwModel m;
    const double hgt = v(p, "height"), G = v(p, "ground"), g = v(p, "gap");
    const int cu = metal(m, QStringLiteral("monopole"));
    const int gnd = metal(m, QStringLiteral("ground"));
    m.addBox(gnd, {-G / 2, -G / 2, 0}, {G / 2, G / 2, 0}, 10);
    FwPrimitive wire;
    wire.kind = PrimitiveKind::Wire;
    wire.material = cu;
    wire.path = {{0, 0, g}, {0, 0, hgt}};
    m.primitives.push_back(wire);
    addPort(m, {0, 0, 0}, {0, 0, g}, 2, v(p, "resistance"));
    m.refinements.push_back({{-5, -5, 0}, {5, 5, g * 3}, std::max(g / 2, 0.2)});
    return m;
}

FwModel helix(const ParameterValues &p)
{
    FwModel m;
    const double r = v(p, "radius"), pitch = v(p, "pitch"), feed = v(p, "feedHeight"), gr = v(p, "groundRadius");
    const int turns = int(v(p, "turns"));
    const int cu = metal(m, QStringLiteral("helix"));
    const int gnd = metal(m, QStringLiteral("ground"));
    // A thin wire, 20 segments per turn, as the openEMS Helical Antenna tutorial.
    FwPrimitive wire;
    wire.kind = PrimitiveKind::Wire;
    wire.material = cu;
    double z = feed;
    for (int t = 0; t < turns; ++t) {
        for (int k = 0; k <= 20; ++k) {
            const double a = 2 * std::numbers::pi * k / 20;
            wire.path.emplace_back(r * std::cos(a), r * std::sin(a), z + a / (2 * std::numbers::pi) * pitch);
        }
        z += pitch;
    }
    m.primitives.push_back(wire);
    FwPrimitive disc;
    disc.kind = PrimitiveKind::Cylinder;
    disc.material = gnd;
    disc.p1 = {0, 0, -0.1};
    disc.p2 = {0, 0, 0.1};
    disc.radius = gr;
    m.primitives.push_back(disc);
    addPort(m, {r, 0, 0}, {r, 0, feed}, 2, v(p, "resistance"));
    m.extraLines[0] = {-r, 0, r};
    m.extraLines[1] = {-r, 0, r};
    m.refinements.push_back({{-r - 3, -r - 3, 0}, {r + 3, r + 3, z + 3}, v(p, "meshResolution")});
    return m;
}

// ---- PCB

FwModel ifa(const ParameterValues &p)
{
    FwModel m;
    const double bx = v(p, "boardX"), by = v(p, "boardY"), h = v(p, "height"), clearance = v(p, "clearance");
    const double H = v(p, "armHeight"), La = v(p, "armLength"), wt = v(p, "traceWidth"), df = v(p, "feedOffset");
    const double gap = v(p, "portGap");
    const int sub = dielectric(m, QStringLiteral("board"), v(p, "epsR"), v(p, "tanD"));
    const int cu = metal(m, QStringLiteral("copper"));
    const double yg = by / 2 - clearance; // ground edge
    const double xs = -bx / 2 + v(p, "shortOffset");
    m.addBox(sub, {-bx / 2, -by / 2, 0}, {bx / 2, by / 2, h}, 0);
    m.addBox(cu, {-bx / 2, -by / 2, h}, {bx / 2, yg, h}, 10);                          // ground
    m.addBox(cu, {xs - wt / 2, yg, h}, {xs + wt / 2, yg + H + wt / 2, h}, 10);         // short to ground
    m.addBox(cu, {xs - wt / 2, yg + H - wt / 2, h}, {xs + La, yg + H + wt / 2, h}, 10); // arm
    const double xf = xs + df;
    m.addBox(cu, {xf - wt / 2, yg + gap, h}, {xf + wt / 2, yg + H, h}, 10);            // feed
    addPort(m, {xf - wt / 2, yg, h}, {xf + wt / 2, yg + gap, h}, 1, v(p, "resistance"));
    layerLines(m, 0, h, 3);
    m.refinements.push_back({{xs - 2, yg - 2, 0}, {xs + La + 2, yg + H + 2, h}, std::max(wt / 3, 0.1)});
    m.farFieldCentre = {0, 0, h};
    if (H + wt > clearance)
        m.warnings << QStringLiteral("the antenna does not fit in the ground clearance");
    if (xs + La > bx / 2)
        m.warnings << QStringLiteral("the arm runs off the board");
    return m;
}

FwModel slot(const ParameterValues &p)
{
    FwModel m;
    const double bx = v(p, "boardX"), by = v(p, "boardY"), h = v(p, "height"), L = v(p, "slotLength"), w = v(p, "slotWidth");
    const int sub = dielectric(m, QStringLiteral("board"), v(p, "epsR"), v(p, "tanD"));
    const int cu = metal(m, QStringLiteral("ground"));
    m.addBox(sub, {-bx / 2, -by / 2, 0}, {bx / 2, by / 2, h}, 0);
    groundWithSlot(m, cu, bx, by, h, -L / 2, L / 2, -w / 2, w / 2);
    addPort(m, {0, -w / 2, h}, {0, w / 2, h}, 1, v(p, "resistance"));
    layerLines(m, 0, h, 3);
    m.refinements.push_back({{-L / 2 - 2, -w * 2, 0}, {L / 2 + 2, w * 2, h}, std::max(w / 3, 0.1)});
    m.farFieldCentre = {0, 0, h};
    return m;
}

FwModel vivaldi(const ParameterValues &p)
{
    FwModel m;
    const double Lx = v(p, "length"), W = v(p, "width"), Wo = v(p, "opening"), w0 = v(p, "slotWidth");
    const double xs = v(p, "slotStart"), xt = v(p, "taperStart"), xp = v(p, "feedPosition"), h = v(p, "height");
    const double c = v(p, "cavity");
    const int cu = metal(m, QStringLiteral("copper"));
    // Metal outline: the rectangle with the slot cut in from the +x edge:
    // a square cavity of side c ending at xs (the open-circuit stub behind
    // the feed), a straight slot of width w0 from xs to xt, then an
    // exponential taper to the opening Wo at x = Lx.
    const double R = std::log(std::max(Wo / w0, 1.0001)) / std::max(Lx - xt, 1e-6);
    std::vector<std::array<double, 2>> lower, upper;
    constexpr int n = 40;
    for (int k = 0; k <= n; ++k) {
        const double x = xt + (Lx - xt) * k / n;
        const double y = 0.5 * w0 * std::exp(R * (x - xt));
        lower.push_back({x, -y});
        upper.push_back({x, y});
    }
    std::vector<std::array<double, 2>> outline{{0, -W / 2}, {Lx, -W / 2}};
    for (auto it = lower.rbegin(); it != lower.rend(); ++it)
        outline.push_back(*it); // from the opening back to the taper start
    outline.push_back({xs, -w0 / 2});
    if (c > w0) {
        outline.push_back({xs, -c / 2});
        outline.push_back({xs - c, -c / 2});
        outline.push_back({xs - c, c / 2});
        outline.push_back({xs, c / 2});
    }
    outline.push_back({xs, w0 / 2});
    for (const auto &q : upper)
        outline.push_back(q);
    outline.push_back({Lx, W / 2});
    outline.push_back({0, W / 2});
    if (h > 0) {
        const int sub = dielectric(m, QStringLiteral("substrate"), v(p, "epsR"), v(p, "tanD"));
        m.addBox(sub, {0, -W / 2, -h}, {Lx, W / 2, 0}, 0);
        layerLines(m, -h, 0, 2);
    }
    m.primitives.push_back(polygon(cu, 2, 0, outline));
    addPort(m, {xp, -w0 / 2, 0}, {xp, w0 / 2, 0}, 1, v(p, "resistance"));
    m.extraLines[1] = {-w0 / 2, w0 / 2};
    m.refinements.push_back({{xs - 2, -3 * w0 - 1, -1}, {xt + 3, 3 * w0 + 1, 1}, std::max(w0 / 3, 0.05)});
    m.farFieldCentre = {Lx / 2, 0, 0};
    if (!(xs < xp && xp < xt && xt < Lx))
        m.warnings << QStringLiteral("expected slot start < feed position < taper start < length");
    if (c > w0 && (xs - c <= 0 || c >= W))
        m.warnings << QStringLiteral("the cavity does not fit behind the slot start");
    return m;
}

// ---- Horn

FwModel horn(const ParameterValues &p)
{
    // The openEMS Horn Antenna tutorial: a pin-fed rectangular waveguide
    // flaring into a pyramidal horn along +z.
    FwModel m;
    const double a = v(p, "waveguideA"), b = v(p, "waveguideB"), t = v(p, "wall"), feedLength = v(p, "feedLength");
    const double hornLength = v(p, "hornLength"), angle = v(p, "flareAngle");
    const double pinLength = v(p, "pinLength") * b, pinR = v(p, "pinHalfWidth"), portH = v(p, "portGap");
    const double f0 = v(p, "designFrequency") * 1e9;
    const double fcWg = ant::kSpeedOfLight / (2 * a * 1e-3);
    const double lambdaG = ant::kSpeedOfLight / f0 * 1e3 / std::sqrt(std::max(1e-6, 1 - std::pow(fcWg / f0, 2)));
    const double zBack = -feedLength;
    const double zPin = zBack + lambdaG / 4;
    const double s = std::sin(ant::toRadians(angle)) * hornLength;
    const int cu = metal(m, QStringLiteral("horn"));
    const int pin = metal(m, QStringLiteral("pin"));
    m.addBox(cu, {-a / 2 - t, -b / 2, zBack}, {-a / 2, b / 2, 0});
    m.addBox(cu, {a / 2, -b / 2, zBack}, {a / 2 + t, b / 2, 0});
    m.addBox(cu, {-a / 2 - t, b / 2, zBack}, {a / 2 + t, b / 2 + t, 0});
    m.addBox(cu, {-a / 2 - t, -b / 2 - t, zBack}, {a / 2 + t, -b / 2, 0});
    m.addBox(cu, {-a / 2 - t, -b / 2 - t, zBack - t}, {a / 2 + t, b / 2 + t, zBack});
    // Flare walls: plates rotated by the flare angle (as the tutorial's LinPoly walls).
    const std::vector<std::array<double, 2>> tb = {{0, a / 2}, {hornLength, a / 2 + s}, {hornLength, -a / 2 - s}, {0, -a / 2}};
    FwPrimitive bottom = extrusion(cu, 1, -t / 2, t, tb);
    bottom.transforms = {{FwTransform::Kind::RotateX, Vec3(angle, 0, 0)}, {FwTransform::Kind::Translate, Vec3(0, -b / 2 - t / 2, 0)}};
    FwPrimitive top = extrusion(cu, 1, -t / 2, t, tb);
    top.transforms = {{FwTransform::Kind::RotateX, Vec3(-angle, 0, 0)}, {FwTransform::Kind::Translate, Vec3(0, b / 2 + t / 2, 0)}};
    const std::vector<std::array<double, 2>> lr = {{b / 2 + t, 0}, {b / 2 + t + s, hornLength}, {-b / 2 - t - s, hornLength}, {-b / 2 - t, 0}};
    FwPrimitive left = extrusion(cu, 0, -t / 2, t, lr);
    left.transforms = {{FwTransform::Kind::RotateY, Vec3(-angle, 0, 0)}, {FwTransform::Kind::Translate, Vec3(-a / 2 - t / 2, 0, 0)}};
    FwPrimitive right = extrusion(cu, 0, -t / 2, t, lr);
    right.transforms = {{FwTransform::Kind::RotateY, Vec3(angle, 0, 0)}, {FwTransform::Kind::Translate, Vec3(a / 2 + t / 2, 0, 0)}};
    for (FwPrimitive *w : {&bottom, &top, &left, &right})
        m.primitives.push_back(*w);
    // The pin: a probe from the broad wall, fed at its base.
    m.addBox(pin, {-pinR, -b / 2 + portH, zPin - pinR}, {pinR, -b / 2 + pinLength, zPin + pinR});
    addPort(m, {-pinR, -b / 2, zPin - pinR}, {pinR, -b / 2 + portH, zPin + pinR}, 1, v(p, "resistance"));
    m.extraLines[0] = {-a / 2 - t, -a / 2, -pinR, pinR, a / 2, a / 2 + t};
    m.extraLines[1] = {-b / 2 - t, -b / 2, -b / 2 + portH, -b / 2 + pinLength, b / 2, b / 2 + t};
    m.extraLines[2] = {zBack - t, zBack, zPin - pinR, zPin + pinR, 0, hornLength};
    m.farFieldCentre = {0, 0, 0};
    if (fcWg >= f0)
        m.warnings << QStringLiteral("the waveguide is below cutoff at the design frequency");
    return m;
}

TemplateParameter param(const char *id, const char *label, const char *unit, double value, double min, double max,
                        const char *help = "")
{
    return {QString::fromLatin1(id), QString::fromUtf8(label), QString::fromLatin1(unit), value, min, max, QString::fromUtf8(help)};
}

TemplateInfo info(const char *id, const char *name, const char *description, double fStart, double fStop,
                  std::vector<TemplateParameter> parameters, Boundary boundary = Boundary::Mur)
{
    TemplateInfo t;
    t.id = QString::fromLatin1(id);
    t.name = QString::fromUtf8(name);
    t.description = QString::fromUtf8(description);
    t.fStart = fStart;
    t.fStop = fStop;
    t.parameters = std::move(parameters);
    t.boundary = boundary;
    return t;
}

} // namespace

const std::vector<TemplateInfo> &fullWaveTemplates()
{
    static const std::vector<TemplateInfo> list = {
        info("patch-probe", "Patch, probe fed",
             "Rectangular patch on a grounded substrate, fed by a probe (lumped port) through the substrate. The resonant length "
             "is along x (x polarisation); the defaults are the openEMS Simple Patch Antenna tutorial (about 2.4 GHz).",
             1e9, 3e9,
             {param("patchX", "Patch length (x, resonant)", "mm", 32, 1, 1e4), param("patchY", "Patch width (y)", "mm", 40, 1, 1e4),
              param("substrateX", "Substrate and ground, x", "mm", 60, 1, 1e5),
              param("substrateY", "Substrate and ground, y", "mm", 60, 1, 1e5),
              param("height", "Substrate height", "mm", 1.524, 0.01, 100), param("epsR", "Permittivity", "", 3.38, 1, 100),
              param("tanD", "Loss tangent", "", 0.001, 0, 1),
              param("feedX", "Feed position (x from the centre)", "mm", -6, -1e4, 1e4, "50 ohm at about a third of the way to the edge"),
              param("resistance", "Port resistance", "ohm", 50, 1, 1e4),
              param("substrateCells", "Cells through the substrate", "", 4, 1, 20)}),
        info("patch-inset", "Patch, inset-fed microstrip",
             "Rectangular patch fed by a 50 ohm microstrip line in an inset notch on its -x edge; the port is at the board edge. "
             "Defaults: 2.45 GHz on 1.6 mm FR-4. x polarisation.",
             1.5e9, 3.5e9,
             {param("patchX", "Patch length (x, resonant)", "mm", 28.8, 1, 1e4), param("patchY", "Patch width (y)", "mm", 37.3, 1, 1e4),
              param("insetDepth", "Inset depth", "mm", 9, 0, 1e4), param("insetGap", "Gap beside the line", "mm", 1, 0.05, 100),
              param("lineWidth", "Line width", "mm", 3.06, 0.05, 100),
              param("substrateX", "Board, x", "mm", 70, 1, 1e5), param("substrateY", "Board, y", "mm", 60, 1, 1e5),
              param("height", "Substrate height", "mm", 1.6, 0.01, 100), param("epsR", "Permittivity", "", 4.4, 1, 100),
              param("tanD", "Loss tangent", "", 0.02, 0, 1), param("resistance", "Port resistance", "ohm", 50, 1, 1e4),
              param("substrateCells", "Cells through the substrate", "", 4, 1, 20)}),
        info("patch-aperture", "Patch, aperture coupled",
             "Patch on top of a two-layer stack, fed through a slot in the ground plane by a microstrip line underneath "
             "(slot along y, line along x, open stub beyond the slot). Defaults: about 2.2 GHz. x polarisation.",
             1.8e9, 3.2e9,
             {param("patchX", "Patch length (x, resonant)", "mm", 40, 1, 1e4), param("patchY", "Patch width (y)", "mm", 30, 1, 1e4),
              param("slotLength", "Slot length (y)", "mm", 16, 0.1, 1e4), param("slotWidth", "Slot width (x)", "mm", 1.5, 0.05, 100),
              param("lineWidth", "Feed line width", "mm", 2.4, 0.05, 100), param("stubLength", "Stub beyond the slot", "mm", 5.5, 0, 1e4),
              param("substrateX", "Board, x", "mm", 80, 1, 1e5), param("substrateY", "Board, y", "mm", 70, 1, 1e5),
              param("feedHeight", "Feed substrate height", "mm", 0.8, 0.01, 100),
              param("feedEpsR", "Feed substrate permittivity", "", 2.2, 1, 100),
              param("patchHeight", "Patch substrate height", "mm", 1.6, 0.01, 100),
              param("patchEpsR", "Patch substrate permittivity", "", 2.2, 1, 100), param("tanD", "Loss tangent", "", 0.001, 0, 1),
              param("resistance", "Port resistance", "ohm", 50, 1, 1e4)}),
        info("dipole", "Dipole (strip)",
             "Half-wave strip dipole along x in free space, fed in a centre gap. Defaults: about 1 GHz. Maximum radiation "
             "broadside (along y and z), x polarisation.",
             0.7e9, 1.3e9,
             {param("length", "Total length", "mm", 132, 1, 1e5), param("width", "Strip width", "mm", 2, 0.01, 1e3),
              param("gap", "Feed gap", "mm", 2, 0.01, 1e3), param("resistance", "Port resistance", "ohm", 73, 1, 1e4)}),
        info("monopole", "Monopole on a ground plane",
             "Thin vertical wire on a square ground plane (z = 0), fed at its base. Defaults: about 1 GHz on a one-wavelength "
             "ground. The beam points near the horizon (theta about 60 deg on a finite ground).",
             0.7e9, 1.3e9,
             {param("height", "Height", "mm", 67.5, 1, 1e5), param("ground", "Ground plane side", "mm", 300, 1, 1e6),
              param("gap", "Feed gap", "mm", 1, 0.05, 100), param("resistance", "Port resistance", "ohm", 36, 1, 1e4)}),
        info("ifa", "PCB inverted-F (IFA)",
             "Printed inverted-F antenna in the ground clearance at the top of a board: a short to ground, a feed and an arm "
             "along x. Defaults: 2.45 GHz on a 40 x 80 mm FR-4 board. Near-omnidirectional, mainly x polarisation.",
             1.8e9, 3.2e9,
             {param("armLength", "Arm length (from the short)", "mm", 17.5, 1, 1e4),
              param("armHeight", "Arm height above the ground edge", "mm", 6, 0.5, 1e3),
              param("feedOffset", "Feed distance from the short", "mm", 3.5, 0.2, 1e3),
              param("shortOffset", "Short from the board's left edge", "mm", 5, 0.5, 1e4),
              param("traceWidth", "Trace width", "mm", 1, 0.05, 100), param("portGap", "Port gap at the ground edge", "mm", 0.5, 0.05, 100),
              param("clearance", "Ground clearance (top of the board)", "mm", 10, 1, 1e4),
              param("boardX", "Board width (x)", "mm", 40, 1, 1e5), param("boardY", "Board length (y)", "mm", 80, 1, 1e5),
              param("height", "Board thickness", "mm", 1.6, 0.01, 100), param("epsR", "Permittivity", "", 4.4, 1, 100),
              param("tanD", "Loss tangent", "", 0.02, 0, 1), param("resistance", "Port resistance", "ohm", 50, 1, 1e4)}),
        info("slot", "Slot in a ground plane",
             "Rectangular slot (along x) in the copper of a board, fed across its centre. Radiates on both sides of the board "
             "(+z and -z); y polarisation (across the slot). Defaults around 2.4 GHz on FR-4; the centre-fed impedance is high.",
             1.5e9, 3.5e9,
             {param("slotLength", "Slot length (x)", "mm", 45, 1, 1e5), param("slotWidth", "Slot width (y)", "mm", 2, 0.05, 100),
              param("boardX", "Board, x", "mm", 100, 1, 1e5), param("boardY", "Board, y", "mm", 80, 1, 1e5),
              param("height", "Board thickness", "mm", 1.6, 0.01, 100), param("epsR", "Permittivity", "", 4.4, 1, 100),
              param("tanD", "Loss tangent", "", 0.02, 0, 1), param("resistance", "Port resistance", "ohm", 600, 1, 1e4)}),
        info("horn", "Pyramidal horn (pin fed)",
             "Rectangular waveguide fed by a pin probe a quarter guide wavelength from the back short, flaring into a pyramidal "
             "horn along +z. The defaults are the openEMS Horn Antenna tutorial (10 to 20 GHz). Polarisation along y (across "
             "the narrow wall).",
             10e9, 20e9,
             {param("waveguideA", "Waveguide width a (x)", "mm", 20, 1, 1e4), param("waveguideB", "Waveguide height b (y)", "mm", 10, 1, 1e4),
              param("wall", "Wall thickness", "mm", 2, 0.1, 100), param("feedLength", "Waveguide length", "mm", 50, 1, 1e5),
              param("hornLength", "Horn length", "mm", 50, 1, 1e5), param("flareAngle", "Flare half-angle", "deg", 20, 0, 60),
              param("pinLength", "Pin length (fraction of b)", "", 0.55, 0.05, 0.95),
              param("pinHalfWidth", "Pin half-width", "mm", 0.5, 0.05, 10), param("portGap", "Port gap at the pin base", "mm", 1, 0.05, 10),
              param("designFrequency", "Back-short design frequency", "GHz", 15, 0.1, 1000),
              param("resistance", "Port resistance", "ohm", 50, 1, 1e4)},
             Boundary::Pml8),
        info("vivaldi", "Vivaldi (tapered slot)",
             "Exponentially tapered slot cut into a copper sheet, fed across the slot near the cavity at its closed end. Ultra-wideband; "
             "end-fire along +x (not +z). Defaults: 3 to 10 GHz; set the height to 0 for no substrate.",
             3e9, 10e9,
             {param("length", "Length (x)", "mm", 80, 5, 1e4), param("width", "Width (y)", "mm", 60, 5, 1e4),
              param("opening", "Opening at the end", "mm", 40, 1, 1e4), param("slotWidth", "Slot width", "mm", 0.5, 0.05, 50),
              param("cavity", "Cavity behind the slot (side; 0: none)", "mm", 10, 0, 1e4),
              param("slotStart", "Slot start (cavity end)", "mm", 14, 0.5, 1e4), param("feedPosition", "Feed position", "mm", 16, 0.5, 1e4),
              param("taperStart", "Taper start", "mm", 20, 1, 1e4), param("height", "Substrate height (below)", "mm", 0.508, 0, 100),
              param("epsR", "Permittivity", "", 3.38, 1, 100), param("tanD", "Loss tangent", "", 0.0027, 0, 1),
              param("resistance", "Port resistance", "ohm", 100, 1, 1e4)}),
        info("stl", "Imported geometry (STL)",
             "Bodies imported from STL files (metal or dielectric each), with a lumped port between two points. Lengths in "
             "millimetres; the port runs along the axis where its two points differ most.",
             1e9, 3e9,
             {param("portStartX", "Port start x", "mm", 0, -1e6, 1e6), param("portStartY", "Port start y", "mm", 0, -1e6, 1e6),
              param("portStartZ", "Port start z", "mm", 0, -1e6, 1e6), param("portStopX", "Port end x", "mm", 0, -1e6, 1e6),
              param("portStopY", "Port end y", "mm", 0, -1e6, 1e6), param("portStopZ", "Port end z", "mm", 1, -1e6, 1e6),
              param("resistance", "Port resistance", "ohm", 50, 1, 1e4)}),
        info("helix", "Helix (axial mode)",
             "Axial-mode helix along +z over a ground disc, fed at its base; circular polarisation. The defaults are the openEMS "
             "Helical Antenna tutorial (2.4 GHz, 9 turns).",
             1.9e9, 2.9e9,
             {param("radius", "Helix radius", "mm", 20, 1, 1e4), param("pitch", "Pitch", "mm", 30, 1, 1e4),
              param("turns", "Turns", "", 9, 1, 50), param("feedHeight", "Feed height", "mm", 3, 0.2, 100),
              param("groundRadius", "Ground disc radius", "mm", 62.5, 1, 1e5),
              param("meshResolution", "Mesh around the helix", "mm", 3, 0.2, 50),
              param("resistance", "Port resistance", "ohm", 120, 1, 1e4)}),
    };
    return list;
}

const TemplateInfo *findTemplate(const QString &id)
{
    for (const TemplateInfo &t : fullWaveTemplates())
        if (t.id == id)
            return &t;
    return nullptr;
}

ParameterValues templateValues(const TemplateInfo &info, const ParameterValues &values)
{
    ParameterValues out;
    for (const TemplateParameter &p : info.parameters) {
        const auto it = values.find(p.id);
        out[p.id] = std::clamp(it != values.end() ? it->second : p.value, p.min, p.max);
    }
    return out;
}

namespace {

bool isPermittivity(const QString &id)
{
    return id == QLatin1String("epsR") || id == QLatin1String("feedEpsR") || id == QLatin1String("patchEpsR");
}

} // namespace

bool hasSubstrate(const TemplateInfo &info)
{
    return std::any_of(info.parameters.begin(), info.parameters.end(),
                       [](const TemplateParameter &p) { return isPermittivity(p.id); });
}

QStringList applySubstrate(const TemplateInfo &info, ParameterValues &values, const SubstrateMaterial &material)
{
    QStringList changed;
    const auto set = [&](const TemplateParameter &p, double value) {
        value = std::clamp(value, p.min, p.max);
        auto it = values.find(p.id);
        if (it == values.end() || it->second != value) {
            values[p.id] = value;
            changed.append(p.id);
        }
    };
    const bool hasEpsR = std::any_of(info.parameters.begin(), info.parameters.end(),
                                     [](const TemplateParameter &p) { return p.id == QLatin1String("epsR"); });
    for (const TemplateParameter &p : info.parameters) {
        if (isPermittivity(p.id)) {
            set(p, material.er);
        } else if (p.id == QLatin1String("tanD")) {
            set(p, material.tand);
        } else if (hasEpsR && p.id == QLatin1String("height") && !material.thicknesses.isEmpty()) {
            const auto it = values.find(p.id);
            const double current = it != values.end() ? it->second : p.value; // mm
            double best = material.thicknesses.first() * 1e3;
            for (double thickness : material.thicknesses) {
                if (std::abs(thickness * 1e3 - current) < std::abs(best - current))
                    best = thickness * 1e3;
            }
            set(p, best);
        }
    }
    return changed;
}

FwModel buildTemplate(const QString &id, const ParameterValues &values, QString *error)
{
    const TemplateInfo *t = findTemplate(id);
    if (!t) {
        setError(error, QStringLiteral("unknown full-wave template \"%1\"").arg(id));
        return {};
    }
    const ParameterValues p = templateValues(*t, values);
    if (id == QLatin1String("patch-probe"))
        return patchProbe(p);
    if (id == QLatin1String("patch-inset"))
        return patchInset(p);
    if (id == QLatin1String("patch-aperture"))
        return patchAperture(p);
    if (id == QLatin1String("dipole"))
        return dipole(p);
    if (id == QLatin1String("monopole"))
        return monopole(p);
    if (id == QLatin1String("ifa"))
        return ifa(p);
    if (id == QLatin1String("slot"))
        return slot(p);
    if (id == QLatin1String("horn"))
        return horn(p);
    if (id == QLatin1String("vivaldi"))
        return vivaldi(p);
    if (id == QLatin1String("helix"))
        return helix(p);
    if (id == QLatin1String("stl")) {
        FwModel m;
        const Vec3 a(v(p, "portStartX"), v(p, "portStartY"), v(p, "portStartZ"));
        const Vec3 b(v(p, "portStopX"), v(p, "portStopY"), v(p, "portStopZ"));
        const Vec3 d = (b - a).cwiseAbs();
        int axis = 0;
        d.maxCoeff(&axis);
        if (d.maxCoeff() <= 0)
            m.warnings << QStringLiteral("the port's two points coincide");
        addPort(m, a, b, axis, v(p, "resistance"));
        m.farFieldCentre = 0.5 * (a + b);
        return m;
    }
    setError(error, QStringLiteral("template \"%1\" is not implemented").arg(id));
    return {};
}

} // namespace lvd::fw

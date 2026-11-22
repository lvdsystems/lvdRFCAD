#include "fullwave/FwModel.h"

#include <algorithm>

namespace lvd::fw {

int FwModel::addMaterial(const FwMaterial &m)
{
    materials.push_back(m);
    return int(materials.size()) - 1;
}

void FwModel::addBox(int material, const Vec3 &a, const Vec3 &b, int priority)
{
    FwPrimitive p;
    p.kind = PrimitiveKind::Box;
    p.material = material;
    p.p1 = a;
    p.p2 = b;
    p.priority = priority;
    primitives.push_back(p);
}

std::vector<Vec3> FwPrimitive::localPoints() const
{
    std::vector<Vec3> pts;
    switch (kind) {
    case PrimitiveKind::Box: {
        const Vec3 lo = p1.cwiseMin(p2), hi = p1.cwiseMax(p2);
        for (int i = 0; i < 8; ++i)
            pts.emplace_back(i & 1 ? hi.x() : lo.x(), i & 2 ? hi.y() : lo.y(), i & 4 ? hi.z() : lo.z());
        break;
    }
    case PrimitiveKind::Cylinder:
        for (const Vec3 &c : {p1, p2})
            for (int i = 0; i < 8; ++i)
                pts.push_back(c + Vec3(i & 1 ? radius : -radius, i & 2 ? radius : -radius, i & 4 ? radius : -radius));
        break;
    case PrimitiveKind::Polygon:
    case PrimitiveKind::Extrusion:
        for (const auto &q : points)
            for (double e : {elevation, elevation + (kind == PrimitiveKind::Extrusion ? length : 0.0)}) {
                Vec3 v;
                v[normal] = e;
                v[(normal + 1) % 3] = q[0];
                v[(normal + 2) % 3] = q[1];
                pts.push_back(v);
            }
        break;
    case PrimitiveKind::Wire:
        for (const Vec3 &v : path) {
            pts.push_back(v - Vec3::Constant(radius));
            pts.push_back(v + Vec3::Constant(radius));
        }
        break;
    case PrimitiveKind::StlFile:
        pts = triangles;
        break;
    }
    return pts;
}

Vec3 FwPrimitive::transformed(const Vec3 &p) const
{
    Vec3 v = p;
    for (const FwTransform &t : transforms) {
        const double a = ant::toRadians(t.value.x());
        switch (t.kind) {
        case FwTransform::Kind::RotateX:
            v = Vec3(v.x(), std::cos(a) * v.y() - std::sin(a) * v.z(), std::sin(a) * v.y() + std::cos(a) * v.z());
            break;
        case FwTransform::Kind::RotateY:
            v = Vec3(std::cos(a) * v.x() + std::sin(a) * v.z(), v.y(), -std::sin(a) * v.x() + std::cos(a) * v.z());
            break;
        case FwTransform::Kind::RotateZ:
            v = Vec3(std::cos(a) * v.x() - std::sin(a) * v.y(), std::sin(a) * v.x() + std::cos(a) * v.y(), v.z());
            break;
        case FwTransform::Kind::Translate:
            v += t.value;
            break;
        case FwTransform::Kind::Scale:
            v *= t.value.x();
            break;
        }
    }
    return v;
}

void FwModel::bounds(Vec3 *lo, Vec3 *hi) const
{
    Vec3 a = Vec3::Constant(INFINITY), b = Vec3::Constant(-INFINITY);
    auto add = [&](const Vec3 &p) {
        a = a.cwiseMin(p);
        b = b.cwiseMax(p);
    };
    for (const FwPrimitive &p : primitives)
        for (const Vec3 &v : p.localPoints())
            add(p.transformed(v));
    for (const FwPort &port : ports) {
        add(port.start);
        add(port.stop);
    }
    if (!std::isfinite(a.x())) {
        a = Vec3::Zero();
        b = Vec3::Zero();
    }
    *lo = a;
    *hi = b;
}

double FwSettings::airMargin() const
{
    const double atStart = airMarginWavelengths * ant::kSpeedOfLight / fStart * 1e3;
    const double halfCentre = 0.5 * ant::kSpeedOfLight / centre() * 1e3;
    return std::min(atStart, halfCentre);
}

} // namespace lvd::fw

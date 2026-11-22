#include "fullwave/Mesher.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace lvd::fw {

std::size_t FwMesh::cells() const
{
    std::size_t n = 1;
    for (const auto &l : lines)
        n *= l.size() > 1 ? l.size() - 1 : 1;
    return n;
}

double FwMesh::smallestCell() const
{
    double m = INFINITY;
    for (const auto &l : lines)
        for (std::size_t i = 1; i < l.size(); ++i)
            m = std::min(m, l[i] - l[i - 1]);
    return m;
}

namespace {

void unique(std::vector<double> &v, double tolerance)
{
    std::sort(v.begin(), v.end());
    std::vector<double> out;
    for (double x : v)
        if (out.empty() || x - out.back() > tolerance)
            out.push_back(x);
    v = std::move(out);
}

} // namespace

std::vector<double> smoothLines(std::vector<double> fixed, const std::function<double(double)> &maxCell, double maxRatio)
{
    unique(fixed, 1e-9);
    if (fixed.size() < 2)
        return fixed;
    // Target cell size: the material limit, and around every fixed line a
    // size that starts at that line's own spacing and grows linearly with
    // the distance (slope ratio - 1, slightly less to stay below the ratio
    // after rounding to whole cells).
    const std::size_t n = fixed.size();
    std::vector<double> local(n);
    for (std::size_t j = 0; j < n; ++j) {
        double h = maxCell(fixed[j]);
        if (j > 0)
            h = std::min(h, fixed[j] - fixed[j - 1]);
        if (j + 1 < n)
            h = std::min(h, fixed[j + 1] - fixed[j]);
        local[j] = h;
    }
    const double slope = (maxRatio - 1) * 0.9;
    auto target = [&](double s) {
        double h = maxCell(s);
        for (std::size_t j = 0; j < n; ++j)
            h = std::min(h, local[j] + slope * std::abs(s - fixed[j]));
        return std::max(h, 1e-9);
    };
    // Fill each gap: as many cells as the integral of 1/h, placed at equal
    // steps of that integral.
    std::vector<double> lines{fixed.front()};
    for (std::size_t i = 1; i < n; ++i) {
        const double a = fixed[i - 1], b = fixed[i];
        constexpr int samples = 400;
        std::vector<double> cumulative(samples + 1, 0.0);
        const double ds = (b - a) / samples;
        for (int k = 0; k < samples; ++k)
            cumulative[std::size_t(k) + 1] = cumulative[std::size_t(k)] + ds / target(a + (k + 0.5) * ds);
        const double total = cumulative.back();
        const int cells = std::max(1, int(std::ceil(total - 1e-6)));
        std::size_t k = 0;
        for (int c = 1; c < cells; ++c) {
            const double goal = total * c / cells;
            while (k < samples && cumulative[k + 1] < goal)
                ++k;
            const double f = (goal - cumulative[k]) / std::max(cumulative[k + 1] - cumulative[k], 1e-300);
            lines.push_back(a + (double(k) + f) * ds);
        }
        lines.push_back(b);
    }
    return lines;
}

FwMesh buildMesh(const FwModel &model, const FwSettings &settings, QStringList *warnings)
{
    FwMesh mesh;
    Vec3 lo, hi;
    model.bounds(&lo, &hi);
    const double lambdaMin = ant::kSpeedOfLight / settings.fStop * 1e3; // mm
    const double airCell = lambdaMin / settings.cellsPerWavelength;
    // Free space around the structure, plus the absorbing layer's own cells.
    const double margin = settings.airMargin() + (settings.boundary == Boundary::Pml8 ? 9 * airCell : 0.0);
    mesh.boxLo = lo - Vec3::Constant(margin);
    mesh.boxHi = hi + Vec3::Constant(margin);
    const double thirds = airCell / 2; // edge resolution of refined metal edges

    for (int axis = 0; axis < 3; ++axis) {
        std::vector<double> fixed{mesh.boxLo[axis], mesh.boxHi[axis]};
        auto add = [&](double v) { fixed.push_back(v); };
        // Metal edges with the thirds rule: 1/3 of the edge cell inside the metal.
        auto edgeThirds = [&](double edge, bool interiorAbove) {
            if (interiorAbove) {
                add(edge + thirds / 3);
                add(edge - 2 * thirds / 3);
            } else {
                add(edge - thirds / 3);
                add(edge + 2 * thirds / 3);
            }
        };

        for (const FwPrimitive &p : model.primitives) {
            const bool metal = p.material >= 0 && p.material < int(model.materials.size()) && model.materials[std::size_t(p.material)].metal;
            // Transformed primitives and long wires: only their extent on this axis.
            if (!p.transforms.empty() || (p.kind == PrimitiveKind::Wire && p.path.size() > 8)) {
                double mn = INFINITY, mx = -INFINITY;
                for (const Vec3 &v : p.localPoints()) {
                    const Vec3 t = p.transformed(v);
                    mn = std::min(mn, t[axis]);
                    mx = std::max(mx, t[axis]);
                }
                if (std::isfinite(mn)) {
                    add(mn);
                    add(mx);
                }
                continue;
            }
            switch (p.kind) {
            case PrimitiveKind::Box: {
                const double a = std::min(p.p1[axis], p.p2[axis]), b = std::max(p.p1[axis], p.p2[axis]);
                // Thin metal sheets: refine the in-plane edges, not the sheet's own plane.
                const bool sheetNormal = std::abs(b - a) < 1e-9;
                if (metal && p.edgeThirds && !sheetNormal) {
                    edgeThirds(a, true);
                    edgeThirds(b, false);
                } else {
                    add(a);
                    add(b);
                }
                break;
            }
            case PrimitiveKind::Cylinder: {
                const Vec3 d = p.p2 - p.p1;
                const bool along = std::abs(d[axis]) > 1e-9;
                if (along) {
                    add(p.p1[axis]);
                    add(p.p2[axis]);
                } else {
                    add(p.p1[axis]);
                    add(p.p1[axis] - p.radius);
                    add(p.p1[axis] + p.radius);
                }
                break;
            }
            case PrimitiveKind::Polygon:
            case PrimitiveKind::Extrusion: {
                if (axis == p.normal) {
                    add(p.elevation);
                    if (p.kind == PrimitiveKind::Extrusion)
                        add(p.elevation + p.length);
                    break;
                }
                const int k = axis == (p.normal + 1) % 3 ? 0 : 1;
                double mn = INFINITY, mx = -INFINITY;
                const std::size_t n = p.points.size();
                for (std::size_t i = 0; i < n; ++i) {
                    const auto &u = p.points[i];
                    const auto &v = p.points[(i + 1) % n];
                    mn = std::min(mn, u[std::size_t(k)]);
                    mx = std::max(mx, u[std::size_t(k)]);
                    // Edges parallel to the other in-plane axis sit on a mesh line.
                    if (std::abs(u[std::size_t(k)] - v[std::size_t(k)]) < 1e-9 && std::abs(u[std::size_t(1 - k)] - v[std::size_t(1 - k)]) > 1e-9)
                        add(u[std::size_t(k)]);
                }
                if (n) {
                    add(mn);
                    add(mx);
                }
                break;
            }
            case PrimitiveKind::Wire:
                for (const Vec3 &v : p.path)
                    add(v[axis]);
                break;
            case PrimitiveKind::StlFile:
                if (!p.triangles.empty()) {
                    double mn = INFINITY, mx = -INFINITY;
                    for (const Vec3 &v : p.triangles) {
                        mn = std::min(mn, v[axis]);
                        mx = std::max(mx, v[axis]);
                    }
                    add(mn);
                    add(mx);
                }
                break;
            }
        }
        for (const FwPort &port : model.ports) {
            add(port.start[axis]);
            add(port.stop[axis]);
        }
        for (double v : model.extraLines[std::size_t(axis)])
            add(v);
        // Keep everything inside the box.
        for (double &v : fixed)
            v = std::clamp(v, mesh.boxLo[axis], mesh.boxHi[axis]);
        unique(fixed, 1e-6);

        // Largest cell at a position: the strictest dielectric covering it.
        std::vector<std::pair<double, std::pair<double, double>>> dielectrics; // (cell, [a, b])
        for (const FwPrimitive &p : model.primitives) {
            if (p.material < 0 || p.material >= int(model.materials.size()))
                continue;
            const FwMaterial &m = model.materials[std::size_t(p.material)];
            if (m.metal || m.epsR <= 1.0001)
                continue;
            double a = INFINITY, b = -INFINITY;
            if (p.kind == PrimitiveKind::Box) {
                a = std::min(p.p1[axis], p.p2[axis]);
                b = std::max(p.p1[axis], p.p2[axis]);
            } else if (p.kind == PrimitiveKind::Extrusion || p.kind == PrimitiveKind::Polygon) {
                if (axis == p.normal) {
                    a = std::min(p.elevation, p.elevation + p.length);
                    b = std::max(p.elevation, p.elevation + p.length);
                } else {
                    const int k = axis == (p.normal + 1) % 3 ? 0 : 1;
                    for (const auto &q : p.points) {
                        a = std::min(a, q[std::size_t(k)]);
                        b = std::max(b, q[std::size_t(k)]);
                    }
                }
            } else if (p.kind == PrimitiveKind::Cylinder) {
                a = std::min(p.p1[axis], p.p2[axis]) - p.radius;
                b = std::max(p.p1[axis], p.p2[axis]) + p.radius;
            }
            if (b > a)
                dielectrics.push_back({airCell / std::sqrt(m.epsR), {a, b}});
        }
        for (const FwRefinement &r : model.refinements) {
            add(std::clamp(r.lo[axis], mesh.boxLo[axis], mesh.boxHi[axis]));
            add(std::clamp(r.hi[axis], mesh.boxLo[axis], mesh.boxHi[axis]));
        }
        unique(fixed, 1e-6);
        auto maxCell = [&](double s) {
            double h = airCell;
            for (const auto &[cell, range] : dielectrics)
                if (s >= range.first - 1e-9 && s <= range.second + 1e-9)
                    h = std::min(h, cell);
            for (const FwRefinement &r : model.refinements)
                if (s >= std::min(r.lo[axis], r.hi[axis]) - 1e-9 && s <= std::max(r.lo[axis], r.hi[axis]) + 1e-9)
                    h = std::min(h, r.maxCell);
            return h;
        };
        // Merge geometry lines that nearly coincide (rotated walls, thin
        // plates): a cell far below the local size would only shrink the time
        // step. Template lines, ports and the box ends are kept.
        std::vector<double> keep{mesh.boxLo[axis], mesh.boxHi[axis]};
        for (double l : model.extraLines[std::size_t(axis)])
            keep.push_back(l);
        for (const FwPort &port : model.ports) {
            keep.push_back(port.start[axis]);
            keep.push_back(port.stop[axis]);
        }
        auto isKept = [&](double x) {
            return std::any_of(keep.begin(), keep.end(), [x](double k) { return std::abs(k - x) < 1e-6; });
        };
        std::vector<double> merged;
        for (double x : fixed) {
            if (!merged.empty() && x - merged.back() < maxCell(0.5 * (x + merged.back())) / 8) {
                if (isKept(merged.back()))
                    continue;           // keep the protected line
                if (isKept(x)) {
                    merged.back() = x;  // replace by the protected one
                    continue;
                }
                merged.back() = 0.5 * (merged.back() + x);
                continue;
            }
            merged.push_back(x);
        }
        mesh.lines[std::size_t(axis)] = smoothLines(merged, maxCell, settings.maxRatio);
    }
    if (warnings) {
        const double smallest = mesh.smallestCell();
        if (smallest < airCell / 200)
            *warnings << QStringLiteral("the smallest cell is %1 mm: the time step will be very small").arg(smallest, 0, 'g', 3);
        if (mesh.cells() > 50e6)
            *warnings << QStringLiteral("the mesh has %1 million cells: expect a long run").arg(double(mesh.cells()) / 1e6, 0, 'f', 1);
    }
    return mesh;
}

} // namespace lvd::fw

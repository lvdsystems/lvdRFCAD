#include "plot3d/Scene3D.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lvd {

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

bool isPole(double thetaDeg)
{
    return std::abs(thetaDeg) < 1e-6 || std::abs(thetaDeg - 180.0) < 1e-6;
}

// Grid line stride so the wireframe has lines about every 15 degrees whatever
// the sampling.
int gridStride(const std::vector<double> &angles, int count)
{
    if (count < 2)
        return 1;
    const double step = std::abs(angles[static_cast<std::size_t>(count - 1)] - angles.front()) / (count - 1);
    if (!(step > 0))
        return 1;
    return std::max(1, static_cast<int>(std::lround(15.0 / step)));
}

} // namespace

QVector3D sphericalDirection(double thetaDeg, double phiDeg)
{
    const double t = thetaDeg * kDeg;
    const double p = phiDeg * kDeg;
    return QVector3D(static_cast<float>(std::sin(t) * std::cos(p)), static_cast<float>(std::sin(t) * std::sin(p)),
                     static_cast<float>(std::cos(t)));
}

PatternMesh buildPatternMesh(const SphericalPattern &pattern, double dynamicRangeDb, double radius, ColorMap map)
{
    PatternMesh mesh;
    mesh.radius = radius;
    const int nTheta = static_cast<int>(pattern.theta.size());
    const int nPhi = static_cast<int>(pattern.phi.size());
    if (nTheta == 0 || nPhi == 0 || pattern.valuesDb.size() != static_cast<std::size_t>(nTheta) * static_cast<std::size_t>(nPhi))
        return mesh;

    double maxDb = -INFINITY;
    for (double v : pattern.valuesDb)
        if (std::isfinite(v))
            maxDb = std::max(maxDb, v);
    if (!std::isfinite(maxDb))
        return mesh;
    const double range = dynamicRangeDb > 0 ? dynamicRangeDb : 40.0;
    mesh.maxDb = maxDb;
    mesh.floorDb = maxDb - range;

    // Seam: a full turn either repeats the first column at +360 (drop it and
    // wrap) or stops one step short (wrap onto the first column).
    int columns = nPhi;
    bool wrap = false;
    if (nPhi >= 3) {
        const double span = pattern.phi.back() - pattern.phi.front();
        const double step = span / (nPhi - 1);
        if (std::abs(span - 360.0) < 1e-3) {
            columns = nPhi - 1;
            wrap = true;
        } else if (std::abs(span + step - 360.0) < 0.01 * std::abs(step) + 1e-6) {
            wrap = true;
        }
    }
    mesh.rows = nTheta;
    mesh.columns = columns;
    mesh.seamClosed = wrap;

    const std::size_t count = static_cast<std::size_t>(nTheta) * static_cast<std::size_t>(columns);
    mesh.positions.resize(count);
    mesh.normals.assign(count, QVector3D());
    mesh.colors.resize(count);
    mesh.valuesDb.resize(count);
    mesh.thetaDeg.resize(count);
    mesh.phiDeg.resize(count);
    std::vector<QVector3D> directions(count);
    for (int i = 0; i < nTheta; ++i) {
        for (int j = 0; j < columns; ++j) {
            const std::size_t k = mesh.vertexIndex(i, j);
            const double theta = pattern.theta[static_cast<std::size_t>(i)];
            const double phi = pattern.phi[static_cast<std::size_t>(j)];
            const double v = pattern.valuesDb[static_cast<std::size_t>(i) * static_cast<std::size_t>(nPhi) + static_cast<std::size_t>(j)];
            const double t = std::isfinite(v) ? std::clamp((v - mesh.floorDb) / range, 0.0, 1.0) : 0.0;
            directions[k] = sphericalDirection(theta, phi);
            mesh.positions[k] = directions[k] * static_cast<float>(t * radius);
            mesh.colors[k] = std::isfinite(v) ? colorMapColor(map, t) : QColor(128, 128, 128);
            mesh.valuesDb[k] = std::isfinite(v) ? v : NAN;
            mesh.thetaDeg[k] = static_cast<float>(theta);
            mesh.phiDeg[k] = static_cast<float>(phi);
        }
    }

    auto valid = [&](std::size_t k) { return std::isfinite(mesh.valuesDb[k]); };
    const float minArea = static_cast<float>(1e-12 * radius * radius);
    auto addTriangle = [&](std::size_t a, std::size_t b, std::size_t c) {
        if (!valid(a) || !valid(b) || !valid(c))
            return;
        const QVector3D n = QVector3D::crossProduct(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
        // Degenerate triangles (poles, samples below the floor) only cost time.
        if (n.length() <= minArea)
            return;
        mesh.normals[a] += n;
        mesh.normals[b] += n;
        mesh.normals[c] += n;
        mesh.triangles.insert(mesh.triangles.end(), {static_cast<quint32>(a), static_cast<quint32>(b), static_cast<quint32>(c)});
    };
    const int quadColumns = wrap ? columns : columns - 1;
    for (int i = 0; i + 1 < nTheta; ++i) {
        for (int j = 0; j < quadColumns; ++j) {
            const int j2 = (j + 1) % columns;
            // Theta grows towards -z and phi counter-clockwise: this winding
            // makes the face normals point outwards.
            const std::size_t a = mesh.vertexIndex(i, j);
            const std::size_t b = mesh.vertexIndex(i + 1, j);
            const std::size_t c = mesh.vertexIndex(i + 1, j2);
            const std::size_t d = mesh.vertexIndex(i, j2);
            addTriangle(a, b, c);
            addTriangle(a, c, d);
        }
    }

    // All vertices of a pole row are one point: give them one normal, or the
    // lobe tip shows a lighting pinwheel.
    for (int i = 0; i < nTheta; ++i) {
        if (!isPole(pattern.theta[static_cast<std::size_t>(i)]) || columns < 2)
            continue;
        QVector3D sum;
        for (int j = 0; j < columns; ++j)
            sum += mesh.normals[mesh.vertexIndex(i, j)];
        for (int j = 0; j < columns; ++j)
            mesh.normals[mesh.vertexIndex(i, j)] = sum;
    }
    for (std::size_t k = 0; k < count; ++k) {
        if (mesh.normals[k].lengthSquared() > 0)
            mesh.normals[k].normalize();
        else
            mesh.normals[k] = directions[k];
    }

    // Wireframe on a subset of the grid lines.
    const int strideTheta = gridStride(pattern.theta, nTheta);
    const int stridePhi = gridStride(pattern.phi, nPhi);
    auto addSegment = [&](std::size_t a, std::size_t b) {
        if (valid(a) && valid(b))
            mesh.gridLines.insert(mesh.gridLines.end(), {static_cast<quint32>(a), static_cast<quint32>(b)});
    };
    if (columns >= 2 && nTheta >= 2) {
        for (int i = 0; i < nTheta; ++i) {
            if (i % strideTheta != 0 && i != nTheta - 1)
                continue;
            if (isPole(pattern.theta[static_cast<std::size_t>(i)]))
                continue;
            for (int j = 0; j < quadColumns; ++j)
                addSegment(mesh.vertexIndex(i, j), mesh.vertexIndex(i, (j + 1) % columns));
        }
        for (int j = 0; j < columns; ++j) {
            if (j % stridePhi != 0)
                continue;
            for (int i = 0; i + 1 < nTheta; ++i)
                addSegment(mesh.vertexIndex(i, j), mesh.vertexIndex(i + 1, j));
        }
    }

    // A single cut (one phi or one theta) has no surface: show it as a curve.
    if (columns == 1 && nTheta >= 2) {
        for (int i = 0; i < nTheta; ++i)
            mesh.curve.push_back(static_cast<quint32>(mesh.vertexIndex(i, 0)));
    } else if (nTheta == 1 && columns >= 2) {
        for (int j = 0; j < columns; ++j)
            mesh.curve.push_back(static_cast<quint32>(mesh.vertexIndex(0, j)));
        if (wrap)
            mesh.curve.push_back(0);
    }
    return mesh;
}

// ---- Camera

QVector3D Camera3D::eyeDirection() const
{
    const double az = azimuthDeg * kDeg;
    const double el = std::clamp(elevationDeg, -kMaxElevation, kMaxElevation) * kDeg;
    return QVector3D(static_cast<float>(std::cos(el) * std::cos(az)), static_cast<float>(std::cos(el) * std::sin(az)),
                     static_cast<float>(std::sin(el)));
}

double Camera3D::distance() const
{
    // The bounding sphere just fits the field of view, with a small margin.
    const double half = 0.5 * fieldOfViewDeg * kDeg;
    const double r = sceneRadius > 0 ? sceneRadius : 1.0;
    return 1.08 * r / std::sin(half) / std::max(zoom, 1e-6);
}

QMatrix4x4 Camera3D::viewMatrix() const
{
    QMatrix4x4 m;
    m.lookAt(eye(), target, QVector3D(0, 0, 1));
    return m;
}

QMatrix4x4 Camera3D::projectionMatrix(double aspect) const
{
    // The field of view applies to the shorter side, so a tall viewport does
    // not crop the pattern sideways.
    double fovY = fieldOfViewDeg;
    if (aspect > 0 && aspect < 1)
        fovY = 2.0 * std::atan(std::tan(0.5 * fieldOfViewDeg * kDeg) / aspect) / kDeg;
    const double d = distance();
    const double r = sceneRadius > 0 ? sceneRadius : 1.0;
    const double farPlane = d + 4.0 * r + target.length();
    const double nearPlane = std::max(d - 4.0 * r - target.length(), farPlane * 1e-4);
    QMatrix4x4 m;
    m.perspective(static_cast<float>(fovY), static_cast<float>(aspect > 0 ? aspect : 1.0), static_cast<float>(nearPlane),
                  static_cast<float>(farPlane));
    return m;
}

bool Camera3D::project(const QVector3D &point, const QRectF &viewport, QPointF *pixel, float *distance) const
{
    const QMatrix4x4 view = viewMatrix();
    const QVector4D eyeSpace = view * QVector4D(point, 1.0f);
    if (distance)
        *distance = -eyeSpace.z();
    if (eyeSpace.z() >= 0)
        return false;
    const double aspect = viewport.height() > 0 ? viewport.width() / viewport.height() : 1.0;
    const QVector4D clip = projectionMatrix(aspect) * eyeSpace;
    if (clip.w() <= 0)
        return false;
    const double x = clip.x() / clip.w();
    const double y = clip.y() / clip.w();
    if (pixel)
        *pixel = QPointF(viewport.left() + (x + 1.0) * 0.5 * viewport.width(), viewport.top() + (1.0 - y) * 0.5 * viewport.height());
    return true;
}

QPointF Camera3D::project(const QVector3D &point, const QRectF &viewport) const
{
    QPointF pixel;
    if (!project(point, viewport, &pixel))
        return QPointF(NAN, NAN);
    return pixel;
}

void Camera3D::ray(const QPointF &pixel, const QRectF &viewport, QVector3D *origin, QVector3D *direction) const
{
    const double aspect = viewport.height() > 0 ? viewport.width() / viewport.height() : 1.0;
    const QMatrix4x4 inverse = (projectionMatrix(aspect) * viewMatrix()).inverted();
    const float x = static_cast<float>(2.0 * (pixel.x() - viewport.left()) / std::max(viewport.width(), 1.0) - 1.0);
    const float y = static_cast<float>(1.0 - 2.0 * (pixel.y() - viewport.top()) / std::max(viewport.height(), 1.0));
    const QVector3D nearPoint = inverse.map(QVector3D(x, y, -1.0f));
    const QVector3D farPoint = inverse.map(QVector3D(x, y, 1.0f));
    if (origin)
        *origin = nearPoint;
    if (direction)
        *direction = (farPoint - nearPoint).normalized();
}

void Camera3D::orbit(double deltaAzimuthDeg, double deltaElevationDeg)
{
    azimuthDeg = std::remainder(azimuthDeg + deltaAzimuthDeg, 360.0);
    elevationDeg = std::clamp(elevationDeg + deltaElevationDeg, -kMaxElevation, kMaxElevation);
}

void Camera3D::pan(const QPointF &delta, const QRectF &viewport)
{
    if (viewport.height() <= 0)
        return;
    const double aspect = viewport.width() / viewport.height();
    double fovY = fieldOfViewDeg;
    if (aspect > 0 && aspect < 1)
        fovY = 2.0 * std::atan(std::tan(0.5 * fieldOfViewDeg * kDeg) / aspect) / kDeg;
    const double perPixel = 2.0 * distance() * std::tan(0.5 * fovY * kDeg) / viewport.height();
    const QMatrix4x4 view = viewMatrix();
    const QVector3D right = view.row(0).toVector3D();
    const QVector3D up = view.row(1).toVector3D();
    target -= right * static_cast<float>(delta.x() * perPixel);
    target += up * static_cast<float>(delta.y() * perPixel);
}

} // namespace lvd

#pragma once

#include "plot/ColorMaps.h"

#include <QColor>
#include <QMatrix4x4>
#include <QPointF>
#include <QRectF>
#include <QVector3D>
#include <QtTypes>

#include <cmath>
#include <vector>

namespace lvd {

// A far-field pattern sampled on a theta/phi grid, in degrees: theta from +z
// (0..180), phi from +x towards +y (-180..180 or 0..360, or a sector).
// valuesDb[iTheta * phi.size() + iPhi], in dB (dBi); NaN marks missing samples.
struct SphericalPattern {
    std::vector<double> theta;
    std::vector<double> phi;
    std::vector<double> valuesDb;
};

// Scene geometry is in metres, z up.
struct Scene3DPoint {
    QVector3D position;
    QColor color;      // invalid: the theme's text colour
    float size = 8.0f; // diameter in pixels
};

struct Scene3DLine {
    std::vector<QVector3D> points; // polyline
    QColor color;                  // invalid: the theme's text colour
    float width = 1.5f;            // pixels
};

// Triangle mesh (antenna geometry). Flat shaded: each triangle gets its face
// normal, so STL-like meshes need no consistent winding or shared vertices.
struct Scene3DMesh {
    std::vector<QVector3D> vertices;
    std::vector<quint32> indices; // three per triangle
    QColor color;
    float opacity = 1.0f;
};

// The triangulated pattern surface: one vertex per (theta, phi) sample, with a
// duplicated 360 degree column dropped so the seam closes on shared vertices.
struct PatternMesh {
    std::vector<QVector3D> positions;
    std::vector<QVector3D> normals;  // smooth, outward
    std::vector<QColor> colors;
    std::vector<double> valuesDb;    // NaN where the sample is missing
    std::vector<float> thetaDeg;
    std::vector<float> phiDeg;
    std::vector<quint32> triangles;  // three indices per triangle
    std::vector<quint32> gridLines;  // index pairs: constant theta and phi lines
    std::vector<quint32> curve;      // line strip when the grid is a single cut
    int rows = 0;                    // theta samples
    int columns = 0;                 // phi samples used (after the seam column)
    bool seamClosed = false;         // phi wraps around the z axis
    double maxDb = NAN;
    double floorDb = NAN;            // maxDb - dynamic range: radius 0
    double radius = 1.0;             // radius at maxDb

    bool isEmpty() const { return positions.empty(); }
    std::size_t vertexIndex(int row, int column) const { return static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column); }
};

// Unit vector of the direction (theta, phi) in degrees.
QVector3D sphericalDirection(double thetaDeg, double phiDeg);

// Radius = (dB - floor) / range * radius, clamped at 0 below the floor;
// colour = the colour map at the same fraction.
PatternMesh buildPatternMesh(const SphericalPattern &pattern, double dynamicRangeDb, double radius, ColorMap map);

// Orbit camera around a target, z up, perspective projection. The distance is
// chosen so a sphere of sceneRadius around the target fills the view at zoom 1.
struct Camera3D {
    double azimuthDeg = 45.0;   // eye direction measured from +x towards +y
    double elevationDeg = 30.0; // above the xy plane, clamped to +-89
    double zoom = 1.0;
    QVector3D target;
    double sceneRadius = 1.0;
    double fieldOfViewDeg = 30.0; // of the shorter viewport side

    static constexpr double kMaxElevation = 89.0;

    QVector3D eyeDirection() const; // unit vector from the target to the eye
    double distance() const;
    QVector3D eye() const { return target + eyeDirection() * static_cast<float>(distance()); }
    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projectionMatrix(double aspect) const;

    // Pixel position of a scene point in a viewport rectangle; false when the
    // point is behind the eye. distance: from the eye along the view direction.
    bool project(const QVector3D &point, const QRectF &viewport, QPointF *pixel, float *distance = nullptr) const;
    QPointF project(const QVector3D &point, const QRectF &viewport) const; // NaN behind the eye
    // Ray through a pixel, for picking.
    void ray(const QPointF &pixel, const QRectF &viewport, QVector3D *origin, QVector3D *direction) const;

    void orbit(double deltaAzimuthDeg, double deltaElevationDeg);
    // Moves the target so the scene follows a drag of delta pixels.
    void pan(const QPointF &delta, const QRectF &viewport);
};

} // namespace lvd

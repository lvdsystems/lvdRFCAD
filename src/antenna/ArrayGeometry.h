#pragma once

#include "antenna/AntennaTypes.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace lvd::ant {

// Array geometry (requirements AN-1). Planar arrays lie in the xy-plane and
// radiate towards +z; a linear array lies along x. A cylinder has its axis
// along z and its elements face outward; its broadside is +x.
enum class LatticeKind { Linear, Rectangular, Triangular, Circular, Cylinder, Custom };

QString latticeId(LatticeKind kind);
QString latticeName(LatticeKind kind);
std::vector<LatticeKind> latticeKinds();

struct RingSpec {
    double radius = 1.0;   // in the geometry's length unit
    int count = 8;
    double rotationDeg = 0; // angle of the first element
};

// One row of a custom element table. Orientation: the element's boresight
// (normal) and a rotation of its polarisation about that normal; a zero
// normal means +z.
struct CustomElement {
    double x = 0, y = 0, z = 0;
    double nx = 0, ny = 0, nz = 1;
    double rotationDeg = 0;
    double amplitudeDb = 0;  // used by the "custom" taper
    double phaseDeg = 0;
    bool enabled = true;
};

struct GeometrySpec {
    LatticeKind kind = LatticeKind::Rectangular;
    // Length unit of the spacings, radii and positions below: "lambda" (at
    // the design frequency) or "m".
    QString unit = QStringLiteral("lambda");
    int nx = 8, ny = 8;           // linear, rectangular, triangular
    double dx = 0.5, dy = 0.5;    // triangular: dy is the row spacing; odd rows shift by dx/2
    // Truncation of rectangular and triangular lattices: "none", "circle", "ellipse".
    QString aperture = QStringLiteral("none");
    double apertureX = 0, apertureY = 0; // semi-axes (circle: apertureX); 0: the lattice extent
    std::vector<RingSpec> rings{RingSpec{}}; // circular: concentric rings
    bool centreElement = false;
    // Cylinder: columns around the axis spanning arcDeg, rows along z.
    double cylinderRadius = 2.0;
    int columns = 16, rows = 4;
    double arcDeg = 120;
    double dz = 0.5;
    std::vector<CustomElement> custom;

    QJsonObject toJson() const;
    static GeometrySpec fromJson(const QJsonObject &object);
};

// A placed element in the array frame (metres).
struct ArrayElement {
    Vec3 position = Vec3::Zero();
    // Columns: the element's own x (polarisation), y and z (boresight) axes.
    Mat3 orientation = Mat3::Identity();
    int row = -1, column = -1; // lattice indices where they exist
    bool enabled = true;
    double amplitudeDb = 0;    // custom taper values
    double phaseDeg = 0;
};

// Periodic lattice basis (metres) for grating-lobe analysis; absent for
// non-periodic geometries. A linear array has one vector.
struct Lattice {
    std::vector<Eigen::Vector2d> basis;
};

struct ArrayLayout {
    LatticeKind kind = LatticeKind::Custom;
    std::vector<ArrayElement> elements;
    std::optional<Lattice> lattice;
    double pitchX = 0, pitchY = 0; // typical spacing along each taper axis (m)
    double cylinderRadius = 0;     // cylinder: radius (m)
    bool identicalOrientation = true; // all elements share the identity orientation
    QStringList warnings;
};

// Builds the element positions; lengths in "lambda" use the design frequency.
ArrayLayout buildLayout(const GeometrySpec &spec, double designFrequency);

// The elements of any geometry as a custom table, in the spec's length unit
// (positions, normals, rotations and on/off; amplitude and phase 0), so a
// lattice can be edited element by element.
std::vector<CustomElement> toCustomElements(const GeometrySpec &spec, double designFrequency);

// Orientation matrix from a boresight normal and a rotation (degrees) of the
// element's x axis about it. The x axis starts as the projection of the
// global x axis (or y when the normal is along x).
Mat3 orientationFromNormal(const Vec3 &normal, double rotationDeg);

} // namespace lvd::ant

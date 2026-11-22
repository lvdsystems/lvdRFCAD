#pragma once

#include "antenna/AntennaTypes.h"

#include <QColor>
#include <QString>
#include <QStringList>

#include <array>
#include <vector>

namespace lvd::fw {

using ant::Vec3;

// A full-wave model for openEMS: materials, primitives and ports in
// millimetres (DeltaUnit 1e-3). Built from a template or from imported
// geometry; written to openEMS XML by OpenEmsXml.

struct FwMaterial {
    QString name;
    bool metal = true;     // perfect electric conductor
    double epsR = 1;       // dielectrics
    double tanD = 0;       // loss tangent, converted to a conductivity at the centre frequency
    QColor color;
};

enum class PrimitiveKind { Box, Cylinder, Polygon, Extrusion, Wire, StlFile };

// A transformation applied to a primitive, in order (as CSXCAD's
// Transformation: Rotate_X/Y/Z in degrees here, Translate, Scale).
struct FwTransform {
    enum class Kind { RotateX, RotateY, RotateZ, Translate, Scale } kind = Kind::Translate;
    Vec3 value = Vec3::Zero(); // rotations: angle in degrees in x; scale: factor in x
};

struct FwPrimitive {
    PrimitiveKind kind = PrimitiveKind::Box;
    int material = 0;
    int priority = 10;
    Vec3 p1 = Vec3::Zero(), p2 = Vec3::Zero(); // box corners; cylinder axis ends
    double radius = 0;                          // cylinder; wire (0: thin wire)
    int normal = 2;                             // polygon and extrusion: normal axis 0, 1, 2
    double elevation = 0;                       // polygon and extrusion: position on the normal axis
    double length = 0;                          // extrusion length along the normal
    std::vector<std::array<double, 2>> points;  // polygon outline (the two other axes, in order)
    std::vector<Vec3> path;                     // wire vertices
    QString file;                               // STL file (absolute), in millimetres
    double stlScale = 1;                        // file units to millimetres
    // Triangles of the STL body for meshing hints and the 3D view (millimetres).
    std::vector<Vec3> triangles;
    // Metal edges in the plane get the "thirds" rule (lines at 1/3 inside and
    // 2/3 outside an edge cell) for accurate edge currents (patch edges).
    bool edgeThirds = false;
    std::vector<FwTransform> transforms;

    // The primitive's own points (before transforms) for bounds and meshing.
    std::vector<Vec3> localPoints() const;
    // Applies the transforms to a point.
    Vec3 transformed(const Vec3 &p) const;
};

// A region where the mesh is finer than the material requires (millimetres).
struct FwRefinement {
    Vec3 lo = Vec3::Zero(), hi = Vec3::Zero();
    double maxCell = 1;
};

// A lumped port between start and stop (along one axis) with resistance r.
struct FwPort {
    int number = 1;
    Vec3 start = Vec3::Zero(), stop = Vec3::Zero();
    int axis = 2;
    double resistance = 50;
    bool excite = true;
};

struct FwModel {
    std::vector<FwMaterial> materials;
    std::vector<FwPrimitive> primitives;
    std::vector<FwPort> ports;
    // Extra mesh lines (millimetres) per axis, e.g. through a thin substrate.
    std::array<std::vector<double>, 3> extraLines;
    std::vector<FwRefinement> refinements;
    // Far-field phase centre (millimetres).
    Vec3 farFieldCentre = Vec3::Zero();
    QStringList warnings;

    int addMaterial(const FwMaterial &m);
    void addBox(int material, const Vec3 &a, const Vec3 &b, int priority = 10);
    // Bounding box of all primitives and ports (millimetres).
    void bounds(Vec3 *lo, Vec3 *hi) const;
};

enum class Boundary { Mur, Pml8 };

struct FwSettings {
    double fStart = 1e9, fStop = 3e9;  // Hz: the Gaussian excitation covers this band
    double cellsPerWavelength = 20;    // at fStop, in each material
    double maxRatio = 1.4;             // neighbouring cell size ratio
    double endCriteriaDb = -40;        // energy decay that ends the run
    int maxTimesteps = 30000;
    Boundary boundary = Boundary::Mur;
    double airMarginWavelengths = 0.25; // free space around the structure: this many wavelengths at fStart,
                                        // at most half a wavelength at the centre frequency
    double farFieldStepDeg = 3;        // theta and phi resolution of the far field
    std::vector<double> farFieldFrequencies; // empty: the S11 minimum and the band centre
    int frequencyPoints = 401;

    double centre() const { return (fStart + fStop) / 2; }
    double airMargin() const; // millimetres
    double halfWidth() const { return (fStop - fStart) / 2; }
};

} // namespace lvd::fw

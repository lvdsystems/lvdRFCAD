#pragma once

#include "fullwave/FwModel.h"

#include <array>
#include <functional>
#include <vector>

namespace lvd::fw {

// A rectilinear FDTD mesh (millimetres) and the simulation box.
struct FwMesh {
    std::array<std::vector<double>, 3> lines;
    Vec3 boxLo = Vec3::Zero(), boxHi = Vec3::Zero();

    std::size_t cells() const;
    double smallestCell() const;
};

// Automatic mesh (requirement AN-9): fixed lines at the structure (box
// faces, polygon edges along the axes, cylinder and wire coordinates,
// ports, the template's extra lines), the "thirds" rule at refined metal
// edges, the simulation box around the structure, then a fill so that no
// cell exceeds wavelength / cellsPerWavelength in its material and
// neighbouring cells grow by at most maxRatio.
FwMesh buildMesh(const FwModel &model, const FwSettings &settings, QStringList *warnings = nullptr);

// Fills one axis between sorted fixed lines; maxCell(s) gives the largest
// allowed cell at position s. Exposed for tests.
std::vector<double> smoothLines(std::vector<double> fixed, const std::function<double(double)> &maxCell, double maxRatio);

} // namespace lvd::fw

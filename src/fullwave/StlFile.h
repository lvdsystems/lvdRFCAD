#pragma once

#include "antenna/AntennaTypes.h"

#include <QString>

#include <optional>
#include <vector>

namespace lvd::fw {

// Triangles of an STL file (ASCII or binary), three vertices per triangle,
// multiplied by `scale` (file units to millimetres).
std::optional<std::vector<ant::Vec3>> readStl(const QString &path, double scale = 1.0, QString *error = nullptr);

} // namespace lvd::fw

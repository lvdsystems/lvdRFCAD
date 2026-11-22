#pragma once

#include "fullwave/FwModel.h"
#include "fullwave/Mesher.h"

#include <QString>

#include <vector>

namespace lvd::fw {

// Names of the files openEMS writes for port n (voltage and current probes).
QString portVoltageFile(int port);
QString portCurrentFile(int port);
// Prefix of the near-field dump files (nf2ff_E_0.h5 ... nf2ff_H_5.h5).
inline const char *kNearFieldPrefix = "nf2ff";

// The openEMS simulation file: FDTD settings (Gaussian excitation over the
// band, boundaries, end criterion), the structure, the lumped ports with
// their probes, the near-field box for the far field, and the mesh.
QString openEmsXml(const FwModel &model, const FwMesh &mesh, const FwSettings &settings);

// The nf2ff.exe input: far field at the given frequencies on a theta/phi
// grid (degrees), from the six near-field faces found in the folder.
QString nf2ffXml(const std::vector<QString> &eFiles, const std::vector<QString> &hFiles, const std::vector<double> &frequencies,
                 const std::vector<double> &thetaDeg, const std::vector<double> &phiDeg, const Vec3 &centreMm,
                 const QString &outfile);

} // namespace lvd::fw

#pragma once

#include "antenna/ElementModel.h"

#include <QString>

#include <complex>
#include <optional>
#include <vector>

namespace lvd::fw {

using cplx = std::complex<double>;

// A time signal from an openEMS probe file (port_ut_1, port_it_1): lines
// "t value" after "%" comment lines.
struct ProbeSignal {
    std::vector<double> time;
    std::vector<double> value;
};
std::optional<ProbeSignal> readProbeFile(const QString &path, QString *error = nullptr);

// Discrete Fourier transform of a pulse, as openEMS's DFT_time2freq:
// 2 sum(v exp(-j 2 pi f t)) dt (a single-sided spectrum).
std::vector<cplx> dftPulse(const ProbeSignal &signal, const std::vector<double> &frequencies);

// Port quantities of a lumped port with reference impedance zRef, from its
// voltage and current probes (openEMS Port.CalcPort).
struct PortResult {
    std::vector<double> frequency;
    std::vector<cplx> voltage, current;      // totals
    std::vector<cplx> s11;                   // reflected / incident voltage
    std::vector<cplx> zin;                   // voltage / current
    std::vector<double> acceptedPower;       // W, 0.5 Re(u i*)
    std::vector<double> incidentPower;       // W
    double zRef = 50;
};
PortResult lumpedPortResult(const ProbeSignal &voltage, const ProbeSignal &current, const std::vector<double> &frequencies,
                            double zRef);

// The far field written by nf2ff in its legacy HDF5 layout (LegacyHDF5="1"):
// fields per frequency over (phi, theta), angles in radians.
struct FarField {
    std::vector<double> frequency;
    std::vector<double> theta, phi;          // radians
    std::vector<std::vector<cplx>> eTheta;   // per frequency, [iTheta * phi.size() + iPhi]
    std::vector<std::vector<cplx>> ePhi;
    std::vector<double> dmax;                // linear, from nf2ff (over the sampled grid)
    std::vector<double> radiatedPower;       // W, from nf2ff
    double radius = 1;
};
std::optional<FarField> readFarField(const QString &path, QString *error = nullptr);

// As an imported element pattern (degrees, phi folded into [0, 360)).
ant::PatternTable toPatternTable(const FarField &farField, const QString &source = {});

} // namespace lvd::fw

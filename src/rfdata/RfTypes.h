#pragma once

#include <Eigen/Dense>

#include <QString>

#include <complex>
#include <optional>
#include <vector>

namespace lvd::rf {

using Complex = std::complex<double>;
using CMatrix = Eigen::MatrixXcd;
using CVector = Eigen::VectorXcd;

constexpr double Pi = 3.14159265358979323846;
constexpr double SpeedOfLight = 299792458.0; // m/s
constexpr double Boltzmann = 1.380649e-23;   // J/K
constexpr double T0 = 290.0;                 // K, reference noise temperature
constexpr double DefaultZ0 = 50.0;           // ohm

// Two-port noise parameters at one frequency.
struct NoiseParameters {
    double fminDb = 0;        // minimum noise figure, dB
    Complex gammaOpt;         // optimum source reflection coefficient, referred to the network's z0 of port 1
    double rn = 0;            // equivalent noise resistance, ohm (not normalised)

    bool operator==(const NoiseParameters &) const = default;
};

// Multi-frequency N-port data, such as the content of a Touchstone file or a
// simulation result. Reference impedances are real and per port.
struct NetworkData {
    std::vector<double> frequencies; // Hz, strictly increasing
    std::vector<CMatrix> s;          // one N x N matrix per frequency
    std::vector<double> z0;          // reference impedance per port, ohm

    // Optional noise data. Either two-port noise parameters (as in Touchstone
    // files) on their own frequency grid, or noise wave correlation matrices
    // in kelvin on the main grid (simulation results); both may be empty.
    std::vector<double> noiseFrequencies;
    std::vector<NoiseParameters> noise;
    std::vector<CMatrix> noiseCorrelation; // N x N, kelvin: <c c^H> / kB

    QString comment;

    int portCount() const { return static_cast<int>(z0.size()); }
    int frequencyCount() const { return static_cast<int>(frequencies.size()); }
    bool hasNoiseParameters() const { return !noise.empty(); }
    bool hasNoiseCorrelation() const { return !noiseCorrelation.empty(); }

    // Problems such as mismatched sizes or unsorted frequencies; empty when valid.
    QString validate() const;
};

} // namespace lvd::rf

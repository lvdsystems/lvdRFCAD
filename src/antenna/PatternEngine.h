#pragma once

#include "antenna/ArrayGeometry.h"
#include "antenna/ElementModel.h"

#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace lvd::ant {

// What a pattern plot shows (requirements AN-6).
enum class PatternComponent { Total, Co, Cross, Rhcp, Lhcp, Theta, Phi };
// Reference (co-)polarisation for Co and Cross: Ludwig-3 x or y.
enum class CoPolarisation { X, Y };

QString patternComponentId(PatternComponent component);
QString patternComponentName(PatternComponent component);
std::vector<PatternComponent> patternComponents();

// Power of one component of a field, linear. phi in radians (Ludwig 3).
double componentPower(const Field &field, double phi, PatternComponent component, CoPolarisation co);

// Runs fn(i) for i in [0, n) on worker threads; stops early when *cancel is set.
void parallelFor(std::size_t n, const std::function<void(std::size_t)> &fn, const std::atomic_bool *cancel = nullptr);

// Far field of a weighted array of identical elements, ignoring mutual
// coupling beyond what an (embedded) element pattern carries.
//
// field() is normalised by the transmitted power sum, so |E|^2 is the gain
// (dBi after 10 log10) by the embedded-element method, including the
// element efficiency: G = eta |sum w_n E_n|^2 / sum |w_n|^2. rawField()
// leaves the weights unnormalised: with w_n = sqrt(P_n / 1 W), |E|^2 is the
// EIRP in watts.
class PatternEngine
{
public:
    PatternEngine(const ArrayLayout &layout, std::vector<cplx> weights, std::shared_ptr<const ElementPattern> element,
                  double frequency);

    double frequency() const { return m_frequency; }
    std::size_t elementCount() const { return m_positions.size(); }
    const std::vector<cplx> &weights() const { return m_weights; }
    const ElementPattern &element() const { return *m_element; }
    double weightPower() const { return m_weightPower; } // sum |w|^2

    // theta, phi in radians.
    Field field(double theta, double phi) const;
    Field rawField(double theta, double phi) const;
    Field fieldAt(const Vec3 &direction) const;
    double gain(double theta, double phi) const { return field(theta, phi).power(); }

    // Frame of the UV plane: boresight, u axis and v axis. Planar and linear
    // arrays: +z, +x, +y; a cylinder: +x, +y, +z.
    const Mat3 &uvFrame() const { return m_uvFrame; }
    Vec3 uvDirection(double u, double v) const; // forward hemisphere; requires u^2 + v^2 <= 1

    // Rough half-power beamwidth from the aperture size, radians.
    double estimatedBeamwidth() const;

    // (1 / 4 pi) times the integral of |field|^2 / efficiency over the sphere:
    // 1 when the power-sum normalisation matches the radiated power. The
    // integrated directivity is gain / (efficiency * this). NaN on cancel.
    double integratedPower(const std::atomic_bool *cancel = nullptr) const;

private:
    Field sum(const Vec3 &r, double theta, double phi, bool normalised) const;

    std::vector<Vec3> m_positions;
    std::vector<Mat3> m_orientations;
    bool m_identical = true;
    std::vector<cplx> m_weights;
    std::shared_ptr<const ElementPattern> m_element;
    double m_frequency = 0;
    double m_k = 0;
    double m_scale = 1;       // element normalisation
    double m_weightPower = 0;
    double m_norm = 1;        // sqrt(eta / sum |w|^2)
    double m_extent = 0;      // largest element distance from the centre, m
    Mat3 m_uvFrame = Mat3::Identity();
};

// ---- Sampled patterns

// A great-circle or constant-phi cut. Angles in degrees.
struct PatternCut {
    QString name;
    std::vector<double> angle;  // signed theta (constant-phi cut) or offset from the beam (beam cut)
    std::vector<double> theta;  // direction of each point
    std::vector<double> phi;
    std::vector<Field> fields;  // normalised (gain) fields
};

// theta from -180 to 180 in the plane phi = phiDeg (negative theta: phi + 180).
PatternCut computePhiCut(const PatternEngine &engine, double phiDeg, double stepDeg, const std::atomic_bool *cancel = nullptr);
// A conical cut at constant theta (theta = 90: the horizon, the azimuth
// pattern of direction-finding arrays); angle is phi from -180 to 180.
PatternCut computeThetaCut(const PatternEngine &engine, double thetaDeg, double stepDeg, const std::atomic_bool *cancel = nullptr);
// A great circle through `direction`, towards `towards` (unit vectors), from -spanDeg to spanDeg.
PatternCut computeGreatCircleCut(const PatternEngine &engine, const Vec3 &direction, const Vec3 &towards, double spanDeg,
                                 double stepDeg, const std::atomic_bool *cancel = nullptr);

// theta 0..180 and phi -180..180 (both ends included), degrees.
struct SphericalGrid {
    std::vector<double> theta;
    std::vector<double> phi;
    std::vector<Field> fields; // [iTheta * phi.size() + iPhi]
};
SphericalGrid computeSphericalGrid(const PatternEngine &engine, double stepDeg, const std::atomic_bool *cancel = nullptr);

// u, v in [-1, 1]; points outside the unit circle have valid = false.
struct UvGrid {
    int size = 0;
    std::vector<double> axis;    // u and v values
    std::vector<Field> fields;   // [iv * size + iu]
    std::vector<double> phi;     // phi (radians) of each direction, for Ludwig 3
    std::vector<bool> valid;
};
UvGrid computeUvGrid(const PatternEngine &engine, int size, const std::atomic_bool *cancel = nullptr);

// ---- Beam metrics

struct BeamMetrics {
    double peakGainDbi = NAN;
    double peakThetaDeg = NAN, peakPhiDeg = NAN;
    double gainAtSteerDbi = NAN;
    double pointingErrorDeg = NAN;
    // Two orthogonal great-circle cuts through the peak: plane 1 along
    // theta_hat (the elevation-like plane), plane 2 along phi_hat.
    double beamwidth1Deg = NAN, beamwidth2Deg = NAN;      // half power
    double nullWidth1Deg = NAN, nullWidth2Deg = NAN;      // first nulls
    double sideLobe1Db = NAN, sideLobe2Db = NAN;          // highest side lobe in the cut, dB below the peak (negative)
    double sideLobeDb = NAN;                              // over the UV grid, when given
    double crossPolDb = NAN;                              // cross / co at the peak
    double axialRatioDb = NAN;                            // at the peak
    double directivityDbi = NAN;                          // integrated (when requested)
    PatternCut cut1, cut2;
};

struct MetricsOptions {
    double steerThetaDeg = 0, steerPhiDeg = 0;
    CoPolarisation co = CoPolarisation::X;
    const UvGrid *uv = nullptr; // for the two-dimensional side-lobe level
    bool integrate = false;     // integrated directivity (slower)
};

BeamMetrics computeMetrics(const PatternEngine &engine, const MetricsOptions &options, const std::atomic_bool *cancel = nullptr);

// Local maximum of the gain near `start` (a quick pattern search); returns
// the direction and sets *gain (linear).
Vec3 findPeakNear(const PatternEngine &engine, const Vec3 &start, double *gain = nullptr);

// Shape of a cut (power in dB relative to its peak) -> half-power width,
// first-null width and highest side lobe, around index `centre`. Exposed for tests.
struct CutShape {
    double beamwidth = NAN, nullWidth = NAN, sideLobeDb = NAN;
};
CutShape analyseCut(const std::vector<double> &angle, const std::vector<double> &relativeDb, std::size_t centre);

// ---- Grating lobes (periodic lattices)

struct GratingLobeInfo {
    bool line = false;                       // linear array: lobes are lines u = const
    std::vector<Eigen::Vector2d> centres;    // lattice points in UV (main beam at broadside excluded)
};
GratingLobeInfo gratingLobes(const Lattice &lattice, double frequency, int order = 3);
// Largest scan angle (degrees) in the plane phiDeg without a grating lobe
// peak in visible space; 90 when none appears.
double gratingLobeFreeScan(const Lattice &lattice, double frequency, double phiDeg);
// Grating lobes in visible space for the main beam steered to (u0, v0).
std::vector<Eigen::Vector2d> visibleGratingLobes(const GratingLobeInfo &info, double u0, double v0);

} // namespace lvd::ant

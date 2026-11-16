#pragma once

#include "antenna/ArrayGeometry.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace lvd::ant {

// Amplitude tapers (requirements AN-2). Continuous tapers are sampled at the
// element positions over the aperture extended by half a spacing at each
// edge; Chebyshev (Dolph) is exact for equally spaced positions.
enum class TaperKind { Uniform, Taylor, Chebyshev, Cosine, CosinePedestal, Hamming, Custom };

QString taperId(TaperKind kind);
QString taperName(TaperKind kind);
std::vector<TaperKind> taperKinds();

struct TaperSpec {
    TaperKind kind = TaperKind::Uniform;
    double sllDb = 30;        // Taylor and Chebyshev: side-lobe level below the peak
    int nbar = 5;             // Taylor
    double exponent = 1;      // cosine: cos^n
    double pedestalDb = -10;  // cosine on a pedestal: edge level

    QJsonObject toJson() const;
    static TaperSpec fromJson(const QJsonObject &object);
};

// Taper functions, exposed for tests. p is the normalised aperture
// coordinate in [-1, 1] (line) or [0, 1] (radius).
double lineTaper(const TaperSpec &spec, double p);
double circularTaylor(double sllDb, int nbar, double p);
std::vector<double> dolphChebyshev(int count, double sllDb);
double besselJ0(double x);

// None: no steering phases; the elements are fed with the taper (and the
// custom phases) only, for amplitude steering, switched beams and DF arrays.
enum class SteeringMode { Phase, TimeDelay, SubarrayTimeDelay, None };
QString steeringModeId(SteeringMode mode);
QString steeringModeName(SteeringMode mode);

enum class BeamPattern { Sum, DeltaAzimuth, DeltaElevation };
QString beamPatternId(BeamPattern pattern);
QString beamPatternName(BeamPattern pattern);

struct BeamSpec {
    double thetaDeg = 0, phiDeg = 0;
    double levelDb = 0;
    double phaseDeg = 0;
};

struct DirectionSpec {
    double thetaDeg = 0, phiDeg = 0;
};

struct ExcitationSpec {
    TaperSpec x;                 // along x (linear and planar), around the arc (cylinder)
    TaperSpec y;                 // along y (planar), along the axis (cylinder)
    bool radial = false;         // planar and circular: one taper over the radius (uses x)

    double steerThetaDeg = 0, steerPhiDeg = 0;
    SteeringMode steering = SteeringMode::Phase;
    int subarrayX = 0, subarrayY = 0; // elements per sub-array; 0: no sub-arrays
    bool subarrayTaper = false;       // one amplitude per sub-array (at its centre)

    std::vector<BeamSpec> extraBeams; // simultaneous beams added to the main one (AN-4)
    std::vector<DirectionSpec> nulls; // directions forced to zero by projection
    BeamPattern pattern = BeamPattern::Sum;

    // Per-element amplitude (dB) and phase (degrees) of the "custom" taper,
    // for example from pattern synthesis; geometry custom values otherwise.
    std::vector<std::pair<double, double>> weights;

    QJsonObject toJson() const;
    static ExcitationSpec fromJson(const QJsonObject &object);
};

// AESA hardware effects (requirements AN-3).
struct AesaSpec {
    int phaseBits = 0;            // 0: continuous phase
    int attenuatorBits = 0;       // 0: continuous amplitude
    double attenuatorStepDb = 0.5;
    double delayLsbPs = 0;        // true-time-delay resolution, 0: continuous
    double amplitudeErrorDb = 0;  // RMS, Gaussian
    double phaseErrorDeg = 0;     // RMS, Gaussian
    double failedFraction = 0;    // random failures
    std::vector<int> failed;      // element indices that have failed
    unsigned seed = 1;

    bool isIdeal() const;
    QJsonObject toJson() const;
    static AesaSpec fromJson(const QJsonObject &object);
};

struct ExcitationResult {
    std::vector<cplx> weights;   // per element, at the operating frequency
    std::vector<int> subarray;   // sub-array index per element, -1 when none
    std::vector<bool> failed;
    std::vector<Vec3> subarrayCentres;
    double rmsPhaseQuantisationDeg = 0; // from the phase bits alone
    QStringList warnings;
};

// The element weights at `frequency` for a layout; designFrequency sets the
// phase-shifter steering and the "lambda" unit. With applyAesa false the
// quantisation, errors and failures are left out (the ideal reference).
ExcitationResult computeExcitation(const ArrayLayout &layout, const ExcitationSpec &spec, const AesaSpec &aesa,
                                   double frequency, double designFrequency, bool applyAesa = true);

} // namespace lvd::ant

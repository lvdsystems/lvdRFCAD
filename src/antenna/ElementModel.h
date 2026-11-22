#pragma once

#include "antenna/AntennaTypes.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace lvd::ant {

// A far-field pattern sampled on a theta/phi grid, possibly at several
// frequencies (imported or computed patterns; requirements AN-5).
struct PatternTable {
    std::vector<double> frequencies; // Hz, ascending; a single 0 means "frequency unknown"
    std::vector<double> theta;       // degrees, ascending, within 0..180
    std::vector<double> phi;         // degrees, ascending
    // One vector per frequency, indexed [iTheta * phi.size() + iPhi].
    std::vector<std::vector<Field>> fields;
    bool hasPhase = true;  // false: magnitudes only (phase set to zero)
    QString source;        // file the table was read from

    bool isValid() const;
    // Bilinear in theta and phi (periodic in phi when the grid spans 360
    // degrees), linear between frequencies, clamped outside the frequency
    // range; zero outside the theta range. Angles in degrees.
    Field sample(double thetaDeg, double phiDeg, double frequency) const;
};

// FullWave: an openEMS model (template and settings in `fullWave`); its
// pattern is the simulated far field in patternFile once it has run.
enum class ElementModelKind { Isotropic, CosN, Dipole, DipoleOverGround, Monopole, Patch, Aperture, Imported, FullWave };
enum class Polarisation { LinearX, LinearY, Slant45, SlantMinus45, Rhcp, Lhcp };

QString elementModelId(ElementModelKind kind);
QString elementModelName(ElementModelKind kind); // for the user interface
std::vector<ElementModelKind> elementModelKinds();
QString polarisationId(Polarisation polarisation);
QString polarisationName(Polarisation polarisation);
std::vector<Polarisation> polarisations();

// Description of a radiating element (the "element" object of an antenna
// element document or library entry; docs/file-format.md, section 7).
// Lengths in metres; zero lengths mean "derive from the design frequency".
// The element's own frame has its boresight along +z and its reference
// (co-)polarisation along +x.
struct ElementSpec {
    ElementModelKind model = ElementModelKind::CosN;
    Polarisation polarisation = Polarisation::LinearX;
    double frequency = 10e9;     // design frequency, Hz
    double efficiencyDb = 0;     // radiation efficiency (<= 0 dB)

    double exponent = 1.5;       // cos^n power pattern
    double backLobeDb = -30;     // cos^n, patch and aperture: level behind the ground plane, dBi relative to peak
    double length = 0;           // dipole (full length) and monopole height; 0: half and quarter wavelength
    double height = 0;           // dipole over ground: height above the ground plane; 0: quarter wavelength
    double patchWidth = 0;       // patch (cavity model): 0 derives width and length from the design frequency
    double patchLength = 0;
    double substrateHeight = 1.524e-3;
    double permittivity = 3.38;
    double apertureA = 0;        // aperture: size along y (broad wall; cosine distribution), 0: 0.72 wavelength
    double apertureB = 0;        // aperture: size along x (E-plane), 0: 0.34 wavelength
    bool cosineDistribution = true;

    QString patternFile;         // imported and full-wave: file name relative to the owning document or library
    QJsonObject fullWave;        // full-wave: template, parameters and run settings (src/fullwave)
    std::shared_ptr<const PatternTable> pattern; // imported and full-wave: loaded table (not stored in JSON)

    bool usesPatternFile() const { return model == ElementModelKind::Imported || model == ElementModelKind::FullWave; }

    QJsonObject toJson() const;
    static ElementSpec fromJson(const QJsonObject &object);

    // The patch dimensions for the design frequency (Balanis' design
    // equations): width, physical length and effective length.
    struct PatchSize {
        double width = 0;
        double length = 0;
        double effectiveLength = 0;
        double effectivePermittivity = 1;
    };
    PatchSize patchSize() const;
    // A short summary such as "Patch 12.6 x 9.8 mm, er 3.38, x-pol".
    QString summary() const;
};

// Evaluates an element's far field, normalised so that |E|^2 is the
// directivity (the peak gain is directivity times efficiency). Thread-safe
// after prepare() has been called for the frequencies used; field() at an
// unprepared frequency prepares it under a lock.
class ElementPattern
{
public:
    explicit ElementPattern(ElementSpec spec);

    const ElementSpec &spec() const { return m_spec; }

    // Normalised field; direction in the element's own frame, radians.
    // Convenient but locks once per call: loops use normalisation() once and
    // shape() per direction.
    Field field(double theta, double phi, double frequency) const { return shape(theta, phi, frequency) * normalisation(frequency); }
    // Amplitude factor that turns shape() into the normalised field.
    double normalisation(double frequency) const;
    // Unnormalised field with the polarisation applied; thread-safe, no locks.
    Field shape(double theta, double phi, double frequency) const;

    double efficiency() const { return std::pow(10.0, m_spec.efficiencyDb / 10.0); }
    // Peak directivity and its direction (degrees), from a half-degree search.
    double peakDirectivityDbi(double frequency, double *thetaDeg = nullptr, double *phiDeg = nullptr) const;

    // Unnormalised field of the x-polarised model; exposed for tests.
    Field rawX(double theta, double phi, double frequency) const;

private:

    ElementSpec m_spec;
    mutable std::mutex m_mutex;
    mutable std::map<double, double> m_scale; // frequency -> amplitude normalisation
};

} // namespace lvd::ant

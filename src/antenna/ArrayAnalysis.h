#pragma once

#include "antenna/ArrayDesign.h"
#include "antenna/PatternEngine.h"

#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <vector>

namespace lvd {
class Project;
}

namespace lvd::ant {

class AntennaLibraryManager;

// ---- Resolving references (call on the thread that owns the project)

struct ElementResolution {
    std::shared_ptr<const ElementPattern> pattern; // null on error
    QString label;
    QString error;
    QStringList warnings;
};

// Loads an imported pattern file (relative to baseDirectory) and wraps the spec.
ElementResolution elementFromSpec(ElementSpec spec, const QString &baseDirectory, const QString &label = {});
// The element of an array: an element document of the project, a library
// entry or the inline element.
ElementResolution resolveElement(const ArrayDesign &design, const Project *project, const AntennaLibraryManager *library);

struct TrValues {
    double powerDbm = 0; // transmit output per element
    double gainDb = 0;
    double nfDb = 0;
    QString origin;      // "typed", "preset X", "rx_chain / Budget"
    QStringList warnings;
    QString error;
};
// Runs the RF budget analysis for a linked side.
TrValues resolveTrSide(const TrSideSpec &side, bool transmit, const Project *project, const AntennaLibraryManager *library);

// Copies a library T/R module preset into typed values and AESA settings.
void applyTrPreset(const QJsonObject &presetData, TrModuleSpec *tr, AesaSpec *aesa);

// ---- Evaluation (thread-safe once the element is resolved)

struct ArrayEvaluation {
    double frequency = 0;
    ArrayLayout layout;
    ExcitationResult excitation; // with the AESA effects
    ExcitationResult ideal;      // without them
    std::shared_ptr<const ElementPattern> element;
    std::shared_ptr<const PatternEngine> engine;
    std::shared_ptr<const PatternEngine> idealEngine;
    QStringList warnings;
};

// frequency 0: the design's operating frequency.
ArrayEvaluation evaluateArray(const ArrayDesign &design, std::shared_ptr<const ElementPattern> element, double frequency = 0);

struct BudgetValues {
    double eirpDbw = NAN;
    double gainDbi = NAN;           // receive gain at the steering direction (before the front loss)
    double gOverTDbK = NAN;
    double systemTemperatureK = NAN; // at the LNA input
    double totalPowerW = 0;         // sum of the PA outputs
    int activeElements = 0;
};
BudgetValues computeArrayBudget(const ArrayEvaluation &evaluation, const ArrayDesign &design, const TrValues &tx,
                                const TrValues &rx);

struct ScanPoint {
    double scanDeg = 0;          // signed angle in the scan plane
    double gainAtSteerDbi = NAN;
    double peakGainDbi = NAN;
    double pointingDeg = NAN;    // signed angle of the actual peak in the plane
    double eirpDbw = NAN;
    double gOverTDbK = NAN;
    bool gratingLobe = false;
};
std::vector<ScanPoint> scanSweep(const ArrayDesign &design, std::shared_ptr<const ElementPattern> element, const TrValues &tx,
                                 const TrValues &rx, const std::atomic_bool *cancel = nullptr);

struct BandPoint {
    double frequency = 0;
    double peakGainDbi = NAN;
    double gainAtSteerDbi = NAN;
    double peakThetaDeg = NAN, peakPhiDeg = NAN;
    double squintDeg = NAN;      // angle between the peak and the steering direction
};
std::vector<BandPoint> bandSweep(const ArrayDesign &design, std::shared_ptr<const ElementPattern> element,
                                 const std::atomic_bool *cancel = nullptr);

} // namespace lvd::ant

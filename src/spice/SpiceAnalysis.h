#pragma once

#include "core/Expression.h"
#include "spice/SpiceData.h"
#include "spice/SpiceEngine.h"
#include "spice/SpiceNetlister.h"
#include "spice/SpiceWaveforms.h"

#include <QJsonObject>
#include <QList>
#include <QString>

namespace lvd {
class Document;
class Project;
} // namespace lvd

namespace lvd::spice {

// SPICE analyses (requirement SP-2).
enum class AnalysisKind {
    OperatingPoint,
    DcSweep,
    Ac,
    Transient,
    Noise,
    Fourier, // a transient whose signals are analysed into harmonics (like .four)
    TransferFunction,
    PoleZero,
    Sensitivity,
};

QString analysisTypeName(AnalysisKind kind);   // "spice-op", "spice-dc", ...
QString analysisDisplayName(AnalysisKind kind); // "Operating point", ...
std::optional<AnalysisKind> analysisKindFromType(const QString &type);
bool isSpiceAnalysisType(const QString &type);
QList<AnalysisKind> allAnalysisKinds();

// One analysis as stored in a schematic's "simulation" object
// (docs/file-format.md, section 5.1). Fields hold expression text evaluated
// with the top sheet's variables, except names (sources, nets), choices and
// flags. Unknown keys are kept.
//
//   dc:       source (component reference, or "temp"), start, stop, step;
//             optional source2, start2, stop2, step2 (nested sweep)
//   ac:       sweep (dec, oct, lin), points, start, stop
//   tran:     step, stop; optional start, maxStep; uic (bool)
//   noise:    output (net, or "v(a,b)"), source (component reference),
//             sweep, points, start, stop
//   fourier:  tran fields, plus fundamental, harmonics, signals (list of
//             trace expressions)
//   tf:       output (net, "v(a,b)" or "i(V1)"), source
//   pz:       inputPositive, inputNegative, outputPositive, outputNegative
//             (nets), transfer (voltage, current), find (both, poles, zeros)
//   sens:     output (net, "v(a,b)" or "i(V1)"); ac (bool) with the ac fields
//   all:      temperature (deg C, optional), options (text for .options),
//             cards (extra SPICE text placed before the analysis)
struct SpiceAnalysisSetup {
    QString id;
    QString name;
    AnalysisKind kind = AnalysisKind::Transient;
    QJsonObject data; // the whole stored object

    QString text(const QString &key, const QString &fallback = {}) const;
    bool flag(const QString &key, bool fallback = false) const;
    QStringList list(const QString &key) const;
    void set(const QString &key, const QJsonValue &value) { data.insert(key, value); }

    static SpiceAnalysisSetup create(AnalysisKind kind, const QString &name = {}); // with default fields
    static std::optional<SpiceAnalysisSetup> fromJson(const QJsonObject &object);
    QJsonObject toJson() const;
};

QList<SpiceAnalysisSetup> spiceAnalyses(const QJsonObject &simulation);
// Adds or replaces (by id) an analysis; assigns an id ("a<n>") if it has none.
QJsonObject withSpiceAnalysis(const QJsonObject &simulation, SpiceAnalysisSetup analysis, QString *assignedId = nullptr);

// The control cards of an analysis, for a deck built from the same schematic.
struct AnalysisCards {
    QStringList cards;
    QStringList errors;
};
AnalysisCards analysisCards(const SpiceAnalysisSetup &analysis, const SpiceDeck &deck, const Expression::Lookup &scope);

// The deck's text with the cards inserted before ".end".
QString deckWithCards(const SpiceDeck &deck, const QStringList &cards);

// The top sheet's variables as a lookup.
Expression::Lookup sheetVariables(const Document &document, QStringList *errors = nullptr);

struct SpiceSimulation {
    SpiceAnalysisSetup analysis;
    SpiceDeck deck;
    QString netlist; // what was given to the engine
    SpiceRunResult run;
    QStringList errors; // netlisting and setup problems, then the engine's
    QStringList warnings;
    // Fourier analyses: the evaluated setup and, after the run, one result per
    // signal over the last period(s) of the transient.
    double fourierFundamental = 0;
    int fourierHarmonics = 9;
    int fourierPeriods = 1;
    QList<FourierSignal> fourier;

    bool ok() const { return errors.isEmpty() && run.ok; }
};

// Netlists the document and adds the analysis cards; reads the documents,
// so the GUI calls it on its own thread. errors is non-empty when the
// simulation cannot run.
SpiceSimulation prepareSpiceAnalysis(const Project &project, const Document &document, const SpiceAnalysisSetup &analysis);
// Runs a prepared simulation on the engine (blocking; any thread).
void executeSpiceAnalysis(SpiceSimulation &simulation, SpiceEngine &engine, const SpiceRunOptions &options = {});
// After simulation.run is set (by an engine or a worker process): takes the
// run's errors and warnings and computes the Fourier results.
void finishSpiceAnalysis(SpiceSimulation &simulation);

// prepare + execute. Blocking; call from a worker thread in the GUI.
SpiceSimulation runSpiceAnalysis(const Project &project, const Document &document, const SpiceAnalysisSetup &analysis,
                                 SpiceEngine &engine, const SpiceRunOptions &options = {});

} // namespace lvd::spice

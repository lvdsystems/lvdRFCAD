#pragma once

#include "fullwave/FwModel.h"
#include "fullwave/Mesher.h"
#include "fullwave/SimulationResults.h"

#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <optional>
#include <vector>

namespace lvd::fw {

// The folder holding openEMS.exe and nf2ff.exe: LVD_OPENEMS_DIR, else
// "openems" next to the application or one level up (development builds).
QString openEmsDirectory();
bool openEmsAvailable();

// Results of a full-wave run, as stored in its folder (results.json,
// farfield.h5, pattern.csv).
struct FwResults {
    bool ok = false;
    QString error;
    QStringList warnings;
    QString folder;
    PortResult port;                         // port 1
    std::vector<double> farFieldFrequencies; // Hz
    std::vector<double> radiatedPower;       // W, per far-field frequency
    std::vector<double> radiationEfficiency; // radiated / accepted power, per far-field frequency
    std::vector<double> directivityDbi;      // peak, from the full far-field grid
    std::size_t cells = 0;
    int timesteps = 0;
    double seconds = 0;
    bool cancelled = false;

    QString patternFile() const;             // the far field as a pattern CSV
    double resonance() const;                // frequency of the smallest |S11|
};

using ProgressCallback = std::function<void(double fraction, const QString &message)>;

// Runs openEMS and nf2ff for a model in `folder` (created or reused;
// earlier results there are replaced). Blocking; call it on a worker
// thread. `cancel` stops the solver.
FwResults runFullWave(const FwModel &model, const FwSettings &settings, const QString &folder,
                      const std::atomic_bool *cancel = nullptr, const ProgressCallback &progress = {});

// Reads the results stored by an earlier run (without the far field data,
// which the pattern file holds).
std::optional<FwResults> loadResults(const QString &folder, QString *error = nullptr);

} // namespace lvd::fw

#pragma once

#include "spice/SpiceData.h"

#include <QString>

#include <functional>
#include <memory>

namespace lvd::spice {

// Progress of a running simulation, for a progress bar: the analysis name
// ("tran", "ac", ...) and the fraction done (0..1, negative when unknown).
struct SpiceProgress {
    QString analysis;
    double fraction = -1;
};

struct SpiceRunOptions {
    std::function<void(const SpiceProgress &)> progress; // called from the engine's thread
    std::function<void(const QString &line)> output;      // every line the engine prints
    // Vectors to keep; empty keeps everything the engine produces.
    QStringList save;
};

// A SPICE simulator behind which the editor, the waveform viewer and the
// instruments work (requirement SP-1). The netlist is plain SPICE text with
// its analysis cards; the result is engine-independent data.
//
// run() blocks until the simulation ends and is meant to be called from a
// worker thread. abort(), pause() and resume() may be called from any thread
// while run() is busy.
class SpiceEngine
{
public:
    virtual ~SpiceEngine() = default;

    virtual QString name() const = 0;    // "ngspice"
    virtual QString version() const = 0; // "47"
    // Loads the engine if needed. False with *error when it cannot be used.
    virtual bool isAvailable(QString *error = nullptr) = 0;

    virtual SpiceRunResult run(const QString &netlist, const SpiceRunOptions &options = {}) = 0;

    virtual void abort() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual bool isRunning() const = 0;
    virtual bool isPaused() const = 0;
};

// The engine used by the application; ngspice unless replaced (tests, a
// custom engine). The returned engine lives until the process ends.
SpiceEngine &defaultEngine();
void setDefaultEngine(std::shared_ptr<SpiceEngine> engine);

} // namespace lvd::spice

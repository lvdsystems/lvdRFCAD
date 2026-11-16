#pragma once

#include "spice/SpiceAnalysis.h"

#include <QList>
#include <QString>

#include <atomic>
#include <functional>

namespace lvd::spice {

// Runs prepared simulations in parallel worker processes (requirement SP-3,
// risk 6 of the plan: ngspice keeps global state, so one process runs one
// simulation at a time). Each worker is "lvdrfcad-cli --spice-run <netlist>
// <raw file>"; its raw file is read back and the simulation finished as if
// it had run in this process.
class SpiceProcessPool
{
public:
    // workers <= 0: the number of processor cores.
    explicit SpiceProcessPool(int workers = 0);

    // The worker program; default: lvdrfcad-cli next to the application.
    void setProgram(const QString &program) { m_program = program; }
    QString program() const;
    int workers() const { return m_workers; }

    // Blocking; suitable for BatchOptions::execute.
    void execute(QList<SpiceSimulation *> &simulations, const std::atomic_bool *cancel,
                 const std::function<void(int done)> &progress) const;

    // The worker side: runs a netlist file on the default engine and writes
    // the raw file. Errors go to stderr, one per line; returns the exit code.
    static int runWorker(const QString &netlistPath, const QString &rawPath);

private:
    int m_workers;
    QString m_program;
};

} // namespace lvd::spice

#pragma once

#include "core/Netlist.h"
#include "rfsim/ElementModels.h"
#include "rfsim/LinearSolver.h"

#include <QObject>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace lvd::sim {

// Runs simulateLinear on a worker thread, one run at a time (NF-3).
//
// The caller builds the netlist and the sweep on the GUI thread, because
// they read the documents; the runner copies them into the worker, which
// only touches its own copies and the runner's TouchstoneCache. The cache is
// never shared by two runs: start() refuses while a run is active. The result
// comes back through a queued call into the runner's thread and is emitted as
// finished(). cancel() sets the flag the solver polls between frequencies;
// the destructor cancels and joins.
class SimulationRunner : public QObject
{
    Q_OBJECT
public:
    explicit SimulationRunner(QObject *parent = nullptr);
    ~SimulationRunner() override;

    bool isRunning() const { return m_running; }
    // Returns false (and does nothing) while a run is active.
    bool start(Netlist netlist, std::vector<double> frequencies, rf::LinearOptions options, quint64 tag = 0);
    void cancel();
    // Blocks until the worker has finished; for tests and shutdown.
    void wait();

Q_SIGNALS:
    // `tag` is the value given to start(); the result is never null.
    void finished(std::shared_ptr<const lvd::rf::LinearResult> result, quint64 tag);

private:
    void deliver(std::shared_ptr<const rf::LinearResult> result, quint64 tag);

    std::thread m_thread;
    std::atomic_bool m_cancel{false};
    bool m_running = false;
    rf::TouchstoneCache m_touchstones;
};

} // namespace lvd::sim

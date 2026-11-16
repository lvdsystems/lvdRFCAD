#pragma once

#include "core/Expression.h"
#include "core/Netlist.h"
#include "rfsim/Budget.h"
#include "rfsim/ElementModels.h"

#include <QObject>

#include <atomic>
#include <memory>
#include <thread>

namespace lvd::sim {

// Runs rf::runBudget on a worker thread, one run at a time (RF-6 to RF-9,
// NF-3), like SimulationRunner does for linear analyses.
//
// The caller builds the netlist and the expression scope on the GUI thread;
// the worker only touches its own copies and the runner's TouchstoneCache.
// The result comes back through a queued call and is emitted as finished().
// cancel() sets the flag runBudget polls; the destructor cancels and joins.
class BudgetRunner : public QObject
{
    Q_OBJECT
public:
    explicit BudgetRunner(QObject *parent = nullptr);
    ~BudgetRunner() override;

    bool isRunning() const { return m_running; }
    // Returns false (and does nothing) while a run is active.
    bool start(Netlist netlist, rf::BudgetAnalysisSetup setup, Expression::Lookup scope, QString projectDirectory,
               quint64 tag = 0);
    void cancel();
    // Blocks until the worker has finished; for tests and shutdown.
    void wait();

Q_SIGNALS:
    // `tag` is the value given to start(); the run is never null.
    void finished(std::shared_ptr<const lvd::rf::BudgetRun> run, quint64 tag);

private:
    void deliver(std::shared_ptr<const rf::BudgetRun> run, quint64 tag);

    std::thread m_thread;
    std::atomic_bool m_cancel{false};
    bool m_running = false;
    rf::TouchstoneCache m_touchstones;
};

} // namespace lvd::sim

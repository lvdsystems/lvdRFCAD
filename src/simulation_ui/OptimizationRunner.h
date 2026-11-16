#pragma once

#include "rfsim/Optimizer.h"

#include <QObject>
#include <QPointF>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lvd {
class Project;
}

namespace lvd::sim {

// Runs rf::optimize on a project snapshot in a worker thread (requirements
// RF-12, NF-3), with cancel.
//
// The snapshot is taken on the GUI thread and handed over to start(); the
// worker is its only user until the run ends, and it is released on the
// runner's thread afterwards. Progress is collected under a mutex and
// announced with progressed() (queued, at most every 100 ms); the goals of
// the best point so far are re-evaluated at that rate too. The result comes
// back through a queued call as finished().
class OptimizationRunner : public QObject
{
    Q_OBJECT
public:
    struct Progress {
        int evaluation = 0;
        int maxEvaluations = 0;
        double cost = rf::NotSet;
        double bestCost = rf::NotSet;
        std::vector<double> best;
        std::vector<QPointF> history;      // (evaluation, best cost) at every improvement
        QList<rf::GoalEvaluation> goals;   // of the best point so far (may lag a little)
    };

    explicit OptimizationRunner(QObject *parent = nullptr);
    ~OptimizationRunner() override;

    // Returns false while a run is active.
    bool start(std::shared_ptr<Project> snapshot, const QString &documentName, const rf::OptimizationSetup &setup);
    void cancel();
    bool isRunning() const { return m_running; }
    // Blocks until the worker has finished and delivers the result; for
    // tests and shutdown.
    void wait();
    Progress progress() const;

Q_SIGNALS:
    void progressed();
    void finished(std::shared_ptr<const lvd::rf::OptimizationResult> result);

private:
    void deliver(std::shared_ptr<const rf::OptimizationResult> result);

    std::thread m_thread;
    std::atomic_bool m_cancel{false};
    std::atomic_bool m_progressPosted{false};
    bool m_running = false;
    mutable std::mutex m_mutex;
    Progress m_progress;
};

} // namespace lvd::sim

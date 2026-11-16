#pragma once

#include "rfsim/Optimizer.h"

#include <QObject>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace lvd {
class Project;
}

namespace lvd::sim {

// Evaluates tuned designs on a project snapshot in a worker thread
// (requirements RF-11, NF-3).
//
// The snapshot (Project::snapshot(), taken on the GUI thread) is handed over
// with setSnapshot() and from then on used only by the worker thread, which
// lives as long as the runner. request() queues an evaluation; while one is
// running only the newest request is kept (coalescing), so a slider that
// moves faster than the simulator never builds a backlog and the last
// requested values are always evaluated last. Results come back through a
// queued call as evaluated(). A replaced snapshot is released on the
// runner's (GUI) thread.
class TuningRunner : public QObject
{
    Q_OBJECT
public:
    explicit TuningRunner(QObject *parent = nullptr);
    ~TuningRunner() override;

    // Used from the next evaluation on. `documentName` is the analysed
    // schematic in the snapshot.
    void setSnapshot(std::shared_ptr<Project> snapshot, const QString &documentName);
    // Queues an evaluation, replacing a request that has not started yet.
    // Returns its serial number (increasing).
    quint64 request(const rf::LinearAnalysisSetup &analysis, const QList<rf::DesignVariable> &variables,
                    const std::vector<double> &values);
    // Drops a request that has not started; a running evaluation finishes
    // and is still delivered.
    void dropPending();
    // True while the newest request has not been delivered.
    bool isBusy() const { return m_requested != m_delivered && !m_dropped; }
    quint64 lastRequested() const { return m_requested; }
    quint64 lastDelivered() const { return m_delivered; }

Q_SIGNALS:
    // `values` are those of the request; `milliseconds` is the time the
    // evaluation took on the worker thread.
    void evaluated(std::shared_ptr<const lvd::rf::LinearResult> result, quint64 serial, std::vector<double> values,
                   double milliseconds);

private:
    struct Job {
        quint64 serial = 0;
        rf::LinearAnalysisSetup analysis;
        QList<rf::DesignVariable> variables;
        std::vector<double> values;
    };

    void work();
    void deliver(std::shared_ptr<const rf::LinearResult> result, quint64 serial, std::vector<double> values,
                 double milliseconds);

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    // Shared with the worker, under m_mutex.
    std::optional<Job> m_pending;
    std::shared_ptr<Project> m_nextSnapshot;
    QString m_nextDocument;
    bool m_hasNextSnapshot = false;
    bool m_stop = false;
    // GUI thread only.
    quint64 m_requested = 0;
    quint64 m_delivered = 0;
    bool m_dropped = false;
};

} // namespace lvd::sim

#include "simulation_ui/TuningRunner.h"

#include "core/Project.h"

#include <QElapsedTimer>
#include <QPointer>

namespace lvd::sim {

TuningRunner::TuningRunner(QObject *parent) : QObject(parent)
{
    m_thread = std::thread([this] { work(); });
}

TuningRunner::~TuningRunner()
{
    {
        const std::lock_guard lock(m_mutex);
        m_stop = true;
        m_pending.reset();
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void TuningRunner::setSnapshot(std::shared_ptr<Project> snapshot, const QString &documentName)
{
    std::shared_ptr<Project> replaced;
    {
        const std::lock_guard lock(m_mutex);
        // A snapshot handed over earlier but never used is released here, on
        // this thread.
        replaced = std::move(m_nextSnapshot);
        m_nextSnapshot = std::move(snapshot);
        m_nextDocument = documentName;
        m_hasNextSnapshot = true;
    }
}

quint64 TuningRunner::request(const rf::LinearAnalysisSetup &analysis, const QList<rf::DesignVariable> &variables,
                              const std::vector<double> &values)
{
    const quint64 serial = ++m_requested;
    m_dropped = false;
    {
        const std::lock_guard lock(m_mutex);
        m_pending = Job{serial, analysis, variables, values};
    }
    m_wake.notify_one();
    return serial;
}

void TuningRunner::dropPending()
{
    const std::lock_guard lock(m_mutex);
    m_pending.reset();
    m_dropped = true;
}

void TuningRunner::work()
{
    std::shared_ptr<Project> snapshot;
    QString documentName;
    rf::TouchstoneCache touchstones;
    QPointer<TuningRunner> self(this);
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_wake.wait(lock, [this] { return m_stop || m_pending.has_value(); });
        if (m_stop)
            break;
        Job job = std::move(*m_pending);
        m_pending.reset();
        std::shared_ptr<Project> released;
        if (m_hasNextSnapshot) {
            released = std::move(snapshot);
            snapshot = std::move(m_nextSnapshot);
            documentName = m_nextDocument;
            m_hasNextSnapshot = false;
        }
        lock.unlock();

        if (released) {
            // Release the old snapshot on the GUI thread, which created it.
            QMetaObject::invokeMethod(this, [released] {}, Qt::QueuedConnection);
            released.reset();
        }
        QElapsedTimer clock;
        clock.start();
        std::shared_ptr<const rf::LinearResult> result;
        if (snapshot) {
            result = std::make_shared<const rf::LinearResult>(
                rf::evaluateDesign(*snapshot, documentName, job.analysis, job.variables, job.values, &touchstones));
        } else {
            auto failed = std::make_shared<rf::LinearResult>();
            failed->errors.append(QStringLiteral("no snapshot to tune"));
            result = std::move(failed);
        }
        const double milliseconds = static_cast<double>(clock.nsecsElapsed()) / 1e6;
        // The destructor joins this thread before the runner goes away, so
        // `this` is valid; the QPointer guards the queued call.
        QMetaObject::invokeMethod(
            this,
            [self, result, serial = job.serial, values = std::move(job.values), milliseconds] {
                if (self)
                    self->deliver(result, serial, values, milliseconds);
            },
            Qt::QueuedConnection);
        lock.lock();
    }
    lock.unlock();
    // Hand the last snapshot back to the GUI thread too; if the runner is
    // being destroyed the event is discarded there and the snapshot freed.
    if (snapshot) {
        QMetaObject::invokeMethod(this, [snapshot] {}, Qt::QueuedConnection);
        snapshot.reset();
    }
}

void TuningRunner::deliver(std::shared_ptr<const rf::LinearResult> result, quint64 serial, std::vector<double> values,
                           double milliseconds)
{
    m_delivered = std::max(m_delivered, serial);
    Q_EMIT evaluated(std::move(result), serial, std::move(values), milliseconds);
}

} // namespace lvd::sim

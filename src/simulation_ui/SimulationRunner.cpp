#include "simulation_ui/SimulationRunner.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThread>

namespace lvd::sim {

SimulationRunner::SimulationRunner(QObject *parent) : QObject(parent) {}

SimulationRunner::~SimulationRunner()
{
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

bool SimulationRunner::start(Netlist netlist, std::vector<double> frequencies, rf::LinearOptions options, quint64 tag)
{
    if (m_running)
        return false;
    if (m_thread.joinable())
        m_thread.join();
    m_cancel = false;
    m_running = true;
    options.touchstones = &m_touchstones;
    options.cancel = &m_cancel;
    QPointer<SimulationRunner> self(this);
    m_thread = std::thread([self, netlist = std::move(netlist), frequencies = std::move(frequencies), options, tag] {
        auto result = std::make_shared<const rf::LinearResult>(rf::simulateLinear(netlist, frequencies, options));
        // The destructor joins this thread before `self` goes away, so the
        // pointer is still valid here; the queued call runs in the runner's
        // thread, where the QPointer check is meaningful.
        SimulationRunner *runner = self.data();
        if (runner) {
            QMetaObject::invokeMethod(
                runner, [self, result, tag] {
                    if (self)
                        self->deliver(result, tag);
                },
                Qt::QueuedConnection);
        }
    });
    return true;
}

void SimulationRunner::cancel()
{
    if (m_running)
        m_cancel = true;
}

void SimulationRunner::wait()
{
    if (m_thread.joinable())
        m_thread.join();
    // Deliver the queued result now.
    QCoreApplication::sendPostedEvents(this);
}

void SimulationRunner::deliver(std::shared_ptr<const rf::LinearResult> result, quint64 tag)
{
    if (m_thread.joinable())
        m_thread.join(); // the worker has finished; joining returns at once
    m_running = false;
    Q_EMIT finished(std::move(result), tag);
}

} // namespace lvd::sim

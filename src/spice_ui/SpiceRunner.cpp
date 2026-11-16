#include "spice_ui/SpiceRunner.h"

#include <QMetaObject>

namespace lvd::spiceui {

SpiceRunner::SpiceRunner(QObject *parent) : QObject(parent) {}

SpiceRunner::~SpiceRunner()
{
    abort();
    join();
}

spice::SpiceEngine &SpiceRunner::engine() const
{
    return m_engine ? *m_engine : spice::defaultEngine();
}

bool SpiceRunner::isPaused() const
{
    return m_running && engine().isPaused();
}

void SpiceRunner::join()
{
    if (m_thread.joinable())
        m_thread.join();
}

bool SpiceRunner::start(spice::SpiceSimulation simulation, quint64 tag)
{
    if (m_running)
        return false;
    join();
    m_running = true;
    spice::SpiceEngine *engine = &this->engine();
    m_thread = std::thread([this, engine, simulation = std::move(simulation), tag]() mutable {
        spice::SpiceRunOptions options;
        // Progress arrives often; the queued emission keeps the GUI thread
        // in charge of the receivers.
        options.progress = [this](const spice::SpiceProgress &p) {
            QMetaObject::invokeMethod(this, [this, p] { Q_EMIT progress(p.analysis, p.fraction); }, Qt::QueuedConnection);
        };
        options.output = [this](const QString &line) {
            QMetaObject::invokeMethod(this, [this, line] { Q_EMIT output(line); }, Qt::QueuedConnection);
        };
        spice::executeSpiceAnalysis(simulation, *engine, options);
        auto result = std::make_shared<const spice::SpiceSimulation>(std::move(simulation));
        QMetaObject::invokeMethod(
            this,
            [this, result, tag] {
                m_running = false;
                join();
                Q_EMIT finished(result, tag);
            },
            Qt::QueuedConnection);
    });
    return true;
}

void SpiceRunner::abort()
{
    if (m_running)
        engine().abort();
}

void SpiceRunner::pause()
{
    if (m_running)
        engine().pause();
}

void SpiceRunner::resume()
{
    if (m_running)
        engine().resume();
}

} // namespace lvd::spiceui

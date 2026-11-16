#pragma once

#include "spice/SpiceAnalysis.h"

#include <QObject>

#include <atomic>
#include <memory>
#include <thread>

namespace lvd::spiceui {

// Runs prepared SPICE simulations on a worker thread (requirement NF-3), one
// at a time, with progress, pause and abort. The simulation is prepared
// (netlisted) on the GUI thread; the worker only drives the engine.
class SpiceRunner : public QObject
{
    Q_OBJECT
public:
    explicit SpiceRunner(QObject *parent = nullptr);
    ~SpiceRunner() override; // aborts a running simulation and waits for it

    // False when a simulation is still running.
    bool start(spice::SpiceSimulation simulation, quint64 tag);
    void abort();
    void pause();
    void resume();
    bool isRunning() const { return m_running; }
    bool isPaused() const;

    // The engine; the default engine unless replaced (tests).
    void setEngine(spice::SpiceEngine *engine) { m_engine = engine; }
    spice::SpiceEngine &engine() const;

Q_SIGNALS:
    // fraction is 0..1, or negative when unknown.
    void progress(const QString &analysis, double fraction);
    void output(const QString &line);
    void finished(std::shared_ptr<const spice::SpiceSimulation> simulation, quint64 tag);

private:
    void join();

    spice::SpiceEngine *m_engine = nullptr;
    std::thread m_thread;
    std::atomic_bool m_running = false;
};

} // namespace lvd::spiceui

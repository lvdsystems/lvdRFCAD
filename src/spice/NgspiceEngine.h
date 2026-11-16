#pragma once

#include "spice/SpiceEngine.h"

#include <QLibrary>
#include <QMutex>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace lvd::spice {

// ngspice loaded as a shared library at run time (requirement SP-1).
//
// ngspice keeps global state, so a process holds a single instance and runs
// one simulation at a time; run() serialises callers. The simulation runs in
// ngspice's own background thread ("bg_run") so that it can be halted
// (abort, pause) while run() waits for it. When ngspice reports a fatal
// error it asks to be unloaded; the library is then reloaded before the next
// run.
class NgspiceEngine final : public SpiceEngine
{
public:
    static NgspiceEngine &instance();

    // Folder holding ngspice.dll and lib/ngspice/*.cm. Default: "ngspice"
    // next to the application, or one level up (development builds).
    static void setRuntimeFolder(const QString &folder);
    static QString runtimeFolder();

    QString name() const override { return QStringLiteral("ngspice"); }
    QString version() const override { return QStringLiteral("47"); }
    bool isAvailable(QString *error = nullptr) override;

    SpiceRunResult run(const QString &netlist, const SpiceRunOptions &options = {}) override;

    void abort() override;
    void pause() override;
    void resume() override;
    bool isRunning() const override { return m_running; }
    bool isPaused() const override { return m_paused; }

    ~NgspiceEngine() override;

private:
    NgspiceEngine() = default;
    struct Api;

    bool load(QString *error);
    void unload();
    int command(const QString &text);
    QList<SpicePlot> collectPlots();

    // Callbacks from ngspice (any thread).
    static int onChar(char *text, int id, void *user);
    static int onStat(char *text, int id, void *user);
    static int onExit(int status, bool unload, bool quit, int id, void *user);
    static int onBackground(bool notRunning, int id, void *user);

    QMutex m_runMutex; // one run at a time
    QLibrary m_library;
    std::unique_ptr<Api> m_api;
    bool m_loaded = false;
    std::atomic_bool m_needsReload = false;

    std::atomic_bool m_running = false;
    std::atomic_bool m_paused = false;
    std::atomic_bool m_abort = false;

    // Background thread state, guarded by m_stateMutex.
    std::mutex m_stateMutex;
    std::condition_variable m_stateChanged;
    bool m_backgroundRunning = false;
    int m_backgroundEnds = 0;

    // Output of the current run, guarded by m_outputMutex.
    std::mutex m_outputMutex;
    QStringList m_log;
    QStringList m_errors;
    QStringList m_warnings;
    const SpiceRunOptions *m_options = nullptr;
};

} // namespace lvd::spice

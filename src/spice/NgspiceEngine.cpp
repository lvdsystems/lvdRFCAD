#include "spice/NgspiceEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>

#include <chrono>
#include <cstring>

#ifdef Q_OS_WIN
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace lvd::spice {

namespace {

// From ngspice's sharedspice.h (release 47). Only what is used here.
struct NgComplex {
    double real;
    double imag;
};
struct NgVectorInfo {
    char *name;
    int type;
    short flags;
    double *realData;
    NgComplex *complexData;
    int length;
};
using SendChar = int(char *, int, void *);
using SendStat = int(char *, int, void *);
using ControlledExit = int(int, bool, bool, int, void *);
using SendData = int(void *, int, int, void *);
using SendInitData = int(void *, int, void *);
using BGThreadRunning = int(bool, int, void *);

// ngspice's vector types (simulation_types in sim.h).
VectorKind kindOf(int type)
{
    switch (type) {
    case 1: return VectorKind::Time;
    case 2: return VectorKind::Frequency;
    case 3: return VectorKind::Voltage;
    case 4: return VectorKind::Current;
    case 5: return VectorKind::VoltageDensity;
    case 6: return VectorKind::CurrentDensity;
    case 11: return VectorKind::Pole;
    case 12: return VectorKind::Zero;
    case 14: return VectorKind::Temperature;
    case 16: return VectorKind::Impedance;
    case 17: return VectorKind::Admittance;
    case 18: return VectorKind::Power;
    default: return VectorKind::Other;
    }
}

QString folderIfPresent(const QString &folder)
{
    return QFileInfo::exists(folder + QStringLiteral("/ngspice.dll")) ? QDir::cleanPath(folder) : QString();
}

QString g_runtimeFolder;

// A path ngspice can take as a single word.
QString withoutSpaces(const QString &path)
{
#ifdef Q_OS_WIN
    if (path.contains(QLatin1Char(' '))) {
        const std::wstring longPath = QDir::toNativeSeparators(path).toStdWString();
        wchar_t shortPath[MAX_PATH];
        const DWORD length = GetShortPathNameW(longPath.c_str(), shortPath, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
            return QDir::fromNativeSeparators(QString::fromWCharArray(shortPath, int(length)));
    }
#endif
    return path;
}

// "tran1" -> "tran", "noise2" -> "noise".
QString plotType(const QString &plotName)
{
    QString type = plotName;
    while (!type.isEmpty() && type.back().isDigit())
        type.chop(1);
    return type;
}

} // namespace

struct NgspiceEngine::Api {
    int (*init)(SendChar *, SendStat *, ControlledExit *, SendData *, SendInitData *, BGThreadRunning *, void *) = nullptr;
    int (*command)(char *) = nullptr;
    NgVectorInfo *(*vectorInfo)(char *) = nullptr;
    int (*circuit)(char **) = nullptr;
    char **(*allPlots)() = nullptr;
    char **(*allVectors)(char *) = nullptr;
    bool (*running)() = nullptr;
};

NgspiceEngine &NgspiceEngine::instance()
{
    static NgspiceEngine engine;
    return engine;
}

NgspiceEngine::~NgspiceEngine()
{
    // ngspice's background thread and atexit handlers make unloading at
    // process exit fragile; the operating system cleans up.
}

void NgspiceEngine::setRuntimeFolder(const QString &folder)
{
    g_runtimeFolder = folder;
}

QString NgspiceEngine::runtimeFolder()
{
    if (!g_runtimeFolder.isEmpty())
        return g_runtimeFolder;
    const QString environment = qEnvironmentVariable("LVD_NGSPICE");
    if (!environment.isEmpty())
        return environment;
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString &candidate : {appDir + QStringLiteral("/ngspice"), appDir + QStringLiteral("/../ngspice")}) {
        const QString found = folderIfPresent(candidate);
        if (!found.isEmpty())
            return found;
    }
    return appDir + QStringLiteral("/ngspice");
}

bool NgspiceEngine::isAvailable(QString *error)
{
    QMutexLocker locker(&m_runMutex);
    return load(error);
}

bool NgspiceEngine::load(QString *error)
{
    if (m_loaded && !m_needsReload)
        return true;
    if (m_loaded)
        unload();

    const QString folder = runtimeFolder();
    const QString path = folder + QStringLiteral("/ngspice.dll");
    if (!QFileInfo::exists(path)) {
        if (error)
            *error = QCoreApplication::translate("lvd::spice", "ngspice was not found (%1).").arg(QDir::toNativeSeparators(path));
        return false;
    }
    // The DLLs ngspice imports sit next to it. Windows does not search the
    // folder of a library loaded by path, so they are loaded first; ngspice
    // then finds them already in the process.
    const QDir runtime(folder);
    for (const QString &dependency : runtime.entryList({QStringLiteral("*.dll")}, QDir::Files, QDir::Name)) {
        if (dependency.compare(QStringLiteral("ngspice.dll"), Qt::CaseInsensitive) == 0)
            continue;
        QLibrary preload(runtime.absoluteFilePath(dependency));
        preload.load(); // stays loaded: QLibrary does not unload on destruction
    }
    // ngspice reads its start-up script "spinit" from SPICE_SCRIPTS. The one
    // in the ngspice package names fixed installation paths (C:/Spice64), so
    // the runtime folder has its own, and the code models are loaded below.
    // Set before loading: the DLL's C runtime copies the environment then.
    qputenv("SPICE_SCRIPTS", QDir::toNativeSeparators(folder + QStringLiteral("/share/ngspice/scripts")).toLocal8Bit());
    m_library.setFileName(path);
    if (!m_library.load()) {
        if (error)
            *error = QCoreApplication::translate("lvd::spice", "ngspice could not be loaded: %1").arg(m_library.errorString());
        return false;
    }

    auto api = std::make_unique<Api>();
    bool complete = true;
    auto resolve = [&](auto &function, const char *symbol) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(m_library.resolve(symbol));
        complete = complete && function;
    };
    resolve(api->init, "ngSpice_Init");
    resolve(api->command, "ngSpice_Command");
    resolve(api->vectorInfo, "ngGet_Vec_Info");
    resolve(api->circuit, "ngSpice_Circ");
    resolve(api->allPlots, "ngSpice_AllPlots");
    resolve(api->allVectors, "ngSpice_AllVecs");
    resolve(api->running, "ngSpice_running");
    if (!complete) {
        m_library.unload();
        if (error)
            *error = QCoreApplication::translate("lvd::spice", "%1 is not a compatible ngspice shared library.")
                         .arg(QDir::toNativeSeparators(path));
        return false;
    }
    m_api = std::move(api);

    m_api->init(&NgspiceEngine::onChar, &NgspiceEngine::onStat, &NgspiceEngine::onExit, nullptr, nullptr,
                &NgspiceEngine::onBackground, this);
    m_loaded = true;
    m_needsReload = false;

    // XSPICE code models. "codemodel" takes the path as one word and keeps
    // quotes, so a folder with spaces (C:/Program Files/...) is given by its
    // short name.
    const QDir models(folder + QStringLiteral("/lib/ngspice"));
    for (const QString &file : models.entryList({QStringLiteral("*.cm")}, QDir::Files, QDir::Name))
        command(QStringLiteral("codemodel %1").arg(withoutSpaces(models.absoluteFilePath(file))));
    // Interactive behaviour does not suit a library: no "more" prompts, no
    // questions on quit.
    command(QStringLiteral("set nomoremode"));
    command(QStringLiteral("unset askquit"));
    {
        std::lock_guard lock(m_outputMutex);
        m_log.clear();
        m_errors.clear();
        m_warnings.clear();
    }
    return true;
}

void NgspiceEngine::unload()
{
    m_api.reset();
    m_library.unload();
    m_loaded = false;
}

int NgspiceEngine::command(const QString &text)
{
    QByteArray bytes = text.toLocal8Bit();
    return m_api->command(bytes.data());
}

SpiceRunResult NgspiceEngine::run(const QString &netlist, const SpiceRunOptions &options)
{
    QMutexLocker locker(&m_runMutex);
    SpiceRunResult result;
    QElapsedTimer timer;
    timer.start();

    QString loadError;
    if (!load(&loadError)) {
        result.errors << loadError;
        return result;
    }

    {
        std::lock_guard lock(m_outputMutex);
        m_log.clear();
        m_errors.clear();
        m_warnings.clear();
        m_options = &options;
    }
    m_abort = false;
    m_paused = false;
    m_running = true;

    // Forget earlier circuits and results.
    command(QStringLiteral("destroy all"));
    command(QStringLiteral("remcirc"));
    {
        std::lock_guard lock(m_outputMutex);
        // "no circuit" messages from remcirc on a fresh engine.
        m_errors.clear();
        m_warnings.clear();
        m_log.clear();
    }

    // ngspice takes the deck as an array of lines; the first is the title.
    QStringList lines = netlist.split(QLatin1Char('\n'));
    for (QString &line : lines)
        line.remove(QLatin1Char('\r'));
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
        lines.removeLast();
    if (lines.isEmpty() || lines.last().trimmed().compare(QStringLiteral(".end"), Qt::CaseInsensitive) != 0)
        lines << QStringLiteral(".end");
    if (!options.save.isEmpty())
        lines.insert(lines.size() - 1, QStringLiteral(".save ") + options.save.join(QLatin1Char(' ')));
    std::vector<QByteArray> storage;
    storage.reserve(std::size_t(lines.size()));
    std::vector<char *> pointers;
    for (const QString &line : std::as_const(lines)) {
        storage.push_back(line.toLocal8Bit());
        pointers.push_back(storage.back().data());
    }
    pointers.push_back(nullptr);
    const int circuitStatus = m_api->circuit(pointers.data());

    bool loaded = circuitStatus == 0 && !m_needsReload;
    {
        std::lock_guard lock(m_outputMutex);
        loaded = loaded && m_errors.isEmpty();
    }

    if (loaded) {
        {
            std::lock_guard lock(m_stateMutex);
            m_backgroundEnds = 0;
            m_backgroundRunning = true; // until ngspice reports the thread's end
        }
        QElapsedTimer started;
        started.start();
        command(QStringLiteral("bg_run"));

        // Wait for the background thread to end. A pause also ends it; the
        // run then waits for resume() or abort().
        std::unique_lock lock(m_stateMutex);
        for (;;) {
            m_stateChanged.wait_for(lock, std::chrono::milliseconds(50));
            if (m_needsReload)
                break;
            if (m_paused && !m_abort) {
                started.restart();
                continue;
            }
            if (m_backgroundRunning) {
                // Guard against bg_run not starting a thread at all.
                if (m_api->running() || started.elapsed() < 2000)
                    continue;
            }
            break;
        }
    }

    if (!m_needsReload)
        result.plots = collectPlots();

    {
        std::lock_guard lock(m_outputMutex);
        result.log = m_log;
        result.errors = m_errors;
        result.warnings = m_warnings;
        m_options = nullptr;
    }
    if (m_needsReload)
        result.errors << QCoreApplication::translate("lvd::spice", "ngspice stopped after a fatal error; it is restarted for the next run.");
    result.cancelled = m_abort;
    result.ok = loaded && !result.cancelled && result.errors.isEmpty() && !result.plots.isEmpty();
    if (loaded && result.plots.isEmpty() && result.errors.isEmpty() && !result.cancelled)
        result.errors << QCoreApplication::translate("lvd::spice", "The simulation produced no results.");
    result.seconds = double(timer.nsecsElapsed()) * 1e-9;
    m_running = false;
    m_paused = false;
    return result;
}

QList<SpicePlot> NgspiceEngine::collectPlots()
{
    QList<SpicePlot> plots;
    char **names = m_api->allPlots();
    QStringList plotNames;
    for (int i = 0; names && names[i]; ++i)
        plotNames << QString::fromLocal8Bit(names[i]);
    // Newest first; "const" holds constants and is not a result.
    std::reverse(plotNames.begin(), plotNames.end());
    for (const QString &plotName : std::as_const(plotNames)) {
        if (plotName == QLatin1String("const"))
            continue;
        SpicePlot plot;
        plot.name = plotName;
        plot.type = plotType(plotName);
        QByteArray plotBytes = plotName.toLocal8Bit();
        char **vectors = m_api->allVectors(plotBytes.data());
        for (int i = 0; vectors && vectors[i]; ++i) {
            const QString vectorName = QString::fromLocal8Bit(vectors[i]);
            QByteArray full = (plotName + QLatin1Char('.') + vectorName).toLocal8Bit();
            const NgVectorInfo *info = m_api->vectorInfo(full.data());
            if (!info)
                continue;
            SpiceVector v;
            v.kind = kindOf(info->type);
            v.name = normalisedVectorName(vectorName, v.kind, plot.type);
            const int n = std::max(0, info->length);
            if (info->complexData && (v.kind == VectorKind::Frequency || v.kind == VectorKind::Time)) {
                // ngspice stores the AC frequency scale as complex numbers.
                v.real.resize(std::size_t(n));
                for (int k = 0; k < n; ++k)
                    v.real[std::size_t(k)] = info->complexData[k].real;
            } else if (info->complexData) {
                v.isComplex = true;
                v.complex.resize(std::size_t(n));
                for (int k = 0; k < n; ++k)
                    v.complex[std::size_t(k)] = {info->complexData[k].real, info->complexData[k].imag};
            } else if (info->realData) {
                v.real.assign(info->realData, info->realData + n);
            }
            plot.vectors.push_back(std::move(v));
        }
        // The scale: time, frequency, or the swept quantity of a DC sweep.
        for (const SpiceVector &v : plot.vectors) {
            if (v.name == QLatin1String("time") || v.name == QLatin1String("frequency") || v.name.endsWith(QLatin1String("-sweep"))
                || v.name == QLatin1String("sweep")) {
                plot.scaleName = v.name;
                break;
            }
        }
        // Scale first, the rest sorted, for a stable order.
        std::stable_sort(plot.vectors.begin(), plot.vectors.end(), [&](const SpiceVector &a, const SpiceVector &b) {
            const bool aScale = a.name == plot.scaleName, bScale = b.name == plot.scaleName;
            if (aScale != bScale)
                return aScale;
            return a.name < b.name;
        });
        plots << plot;
    }
    return plots;
}

void NgspiceEngine::abort()
{
    if (!m_running)
        return;
    m_abort = true;
    if (m_loaded && m_api && m_api->running())
        command(QStringLiteral("bg_halt"));
    m_stateChanged.notify_all();
}

void NgspiceEngine::pause()
{
    if (!m_running || m_paused || !m_api)
        return;
    m_paused = true;
    command(QStringLiteral("bg_halt"));
}

void NgspiceEngine::resume()
{
    if (!m_running || !m_paused || !m_api)
        return;
    {
        std::lock_guard lock(m_stateMutex);
        m_backgroundRunning = true;
    }
    m_paused = false;
    command(QStringLiteral("bg_resume"));
    m_stateChanged.notify_all();
}

int NgspiceEngine::onChar(char *text, int, void *user)
{
    auto *self = static_cast<NgspiceEngine *>(user);
    QString line = QString::fromLocal8Bit(text);
    bool isError = false;
    if (line.startsWith(QLatin1String("stdout ")))
        line.remove(0, 7);
    else if (line.startsWith(QLatin1String("stderr "))) {
        line.remove(0, 7);
        isError = true;
    }
    if (qEnvironmentVariableIsSet("LVD_NGSPICE_TRACE"))
        fprintf(stderr, "ngspice: %s\n", qPrintable(line));
    std::lock_guard lock(self->m_outputMutex);
    self->m_log << line;
    if (isError) {
        const QString lower = line.toLower();
        if (lower.startsWith(QLatin1String("note:")))
            ; // information only; kept in the log
        else if (lower.contains(QLatin1String("warning")))
            self->m_warnings << line;
        else if (lower.contains(QLatin1String("error")) || lower.contains(QLatin1String("singular matrix"))
                 || lower.contains(QLatin1String("timestep too small")) || lower.contains(QLatin1String("unknown"))
                 || lower.contains(QLatin1String("could not find")) || lower.contains(QLatin1String("can't find"))
                 || lower.contains(QLatin1String("no such")))
            self->m_errors << line;
        else if (!line.trimmed().isEmpty())
            self->m_warnings << line;
    }
    if (self->m_options && self->m_options->output)
        self->m_options->output(line);
    return 0;
}

int NgspiceEngine::onStat(char *text, int, void *user)
{
    auto *self = static_cast<NgspiceEngine *>(user);
    // "tran: 34.5%", "--ready--"
    static const QRegularExpression pattern(QStringLiteral("^\\s*([A-Za-z ]+?)\\s*:?\\s*([0-9.]+)\\s*%"));
    const QString status = QString::fromLocal8Bit(text);
    SpiceProgress progress;
    const QRegularExpressionMatch match = pattern.match(status);
    if (match.hasMatch()) {
        progress.analysis = match.captured(1).trimmed().toLower();
        progress.fraction = match.captured(2).toDouble() / 100.0;
    } else {
        progress.analysis = status.trimmed();
    }
    std::lock_guard lock(self->m_outputMutex);
    if (self->m_options && self->m_options->progress)
        self->m_options->progress(progress);
    return 0;
}

int NgspiceEngine::onExit(int status, bool, bool, int, void *user)
{
    auto *self = static_cast<NgspiceEngine *>(user);
    self->m_needsReload = true;
    {
        std::lock_guard lock(self->m_outputMutex);
        self->m_log << QStringLiteral("ngspice exit, status %1").arg(status);
    }
    self->m_stateChanged.notify_all();
    return 0;
}

int NgspiceEngine::onBackground(bool notRunning, int, void *user)
{
    auto *self = static_cast<NgspiceEngine *>(user);
    {
        std::lock_guard lock(self->m_stateMutex);
        self->m_backgroundRunning = !notRunning;
        if (notRunning)
            ++self->m_backgroundEnds;
    }
    self->m_stateChanged.notify_all();
    return 0;
}

} // namespace lvd::spice

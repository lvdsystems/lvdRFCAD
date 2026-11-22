#include "fullwave/Simulation.h"

#include "antenna/PatternFile.h"
#include "core/JsonFile.h"
#include "fullwave/OpenEmsXml.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>

namespace lvd::fw {

namespace {

QJsonArray toArray(const std::vector<double> &v)
{
    QJsonArray a;
    for (double x : v)
        a.append(x);
    return a;
}

std::vector<double> fromArray(const QJsonValue &v)
{
    std::vector<double> out;
    for (const QJsonValue &x : v.toArray())
        out.push_back(x.toDouble());
    return out;
}

bool writeText(const QString &path, const QString &text, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        setError(error, QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
        return false;
    }
    f.write(text.toUtf8());
    return true;
}

// Runs a program to completion, passing each output line to `line`; kills it
// when *cancel becomes true. Returns the exit code, or -1 when it could not
// start or was cancelled.
int runProcess(const QString &program, const QStringList &arguments, const QString &folder, const std::atomic_bool *cancel,
               const std::function<void(const QString &)> &line, QString *log)
{
    QProcess process;
    process.setWorkingDirectory(folder);
    process.setProcessChannelMode(QProcess::MergedChannels);
    // The solver's DLLs are next to it.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"), QFileInfo(program).absolutePath() + QLatin1Char(';') + env.value(QStringLiteral("PATH")));
    process.setProcessEnvironment(env);
    process.start(program, arguments);
    if (!process.waitForStarted(30000)) {
        if (log)
            *log += QStringLiteral("cannot start %1: %2\n").arg(program, process.errorString());
        return -1;
    }
    QByteArray pending;
    auto drain = [&] {
        pending += process.readAll();
        qsizetype nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            const QString text = QString::fromLocal8Bit(pending.left(nl)).trimmed();
            pending.remove(0, nl + 1);
            if (log)
                *log += text + QLatin1Char('\n');
            if (line)
                line(text);
        }
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        drain();
        if (cancel && cancel->load()) {
            process.kill();
            process.waitForFinished(10000);
            return -1;
        }
    }
    drain();
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

} // namespace

QString openEmsDirectory()
{
    const QString env = qEnvironmentVariable("LVD_OPENEMS_DIR");
    if (!env.isEmpty())
        return env;
    QStringList candidates;
    if (QCoreApplication::instance()) {
        const QString app = QCoreApplication::applicationDirPath();
        candidates << app + QStringLiteral("/openems") << app + QStringLiteral("/../openems");
    }
    for (const QString &c : candidates)
        if (QFileInfo::exists(c + QStringLiteral("/openEMS.exe")))
            return QDir(c).absolutePath();
    return candidates.isEmpty() ? QString() : candidates.front();
}

bool openEmsAvailable()
{
    const QString dir = openEmsDirectory();
    return QFileInfo::exists(dir + QStringLiteral("/openEMS.exe")) && QFileInfo::exists(dir + QStringLiteral("/nf2ff.exe"));
}

QString FwResults::patternFile() const
{
    return QDir(folder).filePath(QStringLiteral("pattern.csv"));
}

double FwResults::resonance() const
{
    if (port.s11.empty())
        return NAN;
    std::size_t best = 0;
    for (std::size_t k = 1; k < port.s11.size(); ++k)
        if (std::abs(port.s11[k]) < std::abs(port.s11[best]))
            best = k;
    return port.frequency[best];
}

FwResults runFullWave(const FwModel &model, const FwSettings &settings, const QString &folderPath,
                      const std::atomic_bool *cancel, const ProgressCallback &progress)
{
    FwResults r;
    QElapsedTimer timer;
    timer.start();
    QDir folder(folderPath);
    r.folder = folder.absolutePath();
    auto report = [&](double f, const QString &m) {
        if (progress)
            progress(std::clamp(f, 0.0, 1.0), m);
    };

    if (!openEmsAvailable()) {
        r.error = QStringLiteral("openEMS was not found in %1").arg(openEmsDirectory());
        return r;
    }
    if (model.ports.empty()) {
        r.error = QStringLiteral("the model has no port");
        return r;
    }
    if (!QDir().mkpath(r.folder)) {
        r.error = QStringLiteral("cannot create %1").arg(r.folder);
        return r;
    }
    // Remove earlier results.
    for (const QString &name : folder.entryList(QDir::Files))
        if (name.startsWith(QLatin1String("port_")) || name.startsWith(QLatin1String(kNearFieldPrefix))
            || name.endsWith(QLatin1String(".xml")) || name == QLatin1String("results.json") || name == QLatin1String("farfield.h5")
            || name == QLatin1String("pattern.csv") || name == QLatin1String("openems.log") || name == QLatin1String("et")
            || name == QLatin1String("ht"))
            folder.remove(name);

    report(0, QStringLiteral("Meshing"));
    const FwMesh mesh = buildMesh(model, settings, &r.warnings);
    r.cells = mesh.cells();
    r.warnings << model.warnings;
    QString error;
    if (!writeText(folder.filePath(QStringLiteral("simulation.xml")), openEmsXml(model, mesh, settings), &error)) {
        r.error = error;
        return r;
    }

    // openEMS: progress from "Timestep: N" and the energy decay "(-X dB)".
    report(0.01, QStringLiteral("Running openEMS (%1 cells)").arg(r.cells));
    const QRegularExpression timestep(QStringLiteral("Timestep:\\s*(\\d+)"));
    const QRegularExpression decay(QStringLiteral("\\(-\\s*([0-9.]+)dB\\)"));
    QString log;
    const QString dir = openEmsDirectory();
    const int code = runProcess(dir + QStringLiteral("/openEMS.exe"), {QStringLiteral("simulation.xml")}, r.folder, cancel,
                                [&](const QString &line) {
                                    const auto t = timestep.match(line);
                                    if (!t.hasMatch())
                                        return;
                                    r.timesteps = t.captured(1).toInt();
                                    double fraction = double(r.timesteps) / settings.maxTimesteps;
                                    const auto d = decay.match(line);
                                    if (d.hasMatch())
                                        fraction = std::max(fraction, d.captured(1).toDouble() / std::abs(settings.endCriteriaDb));
                                    report(0.02 + 0.85 * std::min(fraction, 1.0), line);
                                },
                                &log);
    writeText(folder.filePath(QStringLiteral("openems.log")), log, nullptr);
    if (cancel && cancel->load()) {
        r.cancelled = true;
        r.error = QStringLiteral("cancelled");
        return r;
    }
    if (code != 0) {
        const QStringList lines = log.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        r.error = QStringLiteral("openEMS failed (exit code %1): %2").arg(code).arg(lines.mid(std::max<qsizetype>(0, lines.size() - 5)).join(QLatin1Char(' ')));
        return r;
    }
    if (r.timesteps >= settings.maxTimesteps - 1)
        r.warnings << QStringLiteral("the run stopped at the time-step limit before the energy decayed by %1 dB; results may be inaccurate")
                          .arg(std::abs(settings.endCriteriaDb));

    // Port quantities.
    report(0.88, QStringLiteral("Reading the ports"));
    const FwPort &p1 = model.ports.front();
    const auto u = readProbeFile(folder.filePath(portVoltageFile(p1.number)), &error);
    const auto i = readProbeFile(folder.filePath(portCurrentFile(p1.number)), &error);
    if (!u || !i) {
        r.error = error;
        return r;
    }
    std::vector<double> freq;
    const int n = std::max(2, settings.frequencyPoints);
    for (int k = 0; k < n; ++k)
        freq.push_back(settings.fStart + (settings.fStop - settings.fStart) * k / (n - 1));
    r.port = lumpedPortResult(*u, *i, freq, p1.resistance);

    // Far field: the requested frequencies, or the band edges, centre and S11 minimum.
    std::vector<double> ff = settings.farFieldFrequencies;
    if (ff.empty())
        ff = {settings.fStart, settings.centre(), settings.fStop, r.resonance()};
    std::sort(ff.begin(), ff.end());
    ff.erase(std::unique(ff.begin(), ff.end(), [](double a, double b) { return std::abs(a - b) < 1; }), ff.end());
    std::vector<QString> eFiles, hFiles;
    for (const QString &name : folder.entryList({QStringLiteral("%1_E_*.h5").arg(QLatin1String(kNearFieldPrefix))}, QDir::Files, QDir::Name))
        eFiles.push_back(name);
    for (const QString &name : folder.entryList({QStringLiteral("%1_H_*.h5").arg(QLatin1String(kNearFieldPrefix))}, QDir::Files, QDir::Name))
        hFiles.push_back(name);
    if (eFiles.size() != 6 || hFiles.size() != 6) {
        r.error = QStringLiteral("openEMS wrote %1 near-field files instead of 12").arg(eFiles.size() + hFiles.size());
        return r;
    }
    std::vector<double> theta, phi;
    const double step = std::clamp(settings.farFieldStepDeg, 0.5, 15.0);
    for (double t = 0; t <= 180 + 1e-9; t += step)
        theta.push_back(t);
    for (double p = 0; p < 360 - 1e-9; p += step)
        phi.push_back(p);
    if (!writeText(folder.filePath(QStringLiteral("nf2ff.xml")),
                   nf2ffXml(eFiles, hFiles, ff, theta, phi, model.farFieldCentre, QStringLiteral("farfield.h5")), &error)) {
        r.error = error;
        return r;
    }
    report(0.9, QStringLiteral("Computing the far field"));
    QString nfLog;
    const int nfCode = runProcess(dir + QStringLiteral("/nf2ff.exe"), {QStringLiteral("nf2ff.xml")}, r.folder, cancel, {}, &nfLog);
    if (cancel && cancel->load()) {
        r.cancelled = true;
        r.error = QStringLiteral("cancelled");
        return r;
    }
    if (nfCode != 0) {
        r.error = QStringLiteral("nf2ff failed (exit code %1): %2").arg(nfCode).arg(nfLog.right(400));
        return r;
    }
    const auto farField = readFarField(folder.filePath(QStringLiteral("farfield.h5")), &error);
    if (!farField) {
        r.error = error;
        return r;
    }
    // The near-field dumps are large and no longer needed.
    for (const QString &name : eFiles)
        folder.remove(name);
    for (const QString &name : hFiles)
        folder.remove(name);

    const ant::PatternTable table = toPatternTable(*farField, QStringLiteral("openEMS"));
    if (!ant::writePatternCsv(table, r.patternFile(), &error)) {
        r.error = error;
        return r;
    }
    r.farFieldFrequencies = farField->frequency;
    r.radiatedPower = farField->radiatedPower;
    // Efficiency against the accepted power, and the peak directivity of the full grid.
    ant::ElementSpec spec;
    spec.model = ant::ElementModelKind::Imported;
    spec.pattern = std::make_shared<ant::PatternTable>(table);
    const ant::ElementPattern pattern(spec);
    for (std::size_t k = 0; k < farField->frequency.size(); ++k) {
        const double f = farField->frequency[k];
        // Accepted power from the DFT at exactly f.
        const PortResult at = lumpedPortResult(*u, *i, {f}, p1.resistance);
        const double accepted = at.acceptedPower.front();
        const double prad = k < farField->radiatedPower.size() ? farField->radiatedPower[k] : NAN;
        r.radiationEfficiency.push_back(accepted > 0 && std::isfinite(prad) ? std::min(prad / accepted, 1.0) : NAN);
        r.directivityDbi.push_back(pattern.peakDirectivityDbi(f));
    }
    r.seconds = timer.elapsed() / 1000.0;
    r.ok = true;

    QJsonArray s11re, s11im, zre, zim;
    for (std::size_t k = 0; k < r.port.frequency.size(); ++k) {
        s11re.append(r.port.s11[k].real());
        s11im.append(r.port.s11[k].imag());
        zre.append(r.port.zin[k].real());
        zim.append(r.port.zin[k].imag());
    }
    QJsonObject json{{QStringLiteral("format"), QStringLiteral("lvdrfcad-fullwave-results")},
                     {QStringLiteral("version"), 1},
                     {QStringLiteral("frequency"), toArray(r.port.frequency)},
                     {QStringLiteral("s11Re"), s11re},
                     {QStringLiteral("s11Im"), s11im},
                     {QStringLiteral("zinRe"), zre},
                     {QStringLiteral("zinIm"), zim},
                     {QStringLiteral("zRef"), r.port.zRef},
                     {QStringLiteral("farFieldFrequencies"), toArray(r.farFieldFrequencies)},
                     {QStringLiteral("radiatedPower"), toArray(r.radiatedPower)},
                     {QStringLiteral("radiationEfficiency"), toArray(r.radiationEfficiency)},
                     {QStringLiteral("directivity"), toArray(r.directivityDbi)},
                     {QStringLiteral("cells"), double(r.cells)},
                     {QStringLiteral("timesteps"), r.timesteps},
                     {QStringLiteral("seconds"), r.seconds},
                     {QStringLiteral("warnings"), QJsonArray::fromStringList(r.warnings)}};
    writeJsonFile(folder.filePath(QStringLiteral("results.json")), json, nullptr);
    report(1, QStringLiteral("Done"));
    return r;
}

std::optional<FwResults> loadResults(const QString &folderPath, QString *error)
{
    const QString path = QDir(folderPath).filePath(QStringLiteral("results.json"));
    const auto json = readJsonFile(path, error);
    if (!json)
        return std::nullopt;
    if (!checkFileHeader(*json, QStringLiteral("lvdrfcad-fullwave-results"), 1, path, error))
        return std::nullopt;
    FwResults r;
    r.ok = true;
    r.folder = QDir(folderPath).absolutePath();
    r.port.frequency = fromArray(json->value(QStringLiteral("frequency")));
    const auto s11re = fromArray(json->value(QStringLiteral("s11Re"))), s11im = fromArray(json->value(QStringLiteral("s11Im")));
    const auto zre = fromArray(json->value(QStringLiteral("zinRe"))), zim = fromArray(json->value(QStringLiteral("zinIm")));
    for (std::size_t k = 0; k < r.port.frequency.size() && k < s11re.size() && k < zre.size(); ++k) {
        r.port.s11.emplace_back(s11re[k], s11im.size() > k ? s11im[k] : 0.0);
        r.port.zin.emplace_back(zre[k], zim.size() > k ? zim[k] : 0.0);
    }
    r.port.zRef = json->value(QStringLiteral("zRef")).toDouble(50);
    r.farFieldFrequencies = fromArray(json->value(QStringLiteral("farFieldFrequencies")));
    r.radiatedPower = fromArray(json->value(QStringLiteral("radiatedPower")));
    r.radiationEfficiency = fromArray(json->value(QStringLiteral("radiationEfficiency")));
    r.directivityDbi = fromArray(json->value(QStringLiteral("directivity")));
    r.cells = std::size_t(json->value(QStringLiteral("cells")).toDouble());
    r.timesteps = json->value(QStringLiteral("timesteps")).toInt();
    r.seconds = json->value(QStringLiteral("seconds")).toDouble();
    for (const QJsonValue &w : json->value(QStringLiteral("warnings")).toArray())
        r.warnings << w.toString();
    return r;
}

} // namespace lvd::fw

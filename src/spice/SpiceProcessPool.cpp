#include "spice/SpiceProcessPool.h"

#include "spice/RawFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <memory>

namespace lvd::spice {

SpiceProcessPool::SpiceProcessPool(int workers)
    : m_workers(workers > 0 ? workers : std::max(1, QThread::idealThreadCount()))
{
}

QString SpiceProcessPool::program() const
{
    if (!m_program.isEmpty())
        return m_program;
#ifdef Q_OS_WIN
    return QCoreApplication::applicationDirPath() + QStringLiteral("/lvdrfcad-cli.exe");
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/lvdrfcad-cli");
#endif
}

void SpiceProcessPool::execute(QList<SpiceSimulation *> &simulations, const std::atomic_bool *cancel,
                               const std::function<void(int done)> &progress) const
{
    QTemporaryDir folder;
    if (!folder.isValid()) {
        for (SpiceSimulation *simulation : simulations)
            simulation->errors << QStringLiteral("no temporary folder for the worker processes");
        return;
    }
    struct Job {
        SpiceSimulation *simulation;
        QString netlist;
        QString raw;
        std::unique_ptr<QProcess> process;
        QElapsedTimer timer;
    };
    std::vector<Job> jobs;
    for (qsizetype i = 0; i < simulations.size(); ++i) {
        Job job;
        job.simulation = simulations.at(i);
        job.netlist = folder.filePath(QStringLiteral("run%1.cir").arg(i));
        job.raw = folder.filePath(QStringLiteral("run%1.raw").arg(i));
        jobs.push_back(std::move(job));
    }

    const QString worker = program();
    std::size_t next = 0;
    int running = 0;
    int done = 0;
    std::vector<Job *> active;
    const auto finish = [&](Job &job, bool killed) {
        SpiceRunResult &run = job.simulation->run;
        run.seconds = double(job.timer.nsecsElapsed()) * 1e-9;
        const QString stderrText = QString::fromLocal8Bit(job.process->readAllStandardError());
        for (const QString &line : stderrText.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            run.errors << line.trimmed();
        if (killed) {
            run.cancelled = true;
        } else if (job.process->exitStatus() != QProcess::NormalExit) {
            run.errors << QStringLiteral("the worker process stopped (%1)").arg(job.process->errorString());
        } else {
            QString error;
            if (const auto plots = readRawFile(job.raw, &error))
                run.plots = *plots;
            else if (run.errors.isEmpty())
                run.errors << error;
        }
        run.ok = !killed && job.process->exitCode() == 0 && run.errors.isEmpty() && !run.plots.isEmpty();
        finishSpiceAnalysis(*job.simulation);
        QFile::remove(job.netlist);
        QFile::remove(job.raw);
        job.process.reset();
    };

    while (next < jobs.size() || running > 0) {
        const bool stopping = cancel && cancel->load();
        // Start workers up to the limit.
        while (!stopping && running < m_workers && next < jobs.size()) {
            Job &job = jobs[next++];
            if (!job.simulation->errors.isEmpty()) {
                if (progress)
                    progress(++done);
                continue;
            }
            QFile file(job.netlist);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                job.simulation->errors << file.errorString();
                if (progress)
                    progress(++done);
                continue;
            }
            file.write(job.simulation->netlist.toLocal8Bit());
            file.close();
            job.process = std::make_unique<QProcess>();
            job.process->setProgram(worker);
            job.process->setArguments({QStringLiteral("--spice-run"), job.netlist, job.raw});
            job.process->setProcessChannelMode(QProcess::SeparateChannels);
            job.timer.start();
            job.process->start();
            if (!job.process->waitForStarted(10000)) {
                job.simulation->errors << QStringLiteral("%1 could not be started: %2")
                                              .arg(QDir::toNativeSeparators(worker), job.process->errorString());
                job.process.reset();
                if (progress)
                    progress(++done);
                continue;
            }
            active.push_back(&job);
            ++running;
        }
        if (stopping && running == 0)
            break;
        // Wait a little on each active worker.
        for (auto it = active.begin(); it != active.end();) {
            Job &job = **it;
            if (stopping) {
                job.process->kill();
                job.process->waitForFinished(5000);
                finish(job, true);
            } else if (job.process->waitForFinished(running > 1 ? 10 : 50)) {
                finish(job, false);
            } else {
                // Keep the pipe from filling: stdout is not used.
                job.process->readAllStandardOutput();
                ++it;
                continue;
            }
            it = active.erase(it);
            --running;
            if (progress)
                progress(++done);
        }
    }
}

int SpiceProcessPool::runWorker(const QString &netlistPath, const QString &rawPath)
{
    QFile file(netlistPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "%s: %s\n", qPrintable(netlistPath), qPrintable(file.errorString()));
        return 2;
    }
    const QString netlist = QString::fromLocal8Bit(file.readAll());
    const SpiceRunResult result = defaultEngine().run(netlist);
    for (const QString &error : result.errors)
        std::fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
    QString error;
    if (!result.plots.isEmpty() && !writeRawFile(result.plots, rawPath, &error)) {
        std::fprintf(stderr, "%s\n", error.toLocal8Bit().constData());
        return 1;
    }
    return result.ok ? 0 : 1;
}

} // namespace lvd::spice

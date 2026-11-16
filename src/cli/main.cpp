// Runs a Python script against the application core without the GUI.

#include "core/Session.h"
#include "python/PythonInterpreter.h"
#include "spice/SpiceProcessPool.h"

#include <QCoreApplication>

#include <cstdio>
#include <exception>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("lvdrfcad-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(LVD_VERSION));

    const QStringList arguments = QCoreApplication::arguments();
    // Worker process of parallel SPICE batches (Monte Carlo, sweeps).
    if (arguments.size() == 4 && arguments.at(1) == QLatin1String("--spice-run"))
        return lvd::spice::SpiceProcessPool::runWorker(arguments.at(2), arguments.at(3));
    if (arguments.size() < 2 || arguments.at(1) == QLatin1String("--help")) {
        std::fprintf(stderr, "lvd RF cad %s\nUsage: lvdrfcad-cli <script.py> [arguments...]\n       lvdrfcad-cli --spice-run <netlist.cir> <result.raw>\n", LVD_VERSION);
        return arguments.size() < 2 ? 2 : 0;
    }

    lvd::Session session;
    try {
        lvd::PythonInterpreter python(&session);
        QObject::connect(&python, &lvd::PythonInterpreter::output, [](const QString &text, bool isError) {
            FILE *stream = isError ? stderr : stdout;
            std::fputs(text.toUtf8().constData(), stream);
            std::fflush(stream);
        });
        return python.runFile(arguments.at(1), arguments.mid(2)) ? 0 : 1;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

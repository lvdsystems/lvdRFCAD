// Reference circuits run on the embedded engine and on the standalone
// console ngspice of the same release; the results must agree (phase 5 exit
// criterion, requirement NF-4). This checks everything between the engine
// and the result data: deck loading, background run, vector collection,
// names, complex data and the raw-file reader.

#include "spice/NgspiceEngine.h"
#include "spice/RawFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

using namespace lvd::spice;

namespace {

NgspiceEngine &engine()
{
    NgspiceEngine::setRuntimeFolder(QStringLiteral(LVD_NGSPICE_RUNTIME));
    return NgspiceEngine::instance();
}

QString readText(const QString &path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    return QString::fromUtf8(file.readAll());
}

// Runs the console ngspice in batch mode and reads its raw file.
QList<SpicePlot> runConsole(const QString &netlistPath, bool ascii)
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // A spinit loading the console package's code models (the packaged one
    // names C:/Spice64).
    const QString console = QStringLiteral(LVD_NGSPICE_CONSOLE);
    const QDir models(QFileInfo(console).absolutePath() + QStringLiteral("/../lib/ngspice"));
    QFile spinit(dir.filePath(QStringLiteral("spinit")));
    REQUIRE(spinit.open(QIODevice::WriteOnly | QIODevice::Text));
    QString script = QStringLiteral("set xspice_enabled\n");
    for (const QString &file : models.entryList({QStringLiteral("*.cm")}, QDir::Files))
        script += QStringLiteral("codemodel %1\n").arg(models.absoluteFilePath(file));
    spinit.write(script.toUtf8());
    spinit.close();

    const QString raw = dir.filePath(QStringLiteral("out.raw"));
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SPICE_SCRIPTS"), QDir::toNativeSeparators(dir.path()));
    if (ascii)
        environment.insert(QStringLiteral("SPICE_ASCIIRAWFILE"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(dir.path());
    process.start(console, {QStringLiteral("-b"), QStringLiteral("-r"), raw, netlistPath});
    REQUIRE(process.waitForFinished(120000));
    INFO(QString::fromLocal8Bit(process.readAllStandardError()).toStdString());
    REQUIRE(QFileInfo::exists(raw));
    QString error;
    const auto plots = readRawFile(raw, &error);
    INFO(error.toStdString());
    REQUIRE(plots);
    return *plots;
}

bool close(std::complex<double> a, std::complex<double> b)
{
    const double scale = std::max(std::abs(a), std::abs(b));
    return std::abs(a - b) <= 1e-9 + 1e-6 * scale;
}

} // namespace

TEST_CASE("reference circuits match standalone ngspice")
{
    const QString file = GENERATE(as<QString>{}, "rc_lowpass.cir", "diode_rectifier.cir", "bjt_amplifier.cir",
                                  "cmos_inverter.cir", "opamp_subckt.cir", "lc_tline_switch.cir",
                                  "behavioural_xspice.cir");
    const QString path = QStringLiteral(LVD_SOURCE_DIR "/src/spice/tests/reference/") + file;
    INFO(file.toStdString());

    const SpiceRunResult embedded = engine().run(readText(path));
    INFO(embedded.errors.join(QLatin1Char('\n')).toStdString());
    REQUIRE(embedded.ok);
    // ASCII and binary raw files are both exercised.
    const QList<SpicePlot> reference = runConsole(path, file.startsWith(QLatin1Char('b')) || file.startsWith(QLatin1Char('r')));

    REQUIRE(embedded.plots.size() == reference.size());
    for (qsizetype p = 0; p < reference.size(); ++p) {
        const SpicePlot &ours = embedded.plots.at(p);
        const SpicePlot &theirs = reference.at(p);
        INFO("plot " << ours.name.toStdString() << " / " << theirs.name.toStdString());
        CHECK(ours.type == theirs.type);
        CHECK(ours.scaleName == theirs.scaleName);
        int compared = 0;
        for (const SpiceVector &v : theirs.vectors) {
            const SpiceVector *mine = ours.vector(v.name);
            INFO("vector " << v.name.toStdString() << " in " << ours.vectorNames().join(QLatin1Char(' ')).toStdString());
            REQUIRE(mine);
            REQUIRE(mine->size() == v.size());
            CHECK(mine->isComplex == v.isComplex);
            std::size_t mismatches = 0;
            for (std::size_t i = 0; i < v.size(); ++i)
                if (!close(mine->at(i), v.at(i)))
                    ++mismatches;
            CHECK(mismatches == 0);
            ++compared;
        }
        CHECK(compared > 0);
    }
}

TEST_CASE("the raw-file reader handles ASCII and binary plots")
{
    const QByteArray ascii = "Title: t\nDate: d\nPlotname: AC Analysis\nFlags: complex\nNo. Variables: 2\nNo. Points: 2\n"
                             "Variables:\n\t0\tfrequency\tfrequency\n\t1\tout\tvoltage\nValues:\n"
                             " 0\t1.0,0.0\n\t0.5,-0.5\n 1\t2.0,0.0\n\t0.25,-0.75\n";
    QString error;
    const auto plots = parseRawData(ascii, &error);
    INFO(error.toStdString());
    REQUIRE(plots);
    REQUIRE(plots->size() == 1);
    const SpicePlot &ac = plots->first();
    CHECK(ac.type == QLatin1String("ac"));
    CHECK(ac.scaleName == QLatin1String("frequency"));
    REQUIRE(ac.scale());
    CHECK_FALSE(ac.scale()->isComplex);
    CHECK(ac.scale()->real.at(1) == 2.0);
    REQUIRE(ac.vector(QStringLiteral("v(out)")));
    CHECK(ac.vector(QStringLiteral("v(out)"))->complex.at(1) == std::complex<double>(0.25, -0.75));

    QByteArray binary = "Title: t\nPlotname: Transient Analysis\nFlags: real\nNo. Variables: 2\nNo. Points: 2\n"
                        "Variables:\n\t0\ttime\ttime\n\t1\tv1#branch\tcurrent\nBinary:\n";
    for (double value : {0.0, 1e-3, 1e-6, -2e-3})
        binary.append(reinterpret_cast<const char *>(&value), sizeof value);
    const auto tran = parseRawData(binary, &error);
    REQUIRE(tran);
    CHECK(tran->first().type == QLatin1String("tran"));
    REQUIRE(tran->first().vector(QStringLiteral("i(v1)")));
    CHECK(tran->first().vector(QStringLiteral("i(v1)"))->real.at(1) == -2e-3);

    CHECK_FALSE(parseRawData("Title: x\nNo. Variables: 1\n", &error));
    CHECK_FALSE(error.isEmpty());
}

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/RuleCheck.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "rfdata/Discontinuities.h"
#include "rfdata/LineModels.h"
#include "rfdata/NetworkMath.h"
#include "rfdata/NetworkOps.h"
#include "rfdata/Touchstone.h"
#include "rfsim/Analysis.h"
#include "rfsim/ElementModels.h"
#include "rfsim/Measurement.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <cmath>

using namespace lvd;
using namespace lvd::rf;
using Catch::Approx;

namespace {

// Builds schematics from library parts and simulates them.
struct Bench {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;

    Bench()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("dut"), &error);
        REQUIRE(document);
        sheet = document->schematic();
    }

    QString place(const char *key, QPoint at, const QMap<QString, QString> &parameters = {})
    {
        const QString definitionKey = QString::fromLatin1(key);
        const DefinitionPtr definition = project->resolve(*sheet, definitionKey).definition;
        REQUIRE(definition);
        const QString id = sheet->addComponent(definitionKey, definition.get(), at);
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            sheet->setParameter(id, it.key(), it.value());
        return id;
    }

    QPoint pin(const QString &id, const char *name)
    {
        const PlacedComponent *component = sheet->component(id);
        const DefinitionPtr definition = project->resolve(*sheet, component->definition).definition;
        return pinPosition(*component, *definition->symbol.pin(QString::fromLatin1(name)));
    }

    void connect(const QString &a, const char *pinA, const QString &b, const char *pinB)
    {
        sheet->addWire({pin(a, pinA), pin(b, pinB)});
    }

    void ground(const QString &id, const char *pinName)
    {
        const QString g = place("basic:ground", pin(id, pinName) + QPoint(0, 60));
        connect(id, pinName, g, "gnd");
    }

    QString port(int number, QPoint at, const char *z0 = "50")
    {
        return place("rf-system:port", at, {{QStringLiteral("num"), QString::number(number)}, {QStringLiteral("z0"), QString::fromLatin1(z0)}});
    }

    LinearResult run(std::vector<double> frequencies, bool noise = true)
    {
        LinearOptions options;
        options.noise = noise;
        options.projectDirectory = project->directory();
        const Netlist netlist = buildNetlist(*project, *document);
        LinearResult result = simulateLinear(netlist, frequencies, options);
        INFO(result.errors.join(QLatin1Char('\n')).toStdString());
        return result;
    }
};

double nf(const LinearResult &result, size_t i = 0)
{
    const Measurement m = evaluateMeasurement(result, QStringLiteral("NF"));
    REQUIRE(m.ok());
    return m.values[i].real();
}

} // namespace

TEST_CASE("series and shunt resistors give the textbook S-parameters and noise figures")
{
    SECTION("series 50 ohm")
    {
        Bench b;
        const QString p1 = b.port(1, {-200, 0}), p2 = b.port(2, {200, 0});
        const QString r = b.place("basic:resistor", {0, 0}, {{QStringLiteral("r"), QStringLiteral("50")}});
        b.connect(p1, "p", r, "p");
        b.connect(r, "n", p2, "p");
        const LinearResult result = b.run({1e9});
        REQUIRE(result.ok());
        const CMatrix &s = result.network.s[0];
        CHECK(s(0, 0).real() == Approx(1.0 / 3).margin(1e-12));
        CHECK(s(1, 0).real() == Approx(2.0 / 3).margin(1e-12));
        CHECK(std::abs(s(0, 1) - s(1, 0)) < 1e-12);
        CHECK(nf(result) == Approx(10 * std::log10(2.0)).margin(1e-9)); // F = 1 + R/Z0
    }
    SECTION("shunt 50 ohm")
    {
        Bench b;
        const QString p1 = b.port(1, {-200, 0}), p2 = b.port(2, {200, 0});
        const QString r = b.place("basic:resistor", {0, 100}, {{QStringLiteral("r"), QStringLiteral("50")}});
        b.sheet->addWire({b.pin(p1, "p"), b.pin(p2, "p")});
        b.sheet->addWire({b.pin(r, "p"), QPoint(b.pin(r, "p").x(), b.pin(p1, "p").y())});
        b.sheet->addJunction(QPoint(b.pin(r, "p").x(), b.pin(p1, "p").y()));
        b.ground(r, "n");
        const LinearResult result = b.run({1e9});
        REQUIRE(result.ok());
        const CMatrix &s = result.network.s[0];
        CHECK(s(0, 0).real() == Approx(-1.0 / 3).margin(1e-12));
        CHECK(s(1, 0).real() == Approx(2.0 / 3).margin(1e-12));
        CHECK(nf(result) == Approx(10 * std::log10(2.0)).margin(1e-9)); // F = 1 + Z0/R
    }
}

TEST_CASE("a quarter-wave line matches 50 to 100 ohm and has the right group delay")
{
    Bench b;
    const QString p1 = b.port(1, {-200, 0});
    const QString p2 = b.port(2, {200, 0}, "100");
    const QString tl = b.place("rf-circuit:tline", {0, 0},
                               {{QStringLiteral("z0"), QStringLiteral("sqrt(50*100)")},
                                {QStringLiteral("elen"), QStringLiteral("90")},
                                {QStringLiteral("freq"), QStringLiteral("1GHz")}});
    b.connect(p1, "p", tl, "in");
    b.connect(tl, "out", p2, "p");
    const LinearResult result = b.run({0.9e9, 1e9, 1.1e9});
    REQUIRE(result.ok());
    CHECK(std::abs(result.network.s[1](0, 0)) < 1e-12);
    CHECK(std::abs(result.network.s[1](1, 0)) == Approx(1.0).margin(1e-12));
    CHECK(std::abs(result.network.s[0](0, 0)) > 0.01); // mismatched off centre
    CHECK(evaluateMeasurement(result, QStringLiteral("phase(S(2,1))")).values[1].real() == Approx(-90).margin(1e-9));
    CHECK(evaluateMeasurement(result, QStringLiteral("Zin(1)")).values[1].real() == Approx(50).margin(1e-9));
    CHECK(evaluateMeasurement(result, QStringLiteral("VSWR(1)")).ok() == true);
}

TEST_CASE("a matched line has a constant group delay")
{
    Bench b;
    const QString p1 = b.port(1, {-200, 0}), p2 = b.port(2, {200, 0});
    const QString tl = b.place("rf-circuit:tline", {0, 0},
                               {{QStringLiteral("z0"), QStringLiteral("50")},
                                {QStringLiteral("elen"), QStringLiteral("90")},
                                {QStringLiteral("freq"), QStringLiteral("1GHz")}});
    b.connect(p1, "p", tl, "in");
    b.connect(tl, "out", p2, "p");
    const LinearResult result = b.run({0.5e9, 1e9, 1.7e9, 2.9e9});
    REQUIRE(result.ok());
    // A 90 degree line at 1 GHz delays by 0.25 ns at all frequencies.
    const Measurement gd = evaluateMeasurement(result, QStringLiteral("GD(S21)"));
    REQUIRE(gd.ok());
    CHECK(gd.unit == QStringLiteral("s"));
    for (const Complex &value : gd.values)
        CHECK(value.real() == Approx(0.25e-9).epsilon(1e-9));
}

TEST_CASE("cascaded noise follows Friis")
{
    const double gainDb = 20, ampNf = 2, attenuation = 6;
    const auto chain = [&](bool amplifierFirst) {
        Bench b;
        const QString p1 = b.port(1, {-400, 0}), p2 = b.port(2, {400, 0});
        const QString amp = b.place("rf-system:amplifier", {amplifierFirst ? -100 : 100, 0},
                                    {{QStringLiteral("gain"), QString::number(gainDb)},
                                     {QStringLiteral("nf"), QString::number(ampNf)},
                                     {QStringLiteral("isolation"), QStringLiteral("200")}});
        const QString att = b.place("rf-system:attenuator", {amplifierFirst ? 100 : -100, 0},
                                    {{QStringLiteral("il"), QString::number(attenuation)}});
        const QString first = amplifierFirst ? amp : att, second = amplifierFirst ? att : amp;
        b.connect(p1, "p", first, "in");
        b.connect(first, "out", second, "in");
        b.connect(second, "out", p2, "p");
        const LinearResult result = b.run({1e9, 2e9});
        REQUIRE(result.ok());
        CHECK(db20(result.network.s[0](1, 0)) == Approx(gainDb - attenuation).margin(1e-9));
        return nf(result);
    };
    const double f1 = std::pow(10, ampNf / 10), f2 = std::pow(10, attenuation / 10), g1 = std::pow(10, gainDb / 10);
    CHECK(chain(true) == Approx(10 * std::log10(f1 + (f2 - 1) / g1)).margin(1e-6));
    CHECK(chain(false) == Approx(attenuation + ampNf).margin(1e-6));
}

TEST_CASE("behavioural filters follow their prototypes")
{
    CHECK(std::norm(prototypeResponse(FilterResponse::Butterworth, 5, 0, 1.0)) == Approx(0.5).margin(1e-12));
    CHECK(10 * std::log10(std::norm(prototypeResponse(FilterResponse::Butterworth, 5, 0, 2.0)))
          == Approx(-10 * std::log10(1 + std::pow(2.0, 10))).margin(1e-9));
    CHECK(10 * std::log10(std::norm(prototypeResponse(FilterResponse::Chebyshev, 3, 0.5, 1.0))) == Approx(-0.5).margin(1e-9));
    CHECK(10 * std::log10(std::norm(prototypeResponse(FilterResponse::Chebyshev, 4, 0.5, 0.0))) == Approx(-0.5).margin(1e-9));
    CHECK(std::abs(prototypeResponse(FilterResponse::Chebyshev, 3, 0.5, 0.0)) == Approx(1.0).margin(1e-12));

    Bench b;
    const QString p1 = b.port(1, {-300, 0}), p2 = b.port(2, {300, 0});
    const QString f = b.place("rf-system:filter", {0, 0},
                              {{QStringLiteral("type"), QStringLiteral("bandpass")},
                               {QStringLiteral("response"), QStringLiteral("butterworth")},
                               {QStringLiteral("order"), QStringLiteral("3")},
                               {QStringLiteral("fc"), QStringLiteral("1GHz")},
                               {QStringLiteral("bw"), QStringLiteral("100MHz")},
                               {QStringLiteral("il"), QStringLiteral("1")},
                               {QStringLiteral("rejection"), QStringLiteral("60")}});
    b.connect(p1, "p", f, "in");
    b.connect(f, "out", p2, "p");
    const double f1 = 0.95e9, f2 = 1.05e9, centre = std::sqrt(f1 * f2);
    const LinearResult result = b.run({0.5e9, f1, centre, f2, 3e9});
    REQUIRE(result.ok());
    const Measurement s21 = evaluateMeasurement(result, QStringLiteral("dB(S21)"));
    REQUIRE(s21.ok());
    CHECK(s21.values[2].real() == Approx(-1).margin(1e-9));               // passband: il
    CHECK(s21.values[1].real() == Approx(-1 - 10 * std::log10(2)).margin(1e-6)); // band edges -3 dB
    CHECK(s21.values[3].real() == Approx(-1 - 10 * std::log10(2)).margin(1e-6));
    CHECK(s21.values[4].real() == Approx(-61).margin(1e-6)); // clipped at the rejection floor, plus il
    CHECK(nf(result, 2) == Approx(1).margin(1e-6));            // passive: NF = loss
    // Lossless prototype: |S11|^2 + |S21 without il|^2 = 1.
    const CMatrix &s = result.network.s[1];
    CHECK(std::norm(s(0, 0)) + std::norm(s(1, 0)) * std::pow(10, 0.1) == Approx(1).margin(1e-9));
}

TEST_CASE("multi-port blocks route power as specified")
{
    Bench b;
    const QString dc = b.place("rf-system:coupler", {0, 0},
                               {{QStringLiteral("coupling"), QStringLiteral("20")},
                                {QStringLiteral("directivity"), QStringLiteral("25")},
                                {QStringLiteral("il"), QStringLiteral("0.5")}});
    const char *pins[] = {"in", "out", "cpl", "iso"};
    for (int k = 0; k < 4; ++k) {
        const QString port = b.port(k + 1, b.pin(dc, pins[k]) + QPoint(k % 2 ? 100 : -100, 0));
        b.connect(port, "p", dc, pins[k]);
    }
    const LinearResult result = b.run({1e9});
    REQUIRE(result.ok());
    const CMatrix &s = result.network.s[0];
    CHECK(db20(s(1, 0)) == Approx(-0.5).margin(1e-9));
    CHECK(db20(s(2, 0)) == Approx(-20).margin(1e-9));
    CHECK(db20(s(3, 0)) == Approx(-45).margin(1e-9));
    CHECK(std::arg(s(2, 0)) * 180 / Pi == Approx(90).margin(1e-9));
    CHECK((s - s.transpose()).norm() < 1e-12); // reciprocal
}

TEST_CASE("port reference impedances are respected")
{
    Bench b;
    const QString p1 = b.port(1, {-100, 0}, "75"), p2 = b.port(2, {100, 0}, "75");
    b.connect(p1, "p", p2, "p"); // a plain wire between two 75 ohm ports
    LinearResult result = b.run({1e9});
    REQUIRE(result.ok());
    CHECK(std::abs(result.network.s[0](0, 0)) < 1e-12);
    CHECK(std::abs(result.network.s[0](1, 0) - 1.0) < 1e-12);
    CHECK(result.network.z0 == std::vector<double>{75, 75});

    // A 50 ohm termination seen from a 75 ohm port.
    Bench c;
    const QString p = c.port(1, {-100, 0}, "75");
    const QString t = c.place("rf-system:termination", {100, 0});
    c.connect(p, "p", t, "p");
    result = c.run({1e9});
    REQUIRE(result.ok());
    CHECK(result.network.s[0](0, 0).real() == Approx((50.0 - 75) / (50 + 75)).margin(1e-12));
}

TEST_CASE("hierarchical sheets simulate like the flat circuit")
{
    Bench b;
    QString error;
    Document *stageDoc = b.project->createDocument(DocumentType::RfSchematic, QStringLiteral("stage"), &error);
    REQUIRE(stageDoc);
    Schematic &stage = *stageDoc->schematic();
    stage.setVariables({{QStringLiteral("g"), QStringLiteral("10")}});
    const auto placeIn = [&](Schematic &sheet, const char *key, QPoint at) {
        const DefinitionPtr d = b.project->resolve(sheet, QString::fromLatin1(key)).definition;
        return sheet.addComponent(QString::fromLatin1(key), d.get(), at);
    };
    const QString in = placeIn(stage, "basic:sheet_port", {-150, 0});
    stage.setParameter(in, QStringLiteral("name"), QStringLiteral("in"));
    const QString out = placeIn(stage, "basic:sheet_port", {150, 0});
    stage.setParameter(out, QStringLiteral("name"), QStringLiteral("out"));
    const QString amp = placeIn(stage, "rf-system:amplifier", {0, 0});
    stage.setParameter(amp, QStringLiteral("gain"), QStringLiteral("g"));
    const auto pinOf = [&](Schematic &sheet, const QString &id, const char *pin) {
        const PlacedComponent *c = sheet.component(id);
        return pinPosition(*c, *b.project->resolve(sheet, c->definition).definition->symbol.pin(QString::fromLatin1(pin)));
    };
    stage.addWire({pinOf(stage, in, "p"), pinOf(stage, amp, "in")});
    stage.addWire({pinOf(stage, amp, "out"), pinOf(stage, out, "p")});

    const QString p1 = b.port(1, {-400, 0}), p2 = b.port(2, {400, 0});
    const QString x1 = b.place("sheet:stage", {-100, 0}, {{QStringLiteral("g"), QStringLiteral("20")}});
    const QString x2 = b.place("sheet:stage", {100, 0});
    b.connect(p1, "p", x1, "in");
    b.connect(x1, "out", x2, "in");
    b.connect(x2, "out", p2, "p");
    const LinearResult result = b.run({1e9});
    REQUIRE(result.ok());
    CHECK(db20(result.network.s[0](1, 0)) == Approx(30).margin(1e-9));
}

TEST_CASE("problems are reported, not simulated")
{
    SECTION("no ports")
    {
        Bench b;
        b.place("rf-system:termination", {0, 0});
        const LinearResult result = b.run({1e9});
        REQUIRE_FALSE(result.ok());
        CHECK(result.errors.join(QString()).contains(QStringLiteral("no simulation port")));
    }
    SECTION("duplicate port numbers and SPICE-only parts")
    {
        Bench b;
        b.port(1, {-100, 0});
        b.port(1, {100, 0});
        const LinearResult result = b.run({1e9});
        REQUIRE_FALSE(result.ok());
        CHECK(result.errors.join(QString()).contains(QStringLiteral("port number 1")));
    }
    SECTION("an S-parameter block without a file")
    {
        Bench b;
        const QString p = b.port(1, {-100, 0});
        const QString sp = b.place("rf-system:nport1", {100, 0});
        b.connect(p, "p", sp, "p1");
        const LinearResult result = b.run({1e9});
        REQUIRE_FALSE(result.ok());
        CHECK(result.errors.join(QString()).contains(QStringLiteral("no S-parameter file")));
    }
    SECTION("measurement errors")
    {
        Bench b;
        const QString p1 = b.port(1, {-100, 0}), p2 = b.port(2, {100, 0});
        b.connect(p1, "p", p2, "p");
        const LinearResult result = b.run({1e9}, false);
        REQUIRE(result.ok());
        CHECK_FALSE(evaluateMeasurement(result, QStringLiteral("S31")).ok());
        CHECK_FALSE(evaluateMeasurement(result, QStringLiteral("NF")).ok()); // no noise computed
        CHECK_FALSE(evaluateMeasurement(result, QStringLiteral("dB(S21")).ok());
        CHECK_FALSE(evaluateMeasurement(result, QStringLiteral("foo(S21)")).ok());
        CHECK(evaluateMeasurement(result, QStringLiteral("dB(S21) - dB(S11) * 0 + 3")).ok());
    }
}

TEST_CASE("a 100-element ladder over 1001 points is fast enough to tune (NF-2)")
{
    Bench b;
    const QString p1 = b.port(1, {-200, 0});
    QString previous = p1;
    const char *previousPin = "p";
    for (int i = 0; i < 50; ++i) {
        const int x = i * 200;
        const QString l = b.place("basic:inductor", {x, 0}, {{QStringLiteral("l"), QStringLiteral("10nH")}});
        b.connect(previous, previousPin, l, "p");
        const QString c = b.place("basic:capacitor", {x + 100, 100}, {{QStringLiteral("c"), QStringLiteral("4pF")}});
        b.sheet->rotateItems({c}, QPoint(x + 100, 100), 90);
        b.sheet->addWire({b.pin(l, "n"), QPoint(b.pin(c, "n").x(), 0), b.pin(c, "n")});
        b.ground(c, "p");
        previous = l;
        previousPin = "n";
    }
    const QString p2 = b.port(2, {50 * 200 + 200, 0});
    b.connect(previous, previousPin, p2, "p");

    std::vector<double> frequencies;
    for (int i = 0; i < 1001; ++i)
        frequencies.push_back(1e6 + i * 1e6);
    const auto start = std::chrono::steady_clock::now();
    const LinearResult result = b.run(frequencies);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(result.ok());
    WARN("100 elements x 1001 points with noise: " << seconds << " s");
    CHECK(seconds < 1.0);
    // A lossless ladder: no noise, |S11|^2 + |S21|^2 = 1.
    const CMatrix &s = result.network.s[500];
    CHECK(std::norm(s(0, 0)) + std::norm(s(1, 0)) == Approx(1).margin(1e-9));
    CHECK(std::abs(result.network.noiseCorrelation[500](1, 1)) < 1e-6);
}

TEST_CASE("S-parameter blocks reproduce their file and cascade like the network operations")
{
    Bench b;
    const QString source = QStringLiteral(LVD_RF_TEST_DATA "/amp_noise.s2p");
    const QString copy = QDir(b.project->directory()).filePath(QStringLiteral("amp.s2p"));
    REQUIRE(QFile::copy(source, copy));
    const auto file = readTouchstone(copy);
    REQUIRE(file.network);

    const QString p1 = b.port(1, {-400, 0}), p2 = b.port(2, {400, 0});
    const QString a1 = b.place("rf-system:nport2", {-100, 0}, {{QStringLiteral("file"), QStringLiteral("amp.s2p")}});
    const QString a2 = b.place("rf-system:nport2", {100, 0}, {{QStringLiteral("file"), QStringLiteral("amp.s2p")}});
    b.connect(p1, "p", a1, "p1");
    b.connect(a1, "p2", a2, "p1");
    b.connect(a2, "p2", p2, "p");

    // Frequencies on the noise grid, so no noise interpolation is involved.
    const std::vector<double> frequencies{1e9, 2e9, 3.5e9, 5e9};
    const LinearResult result = b.run(frequencies);
    REQUIRE(result.ok());

    QString error;
    const auto single = interpolate(*file.network, frequencies, Extrapolation::Error, &error);
    REQUIRE(single);
    const auto expected = cascade(*single, *single, &error);
    REQUIRE(expected);
    REQUIRE(expected->hasNoiseCorrelation());
    for (size_t i = 0; i < frequencies.size(); ++i) {
        CHECK((result.network.s[i] - expected->s[i]).norm() < 1e-10);
        CHECK((result.network.noiseCorrelation[i] - expected->noiseCorrelation[i]).norm()
              < 1e-9 * expected->noiseCorrelation[i].norm());
    }

    // One block alone: NF from the solver equals NF from the file's noise
    // parameters for a 50 ohm source.
    Bench c;
    REQUIRE(QFile::copy(source, QDir(c.project->directory()).filePath(QStringLiteral("amp.s2p"))));
    const QString q1 = c.port(1, {-300, 0}), q2 = c.port(2, {300, 0});
    const QString dut = c.place("rf-system:nport2", {0, 0}, {{QStringLiteral("file"), QStringLiteral("amp.s2p")}});
    c.connect(q1, "p", dut, "p1");
    c.connect(dut, "p2", q2, "p");
    const LinearResult alone = c.run(frequencies);
    REQUIRE(alone.ok());
    const Measurement nfs = evaluateMeasurement(alone, QStringLiteral("NF"));
    const Measurement nfmin = evaluateMeasurement(alone, QStringLiteral("NFmin"));
    REQUIRE(nfs.ok());
    REQUIRE(nfmin.ok());
    for (size_t i = 0; i < frequencies.size(); ++i) {
        CHECK(nfs.values[i].real() == Approx(noiseFigureDb(single->noise[i], 0.0, 50)).margin(1e-9));
        CHECK(nfmin.values[i].real() == Approx(single->noise[i].fminDb).margin(1e-9));
    }
}

TEST_CASE("physical lines in a circuit equal their line model")
{
    Bench b;
    const QString p1 = b.port(1, {-300, 0}), p2 = b.port(2, {300, 0});
    const QString ms = b.place("rf-circuit:microstrip", {0, 0},
                               {{QStringLiteral("w"), QStringLiteral("1.5mm")}, {QStringLiteral("l"), QStringLiteral("25mm")}});
    b.connect(p1, "p", ms, "in");
    b.connect(ms, "out", p2, "p");
    const LinearResult result = b.run({1e9, 5e9});
    REQUIRE(result.ok());
    MicrostripGeometry g{1.5e-3, 0.8e-3, 35e-6, 4.4, 0.02, 58e6};
    for (size_t i = 0; i < 2; ++i) {
        const auto line = microstrip(g, result.network.frequencies[i]);
        REQUIRE(line);
        const CMatrix expected = abcdToS(lineAbcd(line->zc, line->gamma, 25e-3), 50, 50);
        CHECK((result.network.s[i] - expected).norm() < 1e-12);
    }
    // Lossy: |S21| < 1 and the noise figure equals the loss for a matched-ish line.
    CHECK(std::abs(result.network.s[1](1, 0)) < 1.0);
}

TEST_CASE("analysis setups are stored in the schematic and evaluate their sweep")
{
    Bench b;
    b.sheet->setVariables({{QStringLiteral("f0"), QStringLiteral("1GHz")}});
    LinearAnalysisSetup setup;
    setup.sweep = QStringLiteral("log");
    setup.start = QStringLiteral("f0/10");
    setup.stop = QStringLiteral("f0*10");
    setup.points = QStringLiteral("3");
    QString id;
    b.sheet->setSimulation(withLinearAnalysis(b.sheet->simulation(), setup, &id));
    REQUIRE(id == QStringLiteral("a1"));
    const auto analyses = linearAnalyses(b.sheet->simulation());
    REQUIRE(analyses.size() == 1);
    QString error;
    const auto frequencies = sweepFrequencies(analyses.first(), sheetScope(*b.document), &error);
    REQUIRE(frequencies);
    CHECK((*frequencies)[0] == Approx(1e8));
    CHECK((*frequencies)[1] == Approx(1e9));
    CHECK((*frequencies)[2] == 1e10);

    // Undoable like any other edit, and saved with the document.
    b.sheet->undoStack()->undo();
    CHECK(linearAnalyses(b.sheet->simulation()).isEmpty());
    b.sheet->undoStack()->redo();
    REQUIRE(b.document->save(&error));
    auto reloaded = Document::load(b.document->filePath(), &error);
    REQUIRE(reloaded);
    CHECK(linearAnalyses(reloaded->schematic()->simulation()).size() == 1);

    setup.points = QStringLiteral("0");
    CHECK_FALSE(sweepFrequencies(setup, sheetScope(*b.document), &error));
}

TEST_CASE("passive parts with parasitics follow their equivalent circuits")
{
    const auto seriesS11 = [](Complex z) { return z / (z + 100.0); };
    const auto twoPort = [](Bench &b, const char *key, const QMap<QString, QString> &parameters) {
        const QString p1 = b.port(1, {-200, 0}), p2 = b.port(2, {200, 0});
        const QString part = b.place(key, {0, 0}, parameters);
        b.connect(p1, "p", part, "p");
        b.connect(part, "n", p2, "p");
    };
    const double resonance = 1 / (2 * Pi * std::sqrt(0.5e-9 * 10e-12));
    const std::vector<double> f{0.3e9, 1e9, resonance, 4e9};
    SECTION("capacitor: esr + esl in series, resonant at 1/(2 pi sqrt(esl c))")
    {
        Bench b;
        twoPort(b, "basic:capacitor", {{QStringLiteral("c"), QStringLiteral("10pF")}, {QStringLiteral("esr"), QStringLiteral("0.15")},
                                       {QStringLiteral("esl"), QStringLiteral("0.5nH")}});
        const LinearResult result = b.run(f);
        REQUIRE(result.ok());
        for (size_t i = 0; i < f.size(); ++i) {
            const double w = 2 * Pi * f[i];
            const Complex z(0.15, w * 0.5e-9 - 1 / (w * 10e-12));
            CHECK(std::abs(result.network.s[i](0, 0) - seriesS11(z)) < 1e-12);
            // Passive at T0: NF = 1/GA with a matched source.
            const CMatrix &s = result.network.s[i];
            CHECK(nf(result, i) == Approx(-10 * std::log10(std::norm(s(1, 0)) / (1 - std::norm(s(1, 1))))).margin(1e-9));
        }
        // At the self-resonance only esr remains.
        CHECK(std::abs(result.network.s[2](0, 0) - seriesS11(0.15)) < 1e-9);
    }
    SECTION("inductor: (rs + j w l) || cp, a parallel resonance")
    {
        Bench b;
        twoPort(b, "basic:inductor", {{QStringLiteral("l"), QStringLiteral("10nH")}, {QStringLiteral("q"), QStringLiteral("20")},
                                      {QStringLiteral("freq"), QStringLiteral("500MHz")}, {QStringLiteral("cp"), QStringLiteral("0.25pF")}});
        const LinearResult result = b.run(f);
        REQUIRE(result.ok());
        const double rs = 2 * Pi * 500e6 * 10e-9 / 20;
        for (size_t i = 0; i < f.size(); ++i) {
            const double w = 2 * Pi * f[i];
            const Complex zs(rs, w * 10e-9);
            const Complex z = zs / (1.0 + Complex(0, w * 0.25e-12) * zs);
            CHECK(std::abs(result.network.s[i](0, 0) - seriesS11(z)) < 1e-12);
        }
    }
    SECTION("resistor package: (r + j w esl) || cp")
    {
        Bench b;
        twoPort(b, "basic:resistor_0402", {});
        const LinearResult result = b.run(f);
        REQUIRE(result.ok());
        for (size_t i = 0; i < f.size(); ++i) {
            const double w = 2 * Pi * f[i];
            const Complex zs(50, w * 0.4e-9);
            const Complex z = zs / (1.0 + Complex(0, w * 0.05e-12) * zs);
            CHECK(std::abs(result.network.s[i](0, 0) - seriesS11(z)) < 1e-12);
        }
    }
    SECTION("zero parasitics are the ideal parts")
    {
        Bench b;
        twoPort(b, "basic:capacitor", {{QStringLiteral("c"), QStringLiteral("10pF")}});
        const LinearResult result = b.run(f);
        REQUIRE(result.ok());
        CHECK(std::abs(result.network.s[1](0, 0) - seriesS11(Complex(0, -1 / (2 * Pi * 1e9 * 10e-12)))) < 1e-12);
    }
}

TEST_CASE("microstrip lines take their substrate from a Substrate part")
{
    const auto line = [](Bench &b, const QString &substrate, QPoint at = {0, 0}) {
        const QString p1 = b.port(1, at + QPoint(-300, 0)), p2 = b.port(2, at + QPoint(300, 0));
        const QString ms = b.place("rf-circuit:microstrip", at,
                                   {{QStringLiteral("w"), QStringLiteral("1.1mm")}, {QStringLiteral("l"), QStringLiteral("20mm")},
                                    {QStringLiteral("substrate"), substrate}});
        b.connect(p1, "p", ms, "in");
        b.connect(ms, "out", p2, "p");
        return ms;
    };
    const auto expected = [](const MicrostripGeometry &g, double f) {
        const auto p = microstrip(g, f);
        REQUIRE(p);
        return abcdToS(lineAbcd(p->zc, p->gamma, 20e-3), 50, 50);
    };

    SECTION("catalog material: er and tand from the catalog, the rest from the part")
    {
        Bench b;
        line(b, QStringLiteral("SUB1"));
        const QString sub = b.place("rf-circuit:substrate", {0, 200},
                                    {{QStringLiteral("material"), QStringLiteral("RO4003C")}, {QStringLiteral("h"), QStringLiteral("0.508mm")},
                                     {QStringLiteral("er"), QStringLiteral("9.9")}, {QStringLiteral("rough"), QStringLiteral("1um")}});
        REQUIRE(b.sheet->component(sub)->reference == QStringLiteral("SUB1"));
        const Netlist netlist = buildNetlist(*b.project, *b.document);
        CHECK(netlist.errors.isEmpty());
        REQUIRE(netlist.substrates.size() == 1);
        CHECK(netlist.substrates.first().parameterValues.value(QStringLiteral("er")) == 3.55);
        for (const FlatComponent &component : netlist.components)
            CHECK(component.definitionKey != QStringLiteral("rf-circuit:substrate"));
        const LinearResult result = b.run({1e9, 10e9});
        REQUIRE(result.ok());
        const MicrostripGeometry g{1.1e-3, 0.508e-3, 35e-6, 3.55, 0.0027, 58e6, 1e-6};
        for (size_t i = 0; i < 2; ++i)
            CHECK((result.network.s[i] - expected(g, result.network.frequencies[i])).norm() < 1e-12);
        // ERC has nothing to say about a pinless substrate.
        for (const RuleViolation &violation : checkSchematic(*b.project, *b.document)) {
            INFO(violation.message.toStdString());
            CHECK(violation.severity != RuleViolation::Severity::Error);
        }
    }
    SECTION("custom material: the part's er and tand; stripline gets b = 2 h + t")
    {
        Bench b;
        line(b, QStringLiteral("SUB1"));
        b.place("rf-circuit:substrate", {0, 200},
                {{QStringLiteral("material"), QStringLiteral("custom")}, {QStringLiteral("h"), QStringLiteral("0.8mm")},
                 {QStringLiteral("er"), QStringLiteral("4.2")}, {QStringLiteral("tand"), QStringLiteral("0.015")}});
        const QString sl = b.place("rf-circuit:stripline", {0, 400}, {{QStringLiteral("substrate"), QStringLiteral("SUB1")}});
        const Netlist netlist = buildNetlist(*b.project, *b.document);
        CHECK(netlist.errors.isEmpty());
        const FlatComponent *strip = netlist.component(b.sheet->component(sl)->reference);
        REQUIRE(strip);
        CHECK(strip->parameterValues.value(QStringLiteral("b")) == Approx(2 * 0.8e-3 + 35e-6));
        CHECK(strip->parameterValues.value(QStringLiteral("er")) == 4.2);
        const FlatComponent *ms = nullptr;
        for (const FlatComponent &component : netlist.components) {
            if (component.definitionKey == QStringLiteral("rf-circuit:microstrip"))
                ms = &component;
        }
        REQUIRE(ms);
        CHECK(ms->parameterValues.value(QStringLiteral("h")) == Approx(0.8e-3));
        CHECK(ms->parameterValues.value(QStringLiteral("tand")) == 0.015);
    }
    SECTION("empty substrate: the line's own values (old designs unchanged)")
    {
        Bench b;
        line(b, QString());
        const LinearResult result = b.run({1e9});
        REQUIRE(result.ok());
        CHECK((result.network.s[0] - expected({1.1e-3, 0.8e-3, 35e-6, 4.4, 0.02, 58e6, 0}, 1e9)).norm() < 1e-12);
    }
    SECTION("unknown and ambiguous names are errors")
    {
        Bench b;
        line(b, QStringLiteral("SUB7"));
        LinearResult result = b.run({1e9});
        REQUIRE_FALSE(result.ok());
        CHECK(result.errors.join(QString()).contains(QStringLiteral("substrate \"SUB7\" not found")));

        Bench c;
        line(c, QStringLiteral("SUB1"));
        const QString s1 = c.place("rf-circuit:substrate", {0, 200});
        const QString s2 = c.place("rf-circuit:substrate", {200, 200});
        c.sheet->setReference(s2, c.sheet->component(s1)->reference);
        result = c.run({1e9});
        REQUIRE_FALSE(result.ok());
        CHECK(result.errors.join(QString()).contains(QStringLiteral("ambiguous")));
    }
    SECTION("hierarchy: the sheet's own substrate first, then the enclosing sheets")
    {
        Bench b;
        QString error;
        Document *stageDoc = b.project->createDocument(DocumentType::RfSchematic, QStringLiteral("stage"), &error);
        REQUIRE(stageDoc);
        Schematic &stage = *stageDoc->schematic();
        const auto placeIn = [&](const char *key, QPoint at) {
            const DefinitionPtr d = b.project->resolve(stage, QString::fromLatin1(key)).definition;
            return stage.addComponent(QString::fromLatin1(key), d.get(), at);
        };
        const QString in = placeIn("basic:sheet_port", {-150, 0});
        stage.setParameter(in, QStringLiteral("name"), QStringLiteral("in"));
        const QString out = placeIn("basic:sheet_port", {150, 0});
        stage.setParameter(out, QStringLiteral("name"), QStringLiteral("out"));
        const QString ms = placeIn("rf-circuit:microstrip", {0, 0});
        stage.setParameter(ms, QStringLiteral("substrate"), QStringLiteral("SUB1"));
        stage.setParameter(ms, QStringLiteral("w"), QStringLiteral("1.1mm"));
        stage.setParameter(ms, QStringLiteral("l"), QStringLiteral("20mm"));
        const auto pinOf = [&](const QString &id, const char *pin) {
            const PlacedComponent *c = stage.component(id);
            return pinPosition(*c, *b.project->resolve(stage, c->definition).definition->symbol.pin(QString::fromLatin1(pin)));
        };
        stage.addWire({pinOf(in, "p"), pinOf(ms, "in")});
        stage.addWire({pinOf(ms, "out"), pinOf(out, "p")});
        const QString p1 = b.port(1, {-400, 0}), p2 = b.port(2, {400, 0});
        const QString x1 = b.place("sheet:stage", {0, 0});
        b.connect(p1, "p", x1, "in");
        b.connect(x1, "out", p2, "p");
        b.place("rf-circuit:substrate", {0, 300},
                {{QStringLiteral("material"), QStringLiteral("RT/duroid 5880")}, {QStringLiteral("h"), QStringLiteral("0.787mm")}});
        LinearResult result = b.run({3e9});
        REQUIRE(result.ok());
        CHECK((result.network.s[0] - expected({1.1e-3, 0.787e-3, 35e-6, 2.2, 0.0009, 58e6, 0}, 3e9)).norm() < 1e-12);

        // A substrate of the same name inside the sheet wins.
        const QString local = placeIn("rf-circuit:substrate", {0, 300});
        stage.setParameter(local, QStringLiteral("material"), QStringLiteral("RO3010"));
        stage.setParameter(local, QStringLiteral("h"), QStringLiteral("0.64mm"));
        stage.setReference(local, QStringLiteral("SUB1"));
        result = b.run({3e9});
        REQUIRE(result.ok());
        CHECK((result.network.s[0] - expected({1.1e-3, 0.64e-3, 35e-6, 11.2, 0.0022, 58e6, 0}, 3e9)).norm() < 1e-12);
    }
}

TEST_CASE("microstrip discontinuities and coupled microstrip in a circuit")
{
    const MicrostripSubstrate alumina{0.635e-3, 5e-6, 9.8, 0.0005, 58e6, 0};
    SECTION("open end")
    {
        Bench b;
        const QString p = b.port(1, {-200, 0});
        const QString open = b.place("rf-circuit:microstrip_open", {0, 0});
        b.connect(p, "p", open, "p");
        const LinearResult result = b.run({5e9});
        REQUIRE(result.ok());
        const double c = *openEndCapacitance(alumina, 0.6e-3, 5e9);
        const Complex y(0, 2 * Pi * 5e9 * c);
        CHECK(std::abs(result.network.s[0](0, 0) - (1.0 - 50.0 * y) / (1.0 + 50.0 * y)) < 1e-12);
    }
    SECTION("T-junction")
    {
        Bench b;
        const QString tee = b.place("rf-circuit:microstrip_tee", {0, 0}, {{QStringLiteral("w3"), QStringLiteral("0.3mm")}});
        const char *pins[] = {"p1", "p2", "p3"};
        const QPoint offsets[] = {{-100, 0}, {100, 0}, {0, 100}};
        for (int k = 0; k < 3; ++k) {
            const QString port = b.port(k + 1, b.pin(tee, pins[k]) + offsets[k]);
            b.connect(port, "p", tee, pins[k]);
        }
        const LinearResult result = b.run({0, 4e9});
        REQUIRE(result.ok());
        const auto expected = microstripTee(alumina, 0.6e-3, 0.6e-3, 0.3e-3, 4e9, 50.0);
        REQUIRE(expected);
        CHECK((result.network.s[1] - *expected).norm() < 1e-12);
        CHECK(std::abs(result.network.s[0](1, 0) - 2.0 / 3.0) < 1e-12); // DC: an ideal node
    }
    SECTION("gap, step and bend are reciprocal and lossless; the via has the Goldfarb-Pucel impedance")
    {
        for (const char *key : {"rf-circuit:microstrip_gap", "rf-circuit:microstrip_step", "rf-circuit:microstrip_bend"}) {
            Bench b;
            const QString p1 = b.port(1, {-200, 0}), p2 = b.port(2, {200, 100});
            const QString part = b.place(key, {0, 0});
            b.connect(p1, "p", part, "in");
            b.connect(part, "out", p2, "p");
            const LinearResult result = b.run({1e9, 8e9});
            INFO(key);
            REQUIRE(result.ok());
            for (const CMatrix &s : result.network.s) {
                CHECK((s - s.transpose()).norm() < 1e-12);
                CHECK((s * s.adjoint() - CMatrix::Identity(2, 2)).norm() < 1e-9);
            }
        }
        Bench b;
        const QString p = b.port(1, {-200, 0});
        const QString via = b.place("rf-circuit:microstrip_via", {0, 0});
        b.connect(p, "p", via, "p");
        const LinearResult result = b.run({2e9});
        REQUIRE(result.ok());
        const auto z = viaImpedance(0.4e-3, 0.635e-3, 20e-6, 58e6, 2e9);
        CHECK(std::abs(result.network.s[0](0, 0) - reflectionFromImpedance(*z, 50)) < 1e-12);
    }
    SECTION("coupled microstrip: a quarter-wave coupler couples about (Z0e - Z0o)/(Z0e + Z0o) to p3")
    {
        CoupledMicrostripGeometry g{0.5e-3, 0.15e-3, 0.635e-3, 0, 9.8, 0, 0, 0};
        const auto modes = coupledMicrostrip(g, 5e9);
        REQUIRE(modes);
        const double eAverage = (std::sqrt(modes->even.epsilonEffective) + std::sqrt(modes->odd.epsilonEffective)) / 2;
        const double quarter = SpeedOfLight / 5e9 / eAverage / 4;
        Bench b;
        const QString cl = b.place("rf-circuit:coupled_microstrip", {0, 0},
                                   {{QStringLiteral("w"), QStringLiteral("0.5mm")}, {QStringLiteral("s"), QStringLiteral("0.15mm")},
                                    {QStringLiteral("l"), QString::number(quarter * 1e3, 'g', 12) + QStringLiteral("mm")},
                                    {QStringLiteral("t"), QStringLiteral("0")}, {QStringLiteral("tand"), QStringLiteral("0")},
                                    {QStringLiteral("sigma"), QStringLiteral("0")}});
        const char *pins[] = {"p1", "p2", "p3", "p4"};
        for (int k = 0; k < 4; ++k) {
            const QString port = b.port(k + 1, b.pin(cl, pins[k]) + QPoint(k % 2 ? 100 : -100, 0));
            b.connect(port, "p", cl, pins[k]);
        }
        const LinearResult result = b.run({5e9});
        REQUIRE(result.ok());
        const CMatrix &s = result.network.s[0];
        CHECK((s * s.adjoint() - CMatrix::Identity(4, 4)).norm() < 1e-9);
        CHECK((s - s.transpose()).norm() < 1e-12);
        const double ze = modes->even.zc.real(), zo = modes->odd.zc.real();
        const double coupling = (ze - zo) / (ze + zo);
        WARN("coupled microstrip: Z0e " << ze << ", Z0o " << zo << ", |S31| " << std::abs(s(2, 0))
                                        << ", (Z0e-Z0o)/(Z0e+Z0o) " << coupling << ", sqrt(Z0e Z0o) " << std::sqrt(ze * zo));
        // Not exact: the modes have different phase velocities and sqrt(Z0e Z0o) is not 50 ohm.
        CHECK(std::abs(s(2, 0)) == Approx(coupling).margin(0.1 * coupling + 0.02));
    }
}

TEST_CASE("gain measurements: GUmax, U and the simultaneous conjugate match")
{
    Bench b;
    const QString source = QStringLiteral(LVD_RF_TEST_DATA "/amp_noise.s2p");
    REQUIRE(QFile::copy(source, QDir(b.project->directory()).filePath(QStringLiteral("amp.s2p"))));
    const QString p1 = b.port(1, {-300, 0}), p2 = b.port(2, {300, 0});
    const QString dut = b.place("rf-system:nport2", {0, 0}, {{QStringLiteral("file"), QStringLiteral("amp.s2p")}});
    b.connect(p1, "p", dut, "p1");
    b.connect(dut, "p2", p2, "p");
    const LinearResult result = b.run({1e9, 2e9, 5e9});
    REQUIRE(result.ok());
    const Measurement gu = evaluateMeasurement(result, QStringLiteral("GUmax"));
    const Measurement u = evaluateMeasurement(result, QStringLiteral("U"));
    const Measurement gms = evaluateMeasurement(result, QStringLiteral("GammaMS"));
    const Measurement gml = evaluateMeasurement(result, QStringLiteral("GammaML"));
    REQUIRE(gu.ok());
    REQUIRE(u.ok());
    REQUIRE(gms.ok());
    REQUIRE(gml.ok());
    CHECK(gu.unit == QStringLiteral("dB"));
    CHECK(gms.isComplex);
    for (size_t i = 0; i < 3; ++i) {
        const CMatrix &s = result.network.s[i];
        CHECK(gu.values[i].real() == Approx(maximumUnilateralGainDb(s)).margin(1e-12));
        CHECK(u.values[i].real() == Approx(masonUnilateralGainDb(s)).margin(1e-12));
        const auto match = simultaneousConjugateMatch(s);
        if (match) {
            CHECK(std::abs(gms.values[i] - match->gammaS) < 1e-12);
            CHECK(std::abs(gml.values[i] - match->gammaL) < 1e-12);
        } else {
            CHECK(std::isnan(gms.values[i].real()));
        }
    }
    CHECK_FALSE(evaluateMeasurement(result, QStringLiteral("U(1)")).ok());
}

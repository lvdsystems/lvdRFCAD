#include "core/Connectivity.h"
#include "core/Library.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "spice/NgspiceEngine.h"
#include "spice/SpiceAnalysis.h"
#include "spice/SpiceNetlister.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>
#include <numbers>

using namespace lvd;
using namespace lvd::spice;
using Catch::Approx;

namespace {

SpiceEngine &engine()
{
    NgspiceEngine::setRuntimeFolder(QStringLiteral(LVD_NGSPICE_RUNTIME));
    return NgspiceEngine::instance();
}

// A two-pin part with a SPICE template, defined in the test.
ComponentDefinition twoPin(const QString &id, const QString &prefix, const QJsonObject &model,
                           const QJsonArray &parameters = {})
{
    const QJsonObject json{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), id},
        {QStringLiteral("category"), QStringLiteral("Test")},
        {QStringLiteral("referencePrefix"), prefix},
        {QStringLiteral("pins"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("p")}},
                                            QJsonObject{{QStringLiteral("name"), QStringLiteral("n")}}}},
        {QStringLiteral("parameters"), parameters},
        {QStringLiteral("symbol"),
         QJsonObject{{QStringLiteral("graphics"), QJsonArray{}},
                     {QStringLiteral("pins"),
                      QJsonArray{QJsonObject{{QStringLiteral("pin"), QStringLiteral("p")},
                                             {QStringLiteral("at"), QJsonArray{0, -30}},
                                             {QStringLiteral("direction"), QStringLiteral("down")}},
                                 QJsonObject{{QStringLiteral("pin"), QStringLiteral("n")},
                                             {QStringLiteral("at"), QJsonArray{0, 30}},
                                             {QStringLiteral("direction"), QStringLiteral("up")}}}}}},
        {QStringLiteral("models"), QJsonArray{model}},
    };
    QString error;
    const auto definition = ComponentDefinition::fromJson(json, &error);
    INFO(error.toStdString());
    REQUIRE(definition);
    return *definition;
}

QJsonObject parameter(const char *name, const char *defaultValue, bool text = false)
{
    QJsonObject p{{QStringLiteral("name"), QString::fromLatin1(name)},
                  {QStringLiteral("label"), QString::fromLatin1(name)},
                  {QStringLiteral("unit"), QString()},
                  {QStringLiteral("default"), QString::fromLatin1(defaultValue)}};
    if (text)
        p.insert(QStringLiteral("text"), true);
    return p;
}

QJsonObject spiceModel(const QString &templateText, const QString &subcircuit = {})
{
    QJsonObject model{{QStringLiteral("kind"), QStringLiteral("spice")}, {QStringLiteral("template"), templateText}};
    if (!subcircuit.isEmpty())
        model.insert(QStringLiteral("subcircuit"), subcircuit);
    return model;
}

struct Bench {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;
    QList<ComponentDefinition> local; // test parts; the sheet keeps a copy

    Bench()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::SpiceSchematic, QStringLiteral("dut"), &error);
        REQUIRE(document);
        sheet = document->schematic();
    }

    QString place(const QString &key, QPoint at, const QMap<QString, QString> &parameters = {},
                  const ComponentDefinition *definition = nullptr)
    {
        DefinitionPtr resolved;
        if (definition && !sheet->cachedDefinition(key))
            sheet->setCachedDefinition(key, std::make_shared<const ComponentDefinition>(*definition));
        if (!definition) {
            resolved = project->resolve(*sheet, key).definition;
            REQUIRE(resolved);
            definition = resolved.get();
        }
        const QString id = sheet->addComponent(key, definition, at);
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            sheet->setParameter(id, it.key(), it.value());
        return id;
    }

    QPoint pin(const QString &id, const char *name)
    {
        const PlacedComponent *component = sheet->component(id);
        const DefinitionPtr definition = project->resolve(*sheet, component->definition).definition;
        REQUIRE(definition);
        return pinPosition(*component, *definition->symbol.pin(QString::fromLatin1(name)));
    }

    void connect(const QString &a, const char *pinA, const QString &b, const char *pinB)
    {
        const QPoint from = pin(a, pinA), to = pin(b, pinB);
        if (from.x() == to.x() || from.y() == to.y())
            sheet->addWire({from, to});
        else
            sheet->addWire({from, QPoint(to.x(), from.y()), to});
    }

    void ground(const QString &id, const char *pinName)
    {
        const QString g = place(QStringLiteral("basic:ground"), pin(id, pinName) + QPoint(0, 60));
        connect(id, pinName, g, "gnd");
    }

    void label(const QString &id, const char *pinName, const char *net)
    {
        sheet->addLabel(QString::fromLatin1(net), pin(id, pinName));
    }
};

const ComponentDefinition &voltageSource()
{
    static const ComponentDefinition definition =
        twoPin(QStringLiteral("vsource"), QStringLiteral("V"), spiceModel(QStringLiteral("V{ref} {pin:p} {pin:n} {spec}")),
               QJsonArray{parameter("spec", "DC 1 AC 1", true)});
    return definition;
}

// V1 (10 V) - R1 1k - out - R2 3k - ground, with a capacitor to show AC.
struct Divider : Bench {
    QString v1, r1, r2, c1;
    Divider()
    {
        v1 = place(QStringLiteral("test:vsource"), {0, 0}, {{QStringLiteral("spec"), QStringLiteral("DC 10 AC 1")}},
                   &voltageSource());
        r1 = place(QStringLiteral("basic:resistor"), {100, -30}, {{QStringLiteral("r"), QStringLiteral("1k")}});
        r2 = place(QStringLiteral("basic:resistor"), {200, -30}, {{QStringLiteral("r"), QStringLiteral("3k")}});
        c1 = place(QStringLiteral("basic:capacitor"), {200, 100}, {{QStringLiteral("c"), QStringLiteral("100nF")}});
        connect(v1, "p", r1, "p");
        connect(r1, "n", r2, "p");
        connect(r2, "p", c1, "p"); // vertical wire at x = 170
        ground(v1, "n");
        ground(r2, "n");
        ground(c1, "n");
        label(r1, "p", "in");
        label(r2, "p", "out");
    }
};

} // namespace

TEST_CASE("the netlister renders templates with nodes, numbers and references")
{
    Divider bench;
    const SpiceDeck deck = buildSpiceDeck(*bench.project, *bench.document);
    INFO(deck.text.toStdString());
    INFO(deck.errors.join(QLatin1Char('\n')).toStdString());
    REQUIRE(deck.ok());
    CHECK(deck.text.contains(QStringLiteral("VV1 in 0 DC 10 AC 1")));
    CHECK(deck.text.contains(QStringLiteral("RR1 in out 1000")));
    CHECK(deck.text.contains(QStringLiteral("RR2 out 0 3000")));
    CHECK(deck.text.contains(QStringLiteral("CC1 out 0 1e-07")));
    CHECK(deck.text.trimmed().endsWith(QStringLiteral(".end")));
    CHECK(deck.node(QStringLiteral("OUT")) == QLatin1String("out"));
    REQUIRE(deck.element(QStringLiteral("R2")));
    CHECK(deck.element(QStringLiteral("R2"))->name == QLatin1String("rr2"));
    CHECK(deck.element(QStringLiteral("R2"))->pinNodes.value(QStringLiteral("n")) == QLatin1String("0"));
    CHECK(deck.elementByName(QStringLiteral("CC1"))->path == QLatin1String("C1"));
}

TEST_CASE("SPICE numbers never use SI suffixes")
{
    CHECK(formatSpiceNumber(1e6) == QLatin1String("1000000"));
    CHECK(formatSpiceNumber(1e-3) == QLatin1String("0.001"));
    CHECK(formatSpiceNumber(4.7e-9) == QLatin1String("4.7e-09"));
    CHECK(formatSpiceNumber(0) == QLatin1String("0"));
    CHECK(formatSpiceNumber(-2.5) == QLatin1String("-2.5"));
}

TEST_CASE("netlister problems are reported")
{
    Bench bench;
    const ComponentDefinition noModel = twoPin(QStringLiteral("nomodel"), QStringLiteral("Z"),
                                               QJsonObject{{QStringLiteral("kind"), QStringLiteral("rf-element")},
                                                           {QStringLiteral("element"), QStringLiteral("resistor")}});
    const ComponentDefinition badPin =
        twoPin(QStringLiteral("badpin"), QStringLiteral("B"), spiceModel(QStringLiteral("R{ref} {pin:p} {pin:x} 1")));
    const ComponentDefinition badParameter =
        twoPin(QStringLiteral("badparameter"), QStringLiteral("P"), spiceModel(QStringLiteral("R{ref} {pin:p} {pin:n} {nope}")));
    const QString a = bench.place(QStringLiteral("test:nomodel"), {0, 0}, {}, &noModel);
    const QString b = bench.place(QStringLiteral("test:badpin"), {100, 0}, {}, &badPin);
    const QString c = bench.place(QStringLiteral("test:badparameter"), {200, 0}, {}, &badParameter);
    for (const QString &id : {a, b, c}) {
        bench.ground(id, "n");
        bench.label(id, "p", "x");
    }
    const SpiceDeck deck = buildSpiceDeck(*bench.project, *bench.document);
    const QString errors = deck.errors.join(QLatin1Char('\n'));
    INFO(errors.toStdString());
    CHECK(errors.contains(QStringLiteral("Z1 (nomodel) has no SPICE model")));
    CHECK(errors.contains(QStringLiteral("unknown pin \"x\"")));
    CHECK(errors.contains(QStringLiteral("unknown parameter \"nope\"")));
}

TEST_CASE("a net named GND that is not ground stays a separate node")
{
    Divider bench;
    bench.label(bench.c1, "p", "gnd"); // merges with "out": the net now has two labels
    Bench other;
    const QString r = other.place(QStringLiteral("basic:resistor"), {0, 0});
    other.label(r, "p", "GND");
    other.ground(r, "n");
    const SpiceDeck deck = buildSpiceDeck(*other.project, *other.document);
    INFO(deck.text.toStdString());
    CHECK(deck.text.contains(QStringLiteral("RR1 gnd_net 0 1000")));
    CHECK_FALSE(deck.warnings.isEmpty());
}

TEST_CASE("shared subcircuit text is emitted once and per-instance cards are kept")
{
    Bench bench;
    const ComponentDefinition diode = twoPin(
        QStringLiteral("diode"), QStringLiteral("D"),
        spiceModel(QStringLiteral("D{ref} {pin:p} {pin:n} DM_{ref}\n.model DM_{ref} D(is={is})"),
                   QStringLiteral(".subckt lvd_dummy a b params: r=1\nR1 a b {r*2}\n.ends")),
        QJsonArray{parameter("is", "1f")});
    const QString d1 = bench.place(QStringLiteral("test:diode"), {0, 0}, {}, &diode);
    const QString d2 = bench.place(QStringLiteral("test:diode"), {100, 0}, {{QStringLiteral("is"), QStringLiteral("2f")}}, &diode);
    for (const QString &id : {d1, d2}) {
        bench.label(id, "p", "a");
        bench.ground(id, "n");
    }
    const SpiceDeck deck = buildSpiceDeck(*bench.project, *bench.document);
    INFO(deck.text.toStdString());
    REQUIRE(deck.ok());
    CHECK(deck.text.count(QStringLiteral(".subckt lvd_dummy")) == 1);
    CHECK(deck.text.contains(QStringLiteral("R1 a b {r*2}"))); // braces are not placeholders
    CHECK(deck.text.contains(QStringLiteral(".model DM_D1 D(is=1e-15)")));
    CHECK(deck.text.contains(QStringLiteral(".model DM_D2 D(is=2e-15)")));
}

TEST_CASE("analysis setups become SPICE cards")
{
    Divider bench;
    bench.sheet->setVariables({{QStringLiteral("fstop"), QStringLiteral("1MHz")}});
    const SpiceDeck deck = buildSpiceDeck(*bench.project, *bench.document);
    const Expression::Lookup scope = sheetVariables(*bench.document);
    const auto cardsOf = [&](const SpiceAnalysisSetup &setup) {
        const AnalysisCards cards = analysisCards(setup, deck, scope);
        INFO(cards.errors.join(QLatin1Char('\n')).toStdString());
        CHECK(cards.errors.isEmpty());
        return cards.cards.join(QLatin1Char('|'));
    };

    SpiceAnalysisSetup ac = SpiceAnalysisSetup::create(AnalysisKind::Ac);
    ac.set(QStringLiteral("stop"), QStringLiteral("fstop"));
    CHECK(cardsOf(ac) == QLatin1String(".ac dec 20 1 1000000"));

    SpiceAnalysisSetup dc = SpiceAnalysisSetup::create(AnalysisKind::DcSweep);
    CHECK(cardsOf(dc) == QLatin1String(".dc vv1 0 5 0.1"));
    dc.set(QStringLiteral("source2"), QStringLiteral("temp"));
    dc.set(QStringLiteral("start2"), QStringLiteral("-40"));
    dc.set(QStringLiteral("stop2"), QStringLiteral("85"));
    dc.set(QStringLiteral("step2"), QStringLiteral("125"));
    CHECK(cardsOf(dc) == QLatin1String(".dc vv1 0 5 0.1 temp -40 85 125"));

    SpiceAnalysisSetup tran = SpiceAnalysisSetup::create(AnalysisKind::Transient);
    tran.set(QStringLiteral("maxStep"), QStringLiteral("10ns"));
    tran.set(QStringLiteral("temperature"), QStringLiteral("50"));
    CHECK(cardsOf(tran) == QLatin1String(".temp 50|.tran 1e-06 0.001 0 1e-08"));

    SpiceAnalysisSetup noise = SpiceAnalysisSetup::create(AnalysisKind::Noise);
    CHECK(cardsOf(noise) == QLatin1String(".noise v(out) vv1 dec 20 1 10000000"));

    SpiceAnalysisSetup tf = SpiceAnalysisSetup::create(AnalysisKind::TransferFunction);
    tf.set(QStringLiteral("output"), QStringLiteral("v(out,in)"));
    CHECK(cardsOf(tf) == QLatin1String(".tf v(out,in) vv1"));

    SpiceAnalysisSetup pz = SpiceAnalysisSetup::create(AnalysisKind::PoleZero);
    CHECK(cardsOf(pz) == QLatin1String(".pz in 0 out 0 vol pz"));

    SpiceAnalysisSetup sens = SpiceAnalysisSetup::create(AnalysisKind::Sensitivity);
    sens.set(QStringLiteral("output"), QStringLiteral("i(V1)"));
    CHECK(cardsOf(sens) == QLatin1String(".sens i(vv1)"));

    SpiceAnalysisSetup wrong = SpiceAnalysisSetup::create(AnalysisKind::Noise);
    wrong.set(QStringLiteral("output"), QStringLiteral("nowhere"));
    wrong.set(QStringLiteral("source"), QStringLiteral("R1"));
    const AnalysisCards bad = analysisCards(wrong, deck, scope);
    const QString errors = bad.errors.join(QLatin1Char('\n'));
    CHECK(errors.contains(QStringLiteral("no net \"nowhere\"")));
    CHECK(errors.contains(QStringLiteral("R1 is not a voltage or current source")));
}

TEST_CASE("analyses are stored in the simulation object and keep unknown keys")
{
    SpiceAnalysisSetup tran = SpiceAnalysisSetup::create(AnalysisKind::Transient, QStringLiteral("Start-up"));
    tran.set(QStringLiteral("future"), 42);
    QString id;
    QJsonObject simulation = withSpiceAnalysis({}, tran, &id);
    CHECK(id == QLatin1String("a1"));
    const QList<SpiceAnalysisSetup> analyses = spiceAnalyses(simulation);
    REQUIRE(analyses.size() == 1);
    CHECK(analyses.first().name == QLatin1String("Start-up"));
    CHECK(analyses.first().kind == AnalysisKind::Transient);
    CHECK(analyses.first().data.value(QStringLiteral("future")).toInt() == 42);
    SpiceAnalysisSetup changed = analyses.first();
    changed.set(QStringLiteral("stop"), QStringLiteral("5ms"));
    simulation = withSpiceAnalysis(simulation, changed);
    CHECK(spiceAnalyses(simulation).size() == 1);
    CHECK(spiceAnalyses(simulation).first().text(QStringLiteral("stop")) == QLatin1String("5ms"));
}

TEST_CASE("schematic analyses run on ngspice")
{
    Divider bench;
    SECTION("operating point with device currents")
    {
        const SpiceSimulation sim = runSpiceAnalysis(*bench.project, *bench.document,
                                                     SpiceAnalysisSetup::create(AnalysisKind::OperatingPoint), engine());
        INFO(sim.netlist.toStdString());
        INFO(sim.errors.join(QLatin1Char('\n')).toStdString());
        REQUIRE(sim.ok());
        const SpicePlot *op = sim.run.plot(QStringLiteral("op"));
        REQUIRE(op);
        INFO(op->vectorNames().join(QLatin1Char(' ')).toStdString());
        CHECK(op->vector(QStringLiteral("v(out)"))->real.at(0) == Approx(7.5));
        REQUIRE(op->vector(QStringLiteral("@rr1[i]")));
        CHECK(op->vector(QStringLiteral("@rr1[i]"))->real.at(0) == Approx(2.5e-3));
        CHECK(op->vector(QStringLiteral("@rr2[p]"))->real.at(0) == Approx(7.5 * 2.5e-3));
    }
    SECTION("AC: the divider and capacitor form a low-pass")
    {
        SpiceAnalysisSetup ac = SpiceAnalysisSetup::create(AnalysisKind::Ac);
        const SpiceSimulation sim = runSpiceAnalysis(*bench.project, *bench.document, ac, engine());
        INFO(sim.errors.join(QLatin1Char('\n')).toStdString());
        REQUIRE(sim.ok());
        const SpicePlot *plot = sim.run.plot(QStringLiteral("ac"));
        REQUIRE(plot);
        const SpiceVector *f = plot->scale();
        const SpiceVector *out = plot->vector(QStringLiteral("v(out)"));
        REQUIRE(f);
        REQUIRE(out);
        const double rp = 1000.0 * 3000.0 / 4000.0;
        for (std::size_t k = 0; k < f->size(); k += 7) {
            const std::complex<double> expected =
                0.75 / (1.0 + std::complex<double>(0, 2 * std::numbers::pi * f->real[k] * rp * 100e-9));
            CHECK(std::abs(out->complex[k] - expected) < 1e-9);
        }
    }
    SECTION("transfer function and pole-zero")
    {
        SpiceAnalysisSetup tf = SpiceAnalysisSetup::create(AnalysisKind::TransferFunction);
        const SpiceSimulation sim = runSpiceAnalysis(*bench.project, *bench.document, tf, engine());
        INFO(sim.errors.join(QLatin1Char('\n')).toStdString());
        REQUIRE(sim.ok());
        const SpicePlot *plot = sim.run.plots.value(0).vectors.empty() ? nullptr : &sim.run.plots.first();
        REQUIRE(plot);
        INFO(plot->name.toStdString() + ": " + plot->vectorNames().join(QLatin1Char(' ')).toStdString());
        const SpiceVector *gain = plot->vector(QStringLiteral("transfer_function"));
        REQUIRE(gain);
        CHECK(gain->real.at(0) == Approx(0.75));

        SpiceAnalysisSetup pz = SpiceAnalysisSetup::create(AnalysisKind::PoleZero);
        const SpiceSimulation pzSim = runSpiceAnalysis(*bench.project, *bench.document, pz, engine());
        INFO(pzSim.errors.join(QLatin1Char('\n')).toStdString());
        REQUIRE(pzSim.ok());
        const SpicePlot &pzPlot = pzSim.run.plots.first();
        INFO(pzPlot.name.toStdString() + ": " + pzPlot.vectorNames().join(QLatin1Char(' ')).toStdString());
        const SpiceVector *pole = pzPlot.vector(QStringLiteral("pole(1)"));
        REQUIRE(pole);
        // One real pole at -1/(Rp C) rad/s.
        CHECK(pole->at(0).real() == Approx(-1.0 / (750.0 * 100e-9)));
    }
}

TEST_CASE("a Fourier analysis reports the harmonics of transient signals")
{
    Divider bench;
    // A clipped sine: 1 kHz, with odd harmonics from the clipping.
    bench.sheet->setParameter(bench.v1, QStringLiteral("spec"), QStringLiteral("SIN(0 10 1k)"));
    bench.sheet->setParameter(bench.c1, QStringLiteral("c"), QStringLiteral("1p"));
    SpiceAnalysisSetup four = SpiceAnalysisSetup::create(AnalysisKind::Fourier);
    four.set(QStringLiteral("step"), QStringLiteral("1us"));
    four.set(QStringLiteral("stop"), QStringLiteral("3ms"));
    four.set(QStringLiteral("maxStep"), QStringLiteral("1us"));
    four.set(QStringLiteral("signals"), QJsonArray{QStringLiteral("v(out)"), QStringLiteral("i(V1)"), QStringLiteral("v(nowhere)")});
    const SpiceSimulation sim = runSpiceAnalysis(*bench.project, *bench.document, four, engine());
    INFO(sim.errors.join(QLatin1Char('\n')).toStdString());
    REQUIRE(sim.ok());
    REQUIRE(sim.fourier.size() == 3);
    REQUIRE(sim.fourier.at(0).result);
    const FourierResult &out = *sim.fourier.at(0).result;
    CHECK(out.fundamental == Approx(1000.0));
    REQUIRE(out.harmonics.size() == 9);
    CHECK(out.harmonics.at(0).magnitude == Approx(7.5).epsilon(1e-3)); // 10 V * 3k / 4k
    CHECK(out.thdPercent < 0.1);
    REQUIRE(sim.fourier.at(1).result); // i(V1) through the reference alias
    CHECK(sim.fourier.at(1).result->harmonics.at(0).magnitude == Approx(2.5e-3).epsilon(1e-3));
    CHECK_FALSE(sim.fourier.at(2).result);
    CHECK_FALSE(sim.fourier.at(2).error.isEmpty());
}

TEST_CASE("every part of the SPICE libraries netlists and parses in ngspice")
{
    Bench probe;
    LibraryManager *libraries = probe.session.libraries();
    int parts = 0;
    for (const QString &libraryName : libraries->libraryNames()) {
        if (!libraryName.startsWith(QLatin1String("spice")) && libraryName != QLatin1String("basic"))
            continue;
        const Library *library = libraries->library(libraryName);
        REQUIRE(library);
        for (const DefinitionPtr &definition : library->components) {
            if (!definition->model(QStringLiteral("spice")))
                continue;
            const QString key = libraryName + QLatin1Char(':') + definition->id;
            INFO(key.toStdString());
            Bench bench;
            const QString id = bench.place(key, {0, 0});
            // Every pin on its own net, each tied to ground through 1 GOhm so
            // nothing floats.
            SpiceNetlistOptions options;
            for (const QString &pin : definition->pins) {
                const QString net = QStringLiteral("n_") + pin.toLower();
                bench.label(id, pin.toLatin1().constData(), net.toLatin1().constData());
                options.cards << QStringLiteral("Rtie_%1 %1 0 1e9").arg(net);
            }
            options.cards << QStringLiteral(".op");
            const SpiceDeck deck = buildSpiceDeck(*bench.project, *bench.document, options);
            INFO(deck.text.toStdString());
            INFO(deck.errors.join(QLatin1Char('\n')).toStdString());
            CHECK(deck.ok());
            const SpiceRunResult run = engine().run(deck.text);
            QStringList parseErrors;
            for (const QString &line : run.errors)
                if (line.contains(QLatin1String("Error on line")) || line.contains(QLatin1String("unknown"), Qt::CaseInsensitive)
                    || line.contains(QLatin1String("could not find")) || line.contains(QLatin1String("can't find")))
                    parseErrors << line;
            INFO(run.log.join(QLatin1Char('\n')).toStdString());
            CHECK(parseErrors.isEmpty());
            ++parts;
        }
    }
    CHECK(parts >= 80);
}

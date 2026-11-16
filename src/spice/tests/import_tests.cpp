#include "core/Connectivity.h"
#include "core/Library.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "spice/NgspiceEngine.h"
#include "spice/SpiceImport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>

using namespace lvd;
using namespace lvd::spice;
using Catch::Approx;

namespace {

SpiceEngine &engine()
{
    NgspiceEngine::setRuntimeFolder(QStringLiteral(LVD_NGSPICE_RUNTIME));
    return NgspiceEngine::instance();
}

const char *ModelText = R"(* Vendor models
.model QX2 NPN(IS=1e-14 BF=200
+ VAF=100) ; a comment
.MODEL DX D (IS=2n RS=0.5)  $ another comment
.lib fast
.model MFAST NMOS(LEVEL=1 VTO=0.7 KP=100u)
.endl
.subckt OPX INP INN OUT VCC VEE params: GAIN=1e5 FP={10}
E1 x 0 INP INN {GAIN}
.model inner D
R1 x OUT 1
.ends OPX
)";

} // namespace

TEST_CASE("logical lines join continuations and drop comments")
{
    const auto lines = logicalSpiceLines(QString::fromLatin1(ModelText));
    REQUIRE(lines.size() >= 3);
    CHECK(lines.at(0).first == 2);
    CHECK(lines.at(0).second == QLatin1String(".model QX2 NPN(IS=1e-14 BF=200 VAF=100)"));
    CHECK(lines.at(1).second == QLatin1String(".MODEL DX D (IS=2n RS=0.5)"));
}

TEST_CASE("models and subcircuits are found in a vendor file")
{
    const SpiceModelFile file = parseSpiceModels(QString::fromLatin1(ModelText));
    CHECK(file.errors.isEmpty());
    REQUIRE(file.entries.size() == 4); // the .model inside the subcircuit is not listed
    CHECK(file.entries.at(0).name == QLatin1String("QX2"));
    CHECK(file.entries.at(0).type == QLatin1String("NPN"));
    CHECK(file.entries.at(1).type == QLatin1String("D"));
    CHECK(file.entries.at(2).section == QLatin1String("fast"));
    const SpiceModelEntry &opamp = file.entries.at(3);
    CHECK(opamp.kind == SpiceModelEntry::Kind::Subcircuit);
    CHECK(opamp.pins == QStringList{QStringLiteral("INP"), QStringLiteral("INN"), QStringLiteral("OUT"), QStringLiteral("VCC"),
                                    QStringLiteral("VEE")});
    CHECK(opamp.parameters.value(QStringLiteral("GAIN")) == QLatin1String("1e5"));
    CHECK(opamp.parameters.value(QStringLiteral("FP")) == QLatin1String("{10}"));
    CHECK(file.sections == QStringList{QStringLiteral("fast")});

    CHECK_FALSE(parseSpiceModels(QStringLiteral("$CDNENCSTART\nabc\n")).errors.isEmpty());
    CHECK_FALSE(parseSpiceModels(QStringLiteral(".subckt A 1 2\nR1 1 2 1\n")).errors.isEmpty());
}

TEST_CASE("proposed components are valid and simulate")
{
    QTemporaryDir dir;
    const QString modelPath = dir.filePath(QStringLiteral("vendor.lib"));
    {
        QFile file(modelPath);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(ModelText);
    }
    const SpiceModelFile parsed = *readSpiceModelFile(modelPath);

    Session session;
    QString error;
    Project *project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
    REQUIRE(project);
    QMap<QString, ComponentDefinition> templates;
    if (DefinitionPtr npn = session.libraries()->find(QStringLiteral("spice-semiconductors:npn")))
        templates.insert(QStringLiteral("NPN"), *npn);
    REQUIRE(templates.contains(QStringLiteral("NPN")));

    const ComponentDefinition transistor = proposeComponent(parsed.entries.at(0), modelPath, templates);
    INFO(transistor.validate().join(QLatin1Char('\n')).toStdString());
    CHECK(transistor.validate().isEmpty());
    CHECK(transistor.pins == QStringList{QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("e")});
    CHECK(transistor.symbol == templates.value(QStringLiteral("NPN")).symbol);
    CHECK(transistor.model(QStringLiteral("spice"))->data.value(QStringLiteral("template")).toString()
          == QLatin1String("Q{ref} {pin:c} {pin:b} {pin:e} QX2"));

    const ComponentDefinition fast = proposeComponent(parsed.entries.at(2), modelPath, {});
    CHECK(fast.validate().isEmpty());
    CHECK(fast.model(QStringLiteral("spice"))->data.value(QStringLiteral("section")).toString() == QLatin1String("fast"));

    const ComponentDefinition opamp = proposeComponent(parsed.entries.at(3), modelPath, templates);
    INFO(opamp.validate().join(QLatin1Char('\n')).toStdString());
    CHECK(opamp.validate().isEmpty());
    CHECK(opamp.pins.size() == 5);
    CHECK(opamp.symbol.pins.size() == 5);
    CHECK(opamp.parameter(QStringLiteral("gain")));
    CHECK(opamp.model(QStringLiteral("spice"))->data.value(QStringLiteral("template")).toString()
          == QLatin1String("X{ref} {pin:INP} {pin:INN} {pin:OUT} {pin:VCC} {pin:VEE} OPX params: FP={fp} GAIN={gain}"));

    // The transistor in a circuit: base current through 100k from 1 V. A file
    // with .lib sections is only read through a section, so the transistor
    // comes from a plain file.
    const QString plainPath = dir.filePath(QStringLiteral("plain.mod"));
    {
        QFile file(plainPath);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(".model QX2 NPN(IS=1e-14 BF=200 VAF=100)\n");
    }
    const ComponentDefinition plainTransistor = proposeComponent(parsed.entries.at(0), plainPath, templates);
    Document *document = project->createDocument(DocumentType::SpiceSchematic, QStringLiteral("t"), &error);
    REQUIRE(document);
    Schematic *sheet = document->schematic();
    const QString key = QStringLiteral("imported:qx2");
    sheet->setCachedDefinition(key, std::make_shared<const ComponentDefinition>(plainTransistor));
    const QString q = sheet->addComponent(key, &plainTransistor, {0, 0});
    const PlacedComponent *placed = sheet->component(q);
    for (const SymbolPin &pin : transistor.symbol.pins)
        sheet->addLabel(QStringLiteral("n") + pin.pin, pinPosition(*placed, pin));
    SpiceAnalysisSetup op = SpiceAnalysisSetup::create(AnalysisKind::OperatingPoint);
    op.set(QStringLiteral("cards"), QStringLiteral("VC nc 0 5\nVB vb 0 1\nRB vb nb 100k\nVE ne 0 0"));
    const SpiceSimulation sim = runSpiceAnalysis(*project, *document, op, engine());
    INFO(sim.netlist.toStdString());
    INFO(sim.errors.join(QLatin1Char('\n')).toStdString());
    REQUIRE(sim.ok());
    const SpicePlot *plot = sim.run.plot(QStringLiteral("op"));
    REQUIRE(plot);
    const double ib = (1.0 - plot->vector(QStringLiteral("v(nb)"))->real.at(0)) / 100e3;
    const double ic = -plot->vector(QStringLiteral("i(vc)"))->real.at(0);
    CHECK(ic / ib == Approx(200).epsilon(0.05)); // BF, slightly raised by VAF
}

TEST_CASE("SPICE numbers are read with SPICE suffixes")
{
    CHECK(*parseSpiceNumber(QStringLiteral("10k")) == Approx(1e4));
    CHECK(*parseSpiceNumber(QStringLiteral("1meg")) == Approx(1e6));
    CHECK(*parseSpiceNumber(QStringLiteral("1M")) == Approx(1e-3));
    CHECK(*parseSpiceNumber(QStringLiteral("2.2uF")) == Approx(2.2e-6));
    CHECK(*parseSpiceNumber(QStringLiteral("1e-3")) == Approx(1e-3));
    CHECK(*parseSpiceNumber(QStringLiteral("5")) == Approx(5));
    CHECK_FALSE(parseSpiceNumber(QStringLiteral("{x}")));
}

TEST_CASE("an imported netlist gives its analyses and runs like the original")
{
    QFile file(QStringLiteral(LVD_SOURCE_DIR "/src/spice/tests/reference/bjt_amplifier.cir"));
    REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(file.readAll());
    const ImportedNetlist imported = importSpiceNetlist(text);
    CHECK(imported.title.startsWith(QLatin1String("Common-emitter")));
    REQUIRE(imported.analyses.size() == 5);
    CHECK(imported.analyses.at(0).kind == AnalysisKind::OperatingPoint);
    CHECK(imported.analyses.at(1).kind == AnalysisKind::Ac);
    CHECK(imported.analyses.at(1).text(QStringLiteral("stop")) == QLatin1String("100000000"));
    CHECK(imported.analyses.at(2).kind == AnalysisKind::Noise);
    CHECK(imported.analyses.at(2).text(QStringLiteral("source")) == QLatin1String("VIN"));
    CHECK(imported.analyses.at(3).kind == AnalysisKind::Transient);
    CHECK(imported.analyses.at(4).kind == AnalysisKind::Sensitivity);
    CHECK(imported.nodes.contains(QStringLiteral("out")));
    CHECK(imported.nodes.contains(QStringLiteral("vcc")));
    CHECK(imported.elements.contains(QStringLiteral("Q1")));
    CHECK_FALSE(imported.circuit.contains(QLatin1String(".ac")));

    // As a document: the netlist in the simulation object, analyses from it.
    QTemporaryDir dir;
    Session session;
    QString error;
    Project *project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
    REQUIRE(project);
    Document *document = project->createDocument(DocumentType::SpiceSchematic, QStringLiteral("amp"), &error);
    REQUIRE(document);
    QJsonObject simulation{{QStringLiteral("netlist"), imported.circuit}};
    for (const SpiceAnalysisSetup &setup : imported.analyses)
        simulation = withSpiceAnalysis(simulation, setup);
    document->schematic()->setSimulation(simulation);

    const SpiceSimulation ac = runSpiceAnalysis(*project, *document, spiceAnalyses(simulation).at(1), engine());
    INFO(ac.netlist.toStdString());
    INFO(ac.errors.join(QLatin1Char('\n')).toStdString());
    REQUIRE(ac.ok());
    const SpiceRunResult direct = engine().run(text);
    REQUIRE(direct.ok);
    const SpiceVector *mine = ac.run.plot(QStringLiteral("ac"))->vector(QStringLiteral("v(out)"));
    const SpiceVector *theirs = direct.plot(QStringLiteral("ac"))->vector(QStringLiteral("v(out)"));
    REQUIRE(mine);
    REQUIRE(theirs);
    REQUIRE(mine->size() == theirs->size());
    for (std::size_t i = 0; i < mine->size(); ++i)
        CHECK(std::abs(mine->complex[i] - theirs->complex[i]) <= 1e-9 * std::abs(theirs->complex[i]) + 1e-15);

    // The noise analysis names its source as in the netlist.
    const SpiceSimulation noise = runSpiceAnalysis(*project, *document, spiceAnalyses(simulation).at(2), engine());
    INFO(noise.errors.join(QLatin1Char('\n')).toStdString());
    CHECK(noise.ok());

    const ImportedNetlist withControl =
        importSpiceNetlist(QStringLiteral("t\nV1 a 0 SIN(0 1 1k)\nR1 a 0 1k\n.tran 1u 2m\n.four 1k v(a)\n.control\nrun\n.endc\n.end\n"));
    REQUIRE(withControl.analyses.size() == 1);
    CHECK(withControl.analyses.first().kind == AnalysisKind::Fourier);
    CHECK(withControl.analyses.first().list(QStringLiteral("signals")) == QStringList{QStringLiteral("v(a)")});
    CHECK_FALSE(withControl.warnings.isEmpty());
}

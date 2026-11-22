#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/Netlist.h"
#include "core/RuleCheck.h"
#include "core/Schematic.h"
#include "core/Session.h"

#include <QDir>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace lvd;

namespace {

QString key(const char *text)
{
    return QString::fromLatin1(text);
}

// A session with the built-in libraries and a fresh project.
struct Fixture {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;

    Fixture()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
    }

    Schematic &newSheet(const QString &name, DocumentType type = DocumentType::RfSchematic)
    {
        QString error;
        Document *document = project->createDocument(type, name, &error);
        REQUIRE(document);
        return *document->schematic();
    }

    DefinitionPtr definition(const Schematic &schematic, const QString &id)
    {
        return project->resolve(schematic, schematic.component(id)->definition).definition;
    }

    QString place(Schematic &schematic, const QString &definitionKey, QPoint at, int rotation = 0)
    {
        const DefinitionPtr found = project->resolve(schematic, definitionKey).definition;
        REQUIRE(found);
        return schematic.addComponent(definitionKey, found.get(), at, rotation);
    }

    QPoint pin(const Schematic &schematic, const QString &id, const QString &pinName)
    {
        const DefinitionPtr found = definition(schematic, id);
        REQUIRE(found);
        const SymbolPin *symbolPin = found->symbol.pin(pinName);
        REQUIRE(symbolPin);
        return pinPosition(*schematic.component(id), *symbolPin);
    }

    QString connect(Schematic &schematic, const QString &a, const QString &pinA, const QString &b, const QString &pinB)
    {
        return schematic.addWire({pin(schematic, a, pinA), pin(schematic, b, pinB)});
    }
};

} // namespace

TEST_CASE("the built-in libraries load completely and round-trip")
{
    LibraryManager libraries;
    libraries.setSearchPath(LibraryScope::BuiltIn, LibraryManager::defaultBuiltInPath());
    INFO(libraries.warnings().join(QLatin1Char('\n')).toStdString());
    REQUIRE(libraries.warnings().isEmpty());
    REQUIRE(libraries.libraryNames()
            == QStringList{QStringLiteral("basic"), QStringLiteral("rf-circuit"), QStringLiteral("rf-system"),
                           QStringLiteral("spice-ics"), QStringLiteral("spice-instruments"), QStringLiteral("spice-misc"),
                           QStringLiteral("spice-semiconductors"), QStringLiteral("spice-sources")});

    int count = 0;
    for (const auto &entry : libraries.entries()) {
        for (const DefinitionPtr &definition : entry.library.components) {
            ++count;
            const auto again = ComponentDefinition::fromJson(definition->toJson());
            REQUIRE(again);
            REQUIRE(*again == *definition);
        }
    }
    REQUIRE(count == 154);
    REQUIRE(libraries.find(key("basic:ground"))->netName == QStringLiteral("0"));
    REQUIRE(libraries.find(key("basic:sheet_port"))->isSheetPort());
    REQUIRE(libraries.find(key("nothing:here")) == nullptr);
    REQUIRE(libraries.find(key("malformed")) == nullptr);
}

TEST_CASE("a project library shadows a built-in library of the same name")
{
    QTemporaryDir dir;
    LibraryManager libraries;
    libraries.setSearchPath(LibraryScope::BuiltIn, LibraryManager::defaultBuiltInPath());
    Library custom;
    custom.name = QStringLiteral("basic");
    ComponentDefinition resistor = *libraries.find(key("basic:resistor"));
    resistor.name = QStringLiteral("Custom resistor");
    custom.setComponent(resistor);
    libraries.setSearchPath(LibraryScope::Project, dir.path());
    QString error;
    REQUIRE(libraries.saveLibrary(custom, LibraryScope::Project, &error));
    REQUIRE(libraries.find(key("basic:resistor"))->name == QStringLiteral("Custom resistor"));
    REQUIRE(libraries.find(key("basic:capacitor")) == nullptr); // the whole library is shadowed
    REQUIRE(libraries.scopeOf(QStringLiteral("basic")) == LibraryScope::Project);
}

TEST_CASE("orientation maps symbol points as on screen")
{
    const QPoint right(10, 0);
    REQUIRE(Orientation{0, false}.map(right) == QPoint(10, 0));
    REQUIRE(Orientation{90, false}.map(right) == QPoint(0, -10)); // counter-clockwise: right becomes up
    REQUIRE(Orientation{180, false}.map(right) == QPoint(-10, 0));
    REQUIRE(Orientation{270, false}.map(right) == QPoint(0, 10));
    REQUIRE(Orientation{0, true}.map(right) == QPoint(-10, 0));
    REQUIRE(Orientation{90, true}.map(right) == QPoint(0, 10));
    REQUIRE(Orientation{90, false}.map(PinDirection::Right) == PinDirection::Up);
    REQUIRE(normalizedRotation(-90) == 270);
    REQUIRE(normalizedRotation(450) == 90);
}

TEST_CASE("schematic edits are undoable and survive save and reload")
{
    Fixture f;
    Schematic &sheet = f.newSheet(QStringLiteral("rx"));
    Document *document = f.project->document(QStringLiteral("rx"));
    REQUIRE_FALSE(document->isModified());

    QSignalSpy changed(&sheet, &Schematic::changed);
    const QString amp = f.place(sheet, key("rf-system:amplifier"), {100, 100});
    const QString amp2 = f.place(sheet, key("rf-system:amplifier"), {300, 100});
    REQUIRE(sheet.component(amp)->reference == QStringLiteral("A1"));
    REQUIRE(sheet.component(amp2)->reference == QStringLiteral("A2"));
    REQUIRE(document->isModified());
    REQUIRE(changed.count() == 2);

    // One grouped edit is one undo step.
    sheet.beginEdit(QStringLiteral("tune"));
    sheet.setParameter(amp, QStringLiteral("gain"), QStringLiteral("17.5"));
    sheet.setParameter(amp, QStringLiteral("nf"), QStringLiteral("0.9"));
    sheet.endEdit();
    REQUIRE(sheet.undoStack()->count() == 3);
    REQUIRE(sheet.undoStack()->undoText() == QStringLiteral("tune"));

    sheet.undoStack()->undo();
    REQUIRE(sheet.component(amp)->parameters.isEmpty());
    sheet.undoStack()->redo();
    REQUIRE(sheet.component(amp)->parameters.value(QStringLiteral("gain")) == QStringLiteral("17.5"));

    const QString wire = f.connect(sheet, amp, QStringLiteral("out"), amp2, QStringLiteral("in"));
    REQUIRE_FALSE(wire.isEmpty());
    sheet.removeItem(amp2);
    REQUIRE_FALSE(sheet.contains(amp2));
    sheet.undoStack()->undo();
    REQUIRE(sheet.component(amp2)->reference == QStringLiteral("A2"));

    // Ids are never reused, even after a delete.
    sheet.removeItem(wire);
    const QString label = sheet.addLabel(QStringLiteral("IF"), {0, 0});
    REQUIRE(label != wire);
    REQUIRE(Schematic::kindOf(label) == ItemKind::Label);

    QString error;
    REQUIRE(document->save(&error));
    REQUIRE_FALSE(document->isModified());
    REQUIRE(sheet.cachedDefinitions().keys() == QStringList{key("rf-system:amplifier")});

    // Undo after a save makes the document modified again; redo makes it clean.
    sheet.undoStack()->undo();
    REQUIRE(document->isModified());
    sheet.undoStack()->redo();
    REQUIRE_FALSE(document->isModified());

    auto reloaded = Document::load(document->filePath(), &error);
    REQUIRE(reloaded);
    REQUIRE(reloaded->schematic()->toJson() == sheet.toJson());
    // nextId persists, so a new item after reload does not reuse a deleted id.
    REQUIRE(reloaded->schematic()->addJunction({0, 0}) != wire);
}

TEST_CASE("moving a component drags attached wire ends and keeps them orthogonal")
{
    Fixture f;
    Schematic &sheet = f.newSheet(QStringLiteral("drag"));
    const QString a = f.place(sheet, key("rf-system:amplifier"), {0, 0});
    const QString b = f.place(sheet, key("rf-system:amplifier"), {200, 0});
    const QString wire = f.connect(sheet, a, QStringLiteral("out"), b, QStringLiteral("in"));
    const QPoint before = f.pin(sheet, b, QStringLiteral("in"));

    sheet.moveItems({b}, QPoint(0, 50), {{b, pinPositions(*sheet.component(b), *f.definition(sheet, b))}}, true);
    const QList<QPoint> points = sheet.wires().value(wire).points;
    REQUIRE(points.first() == f.pin(sheet, a, QStringLiteral("out")));
    REQUIRE(points.last() == before + QPoint(0, 50));
    for (qsizetype i = 1; i < points.size(); ++i)
        REQUIRE((points.at(i).x() == points.at(i - 1).x() || points.at(i).y() == points.at(i - 1).y()));
    REQUIRE(sheet.undoStack()->undoText() == QStringLiteral("Move"));

    const Connectivity connectivity = computeConnectivity(sheet, f.project->lookupFor(sheet));
    REQUIRE(connectivity.netOfPin(a, QStringLiteral("out")) == connectivity.netOfPin(b, QStringLiteral("in")));
}

TEST_CASE("rotation and mirroring move pins as expected")
{
    Fixture f;
    Schematic &sheet = f.newSheet(QStringLiteral("rot"));
    const QString r = f.place(sheet, key("basic:resistor"), {100, 100});
    REQUIRE(f.pin(sheet, r, QStringLiteral("p")) == QPoint(70, 100));
    sheet.rotateItems({r}, QPoint(100, 100), 90);
    REQUIRE(f.pin(sheet, r, QStringLiteral("p")) == QPoint(100, 130)); // left pin turns to the bottom
    sheet.mirrorItems({r}, QPoint(100, 100));
    REQUIRE(f.pin(sheet, r, QStringLiteral("p")) == QPoint(100, 130)); // vertical part: mirror about x keeps it
    sheet.rotateItems({r}, QPoint(100, 100), 90);
    REQUIRE(f.pin(sheet, r, QStringLiteral("p")) == QPoint(130, 100));
}

TEST_CASE("connectivity follows the geometry rules")
{
    Fixture f;
    Schematic &sheet = f.newSheet(QStringLiteral("nets"));
    const auto lookup = f.project->lookupFor(sheet);

    SECTION("wires, ground, labels and naming")
    {
        const QString r1 = f.place(sheet, key("basic:resistor"), {100, 100});
        const QString r2 = f.place(sheet, key("basic:resistor"), {300, 100});
        const QString gnd = f.place(sheet, key("basic:ground"), {400, 100});
        f.connect(sheet, r1, QStringLiteral("n"), r2, QStringLiteral("p"));
        f.connect(sheet, r2, QStringLiteral("n"), gnd, QStringLiteral("gnd"));

        Connectivity c = computeConnectivity(sheet, lookup);
        REQUIRE(c.nets.at(c.netOfPin(r2, QStringLiteral("n"))).name == QStringLiteral("0"));
        REQUIRE(c.nets.at(c.netOfPin(r1, QStringLiteral("n"))).name == QStringLiteral("Net-(R1-n)"));
        REQUIRE(c.nets.at(c.netOfPin(r1, QStringLiteral("p"))).pins.size() == 1);

        // A label on the wire names the net; the same label elsewhere joins it.
        sheet.addLabel(QStringLiteral("MID"), {200, 100});
        sheet.addLabel(QStringLiteral("MID"), f.pin(sheet, r1, QStringLiteral("p")));
        c = computeConnectivity(sheet, lookup);
        const SheetNet *mid = c.net(QStringLiteral("MID"));
        REQUIRE(mid);
        REQUIRE(mid->pins.size() == 3);

        // Ground wins over a label.
        sheet.addLabel(QStringLiteral("RETURN"), f.pin(sheet, gnd, QStringLiteral("gnd")));
        c = computeConnectivity(sheet, lookup);
        REQUIRE(c.net(QStringLiteral("0")));
        REQUIRE_FALSE(c.net(QStringLiteral("RETURN")));
    }

    SECTION("crossing wires connect only through a junction; T connections always")
    {
        const QString h = sheet.addWire({{0, 100}, {200, 100}});
        const QString v = sheet.addWire({{100, 0}, {100, 200}});
        Connectivity c = computeConnectivity(sheet, lookup);
        REQUIRE(c.netOfItem(h) != c.netOfItem(v));
        REQUIRE(missingJunctions(sheet, lookup).isEmpty());

        const QString j = sheet.addJunction({100, 100});
        c = computeConnectivity(sheet, lookup);
        REQUIRE(c.netOfItem(h) == c.netOfItem(v));
        REQUIRE(redundantJunctions(sheet, lookup).isEmpty());
        sheet.removeItem(j);

        const QString t = sheet.addWire({{50, 100}, {50, 300}}); // ends in the middle of h
        c = computeConnectivity(sheet, lookup);
        REQUIRE(c.netOfItem(t) == c.netOfItem(h));
        REQUIRE(missingJunctions(sheet, lookup) == QList<QPoint>{QPoint(50, 100)});
    }
}

TEST_CASE("a hierarchical design flattens into a correct netlist")
{
    Fixture f;

    // Sheet "stage": in -> A1 -> out, with gain from a variable.
    Schematic &stage = f.newSheet(QStringLiteral("stage"));
    stage.setVariables({{QStringLiteral("g"), QStringLiteral("10")}, {QStringLiteral("g2"), QStringLiteral("g + 1")}});
    const QString portIn = f.place(stage, key("basic:sheet_port"), {-100, 0});
    stage.setParameter(portIn, QStringLiteral("name"), QStringLiteral("in"));
    const QString portOut = f.place(stage, key("basic:sheet_port"), {150, 0});
    stage.setParameter(portOut, QStringLiteral("name"), QStringLiteral("out"));
    const QString amp = f.place(stage, key("rf-system:amplifier"), {0, 0});
    stage.setParameter(amp, QStringLiteral("gain"), QStringLiteral("g2"));
    f.connect(stage, portIn, QStringLiteral("p"), amp, QStringLiteral("in"));
    f.connect(stage, amp, QStringLiteral("out"), portOut, QStringLiteral("p"));

    const DefinitionPtr block = f.project->sheetDefinition(QStringLiteral("stage"));
    REQUIRE(block);
    REQUIRE(block->pins == QStringList{QStringLiteral("in"), QStringLiteral("out")});
    REQUIRE(block->symbol.pin(QStringLiteral("in"))->at.x() < 0);
    REQUIRE(block->symbol.pin(QStringLiteral("out"))->at.x() > 0);
    REQUIRE(block->parameter(QStringLiteral("g"))->defaultValue == QStringLiteral("10"));
    REQUIRE(block->validate().isEmpty());

    // Top: SRC -> X1 -> X2 -> TERM, X1 with g overridden from a top variable.
    Schematic &top = f.newSheet(QStringLiteral("top"));
    top.setVariables({{QStringLiteral("boost"), QStringLiteral("20")}});
    const QString src = f.place(top, key("rf-system:source"), {-200, 0});
    const QString x1 = f.place(top, Project::sheetKey(QStringLiteral("stage")), {0, 0});
    const QString x2 = f.place(top, Project::sheetKey(QStringLiteral("stage")), {200, 0});
    const QString term = f.place(top, key("rf-system:termination"), {400, 0});
    top.setParameter(x1, QStringLiteral("g"), QStringLiteral("boost"));
    f.connect(top, src, QStringLiteral("out"), x1, QStringLiteral("in"));
    f.connect(top, x1, QStringLiteral("out"), x2, QStringLiteral("in"));
    f.connect(top, x2, QStringLiteral("out"), term, QStringLiteral("p"));
    top.addLabel(QStringLiteral("RF_OUT"), f.pin(top, term, QStringLiteral("p")));
    REQUIRE(top.component(x1)->reference == QStringLiteral("X1"));

    const Netlist netlist = buildNetlist(*f.project, *f.project->document(QStringLiteral("top")));
    INFO(netlist.toText().toStdString());
    REQUIRE(netlist.errors.isEmpty());
    REQUIRE(netlist.components.size() == 4);

    const FlatComponent *a1 = netlist.component(QStringLiteral("X1/A1"));
    const FlatComponent *a2 = netlist.component(QStringLiteral("X2/A1"));
    REQUIRE(a1);
    REQUIRE(a2);
    REQUIRE(a1->parameterValues.value(QStringLiteral("gain")) == 21.0); // g2 = boost + 1
    REQUIRE(a2->parameterValues.value(QStringLiteral("gain")) == 11.0); // g2 = default g + 1
    REQUIRE(a1->parameterValues.value(QStringLiteral("nf")) == 2.0);    // library default

    // Port nets take the parent's names.
    const QString between = a1->pinNets.value(QStringLiteral("out"));
    REQUIRE(between == a2->pinNets.value(QStringLiteral("in")));
    REQUIRE_FALSE(between.contains(QLatin1Char('/')));
    REQUIRE(netlist.pinsOn(between) == QStringList{QStringLiteral("X1/A1.out"), QStringLiteral("X2/A1.in")});
    REQUIRE(a2->pinNets.value(QStringLiteral("out")) == QStringLiteral("RF_OUT"));
    REQUIRE(netlist.component(QStringLiteral("SRC1"))->pinNets.value(QStringLiteral("out"))
            == a1->pinNets.value(QStringLiteral("in")));

    SECTION("a sheet that contains itself is reported, not followed")
    {
        f.place(stage, Project::sheetKey(QStringLiteral("stage")), {0, 200});
        const Netlist broken = buildNetlist(*f.project, *f.project->document(QStringLiteral("top")));
        REQUIRE_FALSE(broken.errors.isEmpty());
        REQUIRE(broken.errors.first().contains(QStringLiteral("contains itself")));
    }

    SECTION("the rule check passes on the clean design")
    {
        const auto violations = checkSchematic(*f.project, *f.project->document(QStringLiteral("top")));
        QStringList messages;
        for (const RuleViolation &violation : violations)
            messages.append(violation.message);
        INFO(messages.join(QLatin1Char('\n')).toStdString());
        REQUIRE(std::none_of(violations.begin(), violations.end(), [](const RuleViolation &v) {
            return v.severity != RuleViolation::Severity::Info;
        }));
    }
}

TEST_CASE("definitions fall back to the cache when the library is missing")
{
    QTemporaryDir dir;
    QString projectFile;
    {
        Fixture f;
        Schematic &sheet = f.newSheet(QStringLiteral("rx"));
        f.place(sheet, key("rf-system:amplifier"), {0, 0});
        f.place(sheet, key("rf-system:amplifier"), {0, 200});
        QString error;
        REQUIRE(f.project->saveAll(&error));
        // Copy the project away before the fixture's folder disappears.
        const QDir source(f.project->directory());
        QDir().mkpath(dir.filePath(QStringLiteral("copy")));
        for (const QString &file : source.entryList(QDir::Files))
            QFile::copy(source.filePath(file), dir.filePath(QStringLiteral("copy/") + file));
        projectFile = dir.filePath(QStringLiteral("copy/p.lvdproj"));
    }

    // No libraries at all: everything comes from the cache.
    QString error;
    auto project = Project::load(projectFile, &error);
    REQUIRE(project);
    Document *document = project->document(QStringLiteral("rx"));
    const Schematic &sheet = *document->schematic();
    const Resolution resolution = project->resolve(sheet, key("rf-system:amplifier"));
    REQUIRE(resolution.source == Resolution::Source::Cache);
    REQUIRE(resolution.definition->name == QStringLiteral("Amplifier"));

    const auto violations = checkSchematic(*project, *document);
    const auto has = [&](const QString &text) {
        return std::any_of(violations.begin(), violations.end(),
                           [&](const RuleViolation &v) { return v.message.contains(text); });
    };
    REQUIRE(has(QStringLiteral("using the copy stored in the schematic")));
    REQUIRE(has(QStringLiteral("not connected")));

    REQUIRE_FALSE(has(QStringLiteral("is used 2 times"))); // A1 and A2

    // A library copy that differs from the cache is flagged.
    LibraryManager libraries;
    Library changed;
    changed.name = QStringLiteral("rf-system");
    ComponentDefinition amplifier = *resolution.definition;
    amplifier.description = QStringLiteral("changed");
    changed.setComponent(amplifier);
    libraries.setSearchPath(LibraryScope::User, dir.filePath(QStringLiteral("userlib")));
    REQUIRE(libraries.saveLibrary(changed, LibraryScope::User, &error));
    project->setLibraryManager(&libraries);
    const Resolution updated = project->resolve(sheet, key("rf-system:amplifier"));
    REQUIRE(updated.source == Resolution::Source::Library);
    REQUIRE(updated.changedInLibrary);
}

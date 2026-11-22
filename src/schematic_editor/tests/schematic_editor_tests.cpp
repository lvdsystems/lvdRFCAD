#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/Document.h"
#include "core/Expression.h"
#include "core/Schematic.h"
#include "core/UndoStack.h"
#include "core/Session.h"
#include "schematic_editor/EditorSupport.h"
#include "schematic_editor/PropertiesPanel.h"
#include "schematic_editor/SchematicEditor.h"
#include "schematic_editor/SchematicItems.h"
#include "schematic_editor/SchematicScene.h"
#include "schematic_editor/SchematicView.h"
#include "schematic_editor/VariablesPanel.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

using namespace lvd;

namespace {

const QString Amplifier = QStringLiteral("rf-system:amplifier");

struct Fixture {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    std::unique_ptr<SchematicEditor> editor;
    QStringList messages;

    Fixture()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("main"), &error);
        REQUIRE(document);
    }

    void open()
    {
        editor = std::make_unique<SchematicEditor>(project, document);
        QObject::connect(editor.get(), &SchematicEditor::statusMessage,
                         [this](const QString &text) { messages.append(text); });
        editor->resize(1000, 700);
        editor->show();
        QApplication::processEvents();
        view()->setZoom(2.0);
        view()->centerOn(0, 0);
        QApplication::processEvents();
    }

    Schematic &schematic() { return *document->schematic(); }
    SchematicView *view() { return editor->view(); }
    QWidget *viewport() { return view()->viewport(); }
    QPoint at(QPoint sheet) { return view()->viewportPoint(sheet); }

    void click(QPoint sheet, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QTest::mouseMove(viewport(), at(sheet));
        QTest::mouseClick(viewport(), Qt::LeftButton, modifiers, at(sheet));
    }
    void doubleClick(QPoint sheet)
    {
        QTest::mouseDClick(viewport(), Qt::LeftButton, Qt::NoModifier, at(sheet));
    }
    void drag(QPoint from, QPoint to)
    {
        QTest::mouseMove(viewport(), at(from));
        QTest::mousePress(viewport(), Qt::LeftButton, Qt::NoModifier, at(from));
        const QPoint middle = (from + to) / 2;
        QTest::mouseMove(viewport(), at(middle));
        QTest::mouseMove(viewport(), at(to));
        QTest::mouseRelease(viewport(), Qt::LeftButton, Qt::NoModifier, at(to));
    }
    void trigger(const char *name)
    {
        QAction *action = editor->action(QString::fromLatin1(name));
        REQUIRE(action);
        action->trigger();
    }

    QString place(const QString &key, QPoint at, int rotation = 0)
    {
        const DefinitionPtr definition = project->resolve(schematic(), key).definition;
        REQUIRE(definition);
        return schematic().addComponent(key, definition.get(), at, rotation);
    }
    QPoint pin(const QString &id, const QString &name)
    {
        const DefinitionPtr definition = project->resolve(schematic(), schematic().component(id)->definition).definition;
        return pinPosition(*schematic().component(id), *definition->symbol.pin(name));
    }
    bool sameNet(const QString &a, const QString &pa, const QString &b, const QString &pb)
    {
        const Connectivity c = computeConnectivity(schematic(), project->lookupFor(schematic()));
        const int na = c.netOfPin(a, pa);
        return na >= 0 && na == c.netOfPin(b, pb);
    }
    QString componentAt(QPoint p)
    {
        for (const PlacedComponent &component : schematic().components()) {
            if (component.at == p)
                return component.id;
        }
        return {};
    }
};

} // namespace

TEST_CASE("placement mode places parts at the snapped cursor and rotates the ghost")
{
    Fixture f;
    f.open();
    f.editor->startPlacing(Amplifier);
    REQUIRE(f.view()->isPlacing());
    f.click(QPoint(-102, 3)); // snaps to (-100, 0)
    f.trigger("rotate");       // R rotates the part being placed
    f.click(QPoint(100, 0));
    REQUIRE(f.view()->isPlacing()); // stays in placement mode
    QTest::keyClick(f.viewport(), Qt::Key_Escape);
    REQUIRE_FALSE(f.view()->isPlacing());

    REQUIRE(f.schematic().components().size() == 2);
    const PlacedComponent *first = f.schematic().component(f.componentAt(QPoint(-100, 0)));
    const PlacedComponent *second = f.schematic().component(f.componentAt(QPoint(100, 0)));
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->reference == QStringLiteral("A1"));
    CHECK(first->rotation == 0);
    CHECK(second->reference == QStringLiteral("A2"));
    CHECK(second->rotation == 90);
    CHECK(f.schematic().undoStack()->count() == 2);

    // Unknown definitions are refused.
    f.editor->startPlacing(QStringLiteral("nolib:nothing"));
    CHECK_FALSE(f.view()->isPlacing());
}

TEST_CASE("the wire tool connects two pins and inserts junctions")
{
    Fixture f;
    const QString a1 = f.place(Amplifier, QPoint(0, 0));
    const QString a2 = f.place(Amplifier, QPoint(150, 0));
    const QString a3 = f.place(Amplifier, QPoint(150, 100));
    f.open();
    f.trigger("wireTool");
    REQUIRE(f.view()->tool() == SchematicView::Tool::Wire);

    // Clicking the second pin ends the wire.
    f.click(f.pin(a1, QStringLiteral("out")));
    f.click(f.pin(a2, QStringLiteral("in")));
    REQUIRE_FALSE(f.view()->isDrawingWire());
    REQUIRE(f.schematic().wires().size() == 1);
    CHECK(f.sameNet(a1, QStringLiteral("out"), a2, QStringLiteral("in")));
    CHECK(f.editor->connectivity().netOfPin(a1, QStringLiteral("out"))
          == f.editor->connectivity().netOfPin(a2, QStringLiteral("in")));

    // A branch from the middle of that wire, vertical first, to a3's input:
    // an L-shaped wire and a junction at the tee, all in one undo step.
    const int steps = f.schematic().undoStack()->count();
    const QPoint tee(80, 0);
    f.click(tee);
    QTest::keyClick(f.viewport(), Qt::Key_Space); // vertical first
    f.click(f.pin(a3, QStringLiteral("in")));
    REQUIRE(f.schematic().wires().size() == 2);
    CHECK(f.schematic().undoStack()->count() == steps + 1);
    CHECK(f.sameNet(a1, QStringLiteral("out"), a3, QStringLiteral("in")));
    REQUIRE(f.schematic().junctions().size() == 1);
    CHECK(f.schematic().junctions().first().at == tee);
    Wire branch;
    for (const Wire &wire : f.schematic().wires()) {
        if (wire.points.first() == tee)
            branch = wire;
    }
    CHECK(branch.points == QList<QPoint>{tee, QPoint(80, 100), f.pin(a3, QStringLiteral("in"))});

    // A free-standing wire ends with a double-click; Backspace drops a point.
    f.click(QPoint(-100, -100));
    f.click(QPoint(-50, -100));
    f.click(QPoint(-50, -50));
    QTest::keyClick(f.viewport(), Qt::Key_Backspace);
    f.click(QPoint(0, -100));
    f.doubleClick(QPoint(0, -100));
    REQUIRE(f.schematic().wires().size() == 3);
    bool found = false;
    for (const Wire &wire : f.schematic().wires())
        found = found || wire.points == QList<QPoint>{QPoint(-100, -100), QPoint(0, -100)};
    CHECK(found);

    // Escape cancels a wire in progress; deleting the branch removes the junction.
    f.click(QPoint(-100, 200));
    QTest::keyClick(f.viewport(), Qt::Key_Escape);
    CHECK_FALSE(f.view()->isDrawingWire());
    CHECK(f.schematic().wires().size() == 3);
    f.editor->selectItems({branch.id});
    f.trigger("delete");
    CHECK(f.schematic().junctions().isEmpty());
    f.trigger("undo");
    CHECK(f.schematic().junctions().size() == 1);
    CHECK(f.schematic().wires().contains(branch.id));
}

TEST_CASE("dragging a part stretches its wires in one undo step")
{
    Fixture f;
    const QString a1 = f.place(Amplifier, QPoint(0, 0));
    const QString a2 = f.place(Amplifier, QPoint(150, 0));
    f.schematic().addWire({f.pin(a1, QStringLiteral("out")), f.pin(a2, QStringLiteral("in"))});
    f.open();
    const int steps = f.schematic().undoStack()->count();

    f.click(QPoint(150, 4));
    REQUIRE(f.editor->selectedItemIds() == QStringList{a2});
    f.drag(QPoint(150, 4), QPoint(181, 63)); // snapped delta (30, 60)
    CHECK(f.schematic().component(a2)->at == QPoint(180, 60));
    CHECK(f.schematic().undoStack()->count() == steps + 1);
    CHECK(f.sameNet(a1, QStringLiteral("out"), a2, QStringLiteral("in")));
    for (const Wire &wire : f.schematic().wires()) {
        for (qsizetype i = 1; i < wire.points.size(); ++i) {
            const QPoint a = wire.points.at(i - 1), b = wire.points.at(i);
            CHECK((a.x() == b.x() || a.y() == b.y())); // still orthogonal
        }
    }

    // Arrow keys nudge by one grid step.
    QTest::keyClick(f.viewport(), Qt::Key_Left);
    CHECK(f.schematic().component(a2)->at == QPoint(170, 60));
    CHECK(f.sameNet(a1, QStringLiteral("out"), a2, QStringLiteral("in")));

    f.trigger("undo");
    f.trigger("undo");
    CHECK(f.schematic().component(a2)->at == QPoint(150, 0));
    CHECK(f.schematic().wires().first().points
          == QList<QPoint>{f.pin(a1, QStringLiteral("out")), f.pin(a2, QStringLiteral("in"))});

    // Rubber band from left to right selects what is inside.
    f.drag(QPoint(-60, -60), QPoint(60, 60));
    CHECK(f.editor->selectedItemIds() == QStringList{a1});
    // Shift+click adds to the selection.
    f.click(QPoint(150, 4), Qt::ShiftModifier);
    CHECK(f.editor->selectedItemIds().size() == 2);
    // Clicking empty space clears it.
    f.click(QPoint(300, 300));
    CHECK(f.editor->selectedItemIds().isEmpty());
}

TEST_CASE("rotate, mirror, delete, undo and redo")
{
    Fixture f;
    const QString a1 = f.place(Amplifier, QPoint(0, 0));
    f.open();
    f.click(QPoint(0, 3));
    REQUIRE(f.editor->selectedItemIds() == QStringList{a1});
    f.trigger("rotate");
    CHECK(f.schematic().component(a1)->rotation == 90);
    CHECK(f.schematic().component(a1)->at == QPoint(0, 0));
    f.trigger("mirror");
    CHECK(f.schematic().component(a1)->mirror);
    f.trigger("delete");
    CHECK(f.schematic().components().isEmpty());
    CHECK(f.editor->scene()->schematicItems().isEmpty());

    QAction *undo = f.editor->action(QStringLiteral("undo"));
    CHECK(undo->isEnabled());
    CHECK(undo->text().contains(QStringLiteral("Delete")));
    f.trigger("undo");
    REQUIRE(f.schematic().component(a1));
    CHECK(f.editor->scene()->item(a1));
    f.trigger("undo");
    CHECK_FALSE(f.schematic().component(a1)->mirror);
    f.trigger("undo");
    CHECK(f.schematic().component(a1)->rotation == 0);
    f.trigger("redo");
    CHECK(f.schematic().component(a1)->rotation == 90);
    CHECK(f.editor->action(QStringLiteral("redo"))->isEnabled());
}

TEST_CASE("the properties panel edits parameters and references undoably")
{
    Fixture f;
    const QString a1 = f.place(Amplifier, QPoint(0, 0));
    f.schematic().setVariables({{QStringLiteral("g"), QStringLiteral("12")}});
    f.open();
    f.editor->selectItems({a1});
    QApplication::processEvents();

    QWidget *panel = f.editor->propertiesWidget();
    auto *gain = panel->findChild<QLineEdit *>(QStringLiteral("value:gain"));
    REQUIRE(gain);
    CHECK(gain->text().isEmpty()); // not overridden: the default shows as placeholder
    CHECK(gain->placeholderText() == QStringLiteral("15"));

    gain->setText(QStringLiteral("g + 8"));
    Q_EMIT gain->editingFinished();
    CHECK(f.schematic().component(a1)->parameters.value(QStringLiteral("gain")) == QStringLiteral("g + 8"));
    QApplication::processEvents();
    auto *table = panel->findChild<QTableWidget *>(QStringLiteral("parameters"));
    REQUIRE(table);
    CHECK(table->item(0, 3)->text() == QStringLiteral("20 dB"));

    // Setting the default value removes the override.
    gain->setText(QStringLiteral("15"));
    Q_EMIT gain->editingFinished();
    CHECK_FALSE(f.schematic().component(a1)->parameters.contains(QStringLiteral("gain")));
    f.trigger("undo");
    CHECK(f.schematic().component(a1)->parameters.value(QStringLiteral("gain")) == QStringLiteral("g + 8"));

    auto *reference = panel->findChild<QLineEdit *>(QStringLiteral("reference"));
    REQUIRE(reference);
    reference->setText(QStringLiteral("LNA1"));
    Q_EMIT reference->editingFinished();
    CHECK(f.schematic().component(a1)->reference == QStringLiteral("LNA1"));

    // An invalid expression shows its error.
    gain->setText(QStringLiteral("nope * 2"));
    Q_EMIT gain->editingFinished();
    QApplication::processEvents();
    CHECK_FALSE(table->item(0, 3)->text().isEmpty());
    CHECK(table->item(0, 3)->text() != QStringLiteral("20 dB"));
}

TEST_CASE("standard values keep the unit style of the text")
{
    const auto text = [](const char *value, const char *unit, ESeries series, Rounding rounding = Rounding::Nearest) {
        return standardValueText(QString::fromUtf8(value), QString::fromLatin1(unit), series, rounding).value_or(QStringLiteral("none"));
    };
    CHECK(text("4.5pF", "F", ESeries::E24) == QStringLiteral("4.7pF"));
    CHECK(text("4.5 pF", "F", ESeries::E24) == QStringLiteral("4.7 pF"));
    CHECK(text("4.5p", "F", ESeries::E12) == QStringLiteral("4.7p"));
    CHECK(text("4.5p", "F", ESeries::E12, Rounding::Down) == QStringLiteral("3.9p"));
    CHECK(text("2*2.2p", "F", ESeries::E12, Rounding::Up) == QStringLiteral("4.7p"));
    CHECK(text("1.06k", "ohm", ESeries::E96, Rounding::Down) == QStringLiteral("1.05k"));
    CHECK(text("1060", "ohm", ESeries::E96).toStdString() == "1.07k");
    CHECK(text("1.055kOhm", "ohm", ESeries::E96).toStdString() == "1.05kohm");
    CHECK(text("30u", "H", ESeries::E12) == QStringLiteral("33u"));
    CHECK(text("30µH", "H", ESeries::E12) == QStringLiteral("33µH"));
    CHECK(text("6.37e-12", "F", ESeries::E96) == QStringLiteral("6.34p"));
    CHECK(text("x * 2", "F", ESeries::E12) == QStringLiteral("none"));
    CHECK(text("-1p", "F", ESeries::E12) == QStringLiteral("none"));
    CHECK(text("0", "F", ESeries::E12) == QStringLiteral("none"));
}

TEST_CASE("the properties panel rounds a parameter to a standard value, undoably")
{
    Fixture f;
    const QString c1 = f.place(QStringLiteral("basic:capacitor"), QPoint(0, 0));
    f.schematic().setParameter(c1, QStringLiteral("c"), QStringLiteral("4.5pF"));
    f.schematic().setVariables({{QStringLiteral("cx"), QStringLiteral("2p")}});
    f.open();
    f.editor->selectItems({c1});
    QApplication::processEvents();
    auto *panel = qobject_cast<PropertiesPanel *>(f.editor->propertiesWidget());
    REQUIRE(panel);
    auto *button = panel->findChild<QToolButton *>(QStringLiteral("standard:c"));
    REQUIRE(button);
    CHECK(button->isEnabled());
    QMenu *menu = panel->standardValueMenu(QStringLiteral("c"));
    REQUIRE(menu);
    Q_EMIT menu->aboutToShow(); // fills the menu with the values
    auto *e24Nearest = menu->findChild<QAction *>(QStringLiteral("E24-%1").arg(int(Rounding::Nearest)));
    REQUIRE(e24Nearest);
    CHECK(e24Nearest->text().contains(QStringLiteral("4.7pF")));

    const int steps = f.schematic().undoStack()->count();
    e24Nearest->trigger();
    CHECK(f.schematic().component(c1)->parameters.value(QStringLiteral("c")) == QStringLiteral("4.7pF"));
    CHECK(f.schematic().undoStack()->count() == steps + 1);
    // Already a standard value: nothing changes.
    CHECK(panel->roundToStandardValue(QStringLiteral("c"), ESeries::E24));
    CHECK(f.schematic().undoStack()->count() == steps + 1);
    CHECK(panel->roundToStandardValue(QStringLiteral("c"), ESeries::E96));
    CHECK(f.schematic().component(c1)->parameters.value(QStringLiteral("c")).toStdString() == "4.75pF");
    f.trigger("undo");
    CHECK(f.schematic().component(c1)->parameters.value(QStringLiteral("c")) == QStringLiteral("4.7pF"));
    f.trigger("undo");
    CHECK(f.schematic().component(c1)->parameters.value(QStringLiteral("c")) == QStringLiteral("4.5pF"));

    // Values that use variables are not rounded.
    f.schematic().setParameter(c1, QStringLiteral("c"), QStringLiteral("cx * 2"));
    QApplication::processEvents();
    CHECK_FALSE(button->isEnabled());
    CHECK_FALSE(panel->roundToStandardValue(QStringLiteral("c"), ESeries::E12));
    CHECK(f.schematic().component(c1)->parameters.value(QStringLiteral("c")) == QStringLiteral("cx * 2"));
}

TEST_CASE("the variables panel adds, edits and evaluates variables")
{
    Fixture f;
    f.open();
    auto *panel = qobject_cast<VariablesPanel *>(f.editor->variablesWidget());
    REQUIRE(panel);
    panel->addVariable();
    panel->addVariable();
    REQUIRE(f.schematic().variables().size() == 2);
    QTableWidget *table = panel->table();
    table->item(0, 1)->setText(QStringLiteral("2.4GHz"));
    table->item(1, 1)->setText(QStringLiteral("var1 / 2"));
    CHECK(f.schematic().variables().at(1).value == QStringLiteral("var1 / 2"));
    CHECK(table->item(1, 2)->text() == formatEngineering(1.2e9));
    table->item(1, 1)->setText(QStringLiteral("undefined_thing"));
    CHECK_FALSE(table->item(1, 2)->text().isEmpty());
    table->setCurrentCell(1, 0);
    panel->moveSelected(-1);
    CHECK(f.schematic().variables().at(0).name == QStringLiteral("var2"));
    panel->removeSelected();
    CHECK(f.schematic().variables().size() == 1);
    f.trigger("undo");
    CHECK(f.schematic().variables().size() == 2);
}

TEST_CASE("copy and paste keep parameters and give new references")
{
    Fixture f;
    const QString a1 = f.place(Amplifier, QPoint(0, 0));
    f.schematic().setParameter(a1, QStringLiteral("gain"), QStringLiteral("21"));
    f.schematic().rotateItems({a1}, QPoint(0, 0));
    f.schematic().addLabel(QStringLiteral("RF"), QPoint(-30, 0));
    f.open();
    f.editor->selectItems(f.schematic().itemIds());
    f.trigger("copy");
    f.trigger("paste");
    REQUIRE(f.view()->isFloating());
    QTest::mouseMove(f.viewport(), f.at(QPoint(200, 100)));
    QTest::mouseClick(f.viewport(), Qt::LeftButton, Qt::NoModifier, f.at(QPoint(200, 100)));
    CHECK_FALSE(f.view()->isFloating());
    REQUIRE(f.schematic().components().size() == 2);
    REQUIRE(f.schematic().labels().size() == 2);

    const PlacedComponent *copy = nullptr;
    for (const PlacedComponent &component : f.schematic().components()) {
        if (component.id != a1)
            copy = &component;
    }
    REQUIRE(copy);
    CHECK(copy->reference == QStringLiteral("A2"));
    CHECK(copy->rotation == 90);
    CHECK(copy->parameters.value(QStringLiteral("gain")) == QStringLiteral("21"));
    // The anchor (centre of the copied items) lands on the cursor.
    INFO("pasted at " << copy->at.x() << "," << copy->at.y());
    CHECK(copy->at == QPoint(200, 100) + (QPoint(0, 0) - snapToGrid(QPointF(-15, 0))));
    CHECK(f.editor->selectedItemIds().size() == 2); // the pasted items are selected

    // Duplicate floats a copy; Escape cancels it.
    f.trigger("duplicate");
    CHECK(f.view()->isFloating());
    QTest::keyClick(f.viewport(), Qt::Key_Escape);
    CHECK_FALSE(f.view()->isFloating());
    CHECK(f.schematic().components().size() == 2);

    // Cut removes and paste brings back, under new ids.
    f.editor->selectItems({a1});
    f.trigger("cut");
    CHECK_FALSE(f.schematic().component(a1));
    f.trigger("paste");
    f.click(QPoint(-200, -100));
    CHECK(f.schematic().components().size() == 2);
}

TEST_CASE("components dropped from the library browser are placed")
{
    Fixture f;
    f.open();
    QMimeData mime;
    mime.setData(componentMimeType(), QByteArray("basic:resistor"));
    const QPoint target = f.at(QPoint(42, 58));
    QDragEnterEvent enter(target, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(f.viewport(), &enter);
    CHECK(enter.isAccepted());
    QDragMoveEvent move(target, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(f.viewport(), &move);
    QDropEvent drop(QPointF(target), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(f.viewport(), &drop);
    REQUIRE(f.schematic().components().size() == 1);
    CHECK(f.schematic().components().first().definition == QStringLiteral("basic:resistor"));
    CHECK(f.schematic().components().first().at == QPoint(40, 60));
}

TEST_CASE("net highlighting, labels and texts")
{
    Fixture f;
    const QString a1 = f.place(Amplifier, QPoint(0, 0));
    const QString a2 = f.place(Amplifier, QPoint(150, 0));
    const QString wire = f.schematic().addWire({f.pin(a1, QStringLiteral("out")), f.pin(a2, QStringLiteral("in"))});
    f.open();

    // Ctrl+click on a wire highlights its net and reports it.
    f.click(QPoint(75, 1), Qt::ControlModifier);
    const SheetNet &net = f.editor->connectivity().nets.at(f.editor->connectivity().netOfItem(wire));
    CHECK(f.editor->highlightedNet() == net.name);
    REQUIRE_FALSE(f.messages.isEmpty());
    CHECK(f.messages.last().contains(net.name));
    CHECK(f.messages.last().contains(QStringLiteral("2 pin")));
    CHECK(static_cast<SchematicItem *>(f.editor->scene()->item(wire))->isHighlighted());

    // Label tool: the prompt is replaced for the test.
    f.view()->setTextPrompt([](const QString &, const QString &, const QString &, const QStringList &, bool multi) {
        return std::optional<QString>(multi ? QStringLiteral("note") : QStringLiteral("IF"));
    });
    f.trigger("labelTool");
    f.click(QPoint(70, 0));
    REQUIRE(f.schematic().labels().size() == 1);
    CHECK(f.schematic().labels().first().net == QStringLiteral("IF"));
    CHECK(f.editor->highlightedNet() == QStringLiteral("IF")); // the net took the label's name
    f.trigger("textTool");
    f.click(QPoint(0, -100));
    REQUIRE(f.schematic().texts().size() == 1);

    // Double-click edits a label.
    f.trigger("selectTool");
    f.view()->setTextPrompt([](const QString &, const QString &, const QString &, const QStringList &, bool) {
        return std::optional<QString>(QStringLiteral("IF2"));
    });
    const QString label = f.schematic().labels().firstKey();
    QTest::mouseDClick(f.viewport(), Qt::LeftButton, Qt::NoModifier, f.at(QPoint(78, -7)));
    CHECK(f.schematic().labels().value(label).net == QStringLiteral("IF2"));

    // Escape clears the highlight.
    QTest::keyClick(f.viewport(), Qt::Key_Escape);
    CHECK(f.editor->highlightedNet().isEmpty());
}

TEST_CASE("unconnected pins, rule check marks and sheet blocks follow changes")
{
    Fixture f;
    QString error;
    Document *sub = f.project->createDocument(DocumentType::RfSchematic, QStringLiteral("sub"), &error);
    REQUIRE(sub);
    Schematic &subSheet = *sub->schematic();
    const DefinitionPtr port = f.project->resolve(subSheet, QStringLiteral("basic:sheet_port")).definition;
    const QString p1 = subSheet.addComponent(QStringLiteral("basic:sheet_port"), port.get(), QPoint(0, 0));
    subSheet.setParameter(p1, QStringLiteral("name"), QStringLiteral("in"));

    const QString a1 = f.place(Amplifier, QPoint(0, 0));
    const QString block = f.place(Project::sheetKey(QStringLiteral("sub")), QPoint(200, 0));
    f.open();
    ComponentItem *blockItem = f.editor->scene()->componentItem(block);
    REQUIRE(blockItem);
    CHECK(blockItem->pins().size() == 1);
    f.editor->connectivity();
    CHECK(f.editor->scene()->componentItem(a1)->unconnectedPins().size() == 2);

    // A second port in the sub-sheet adds a pin to the block.
    const QString p2 = subSheet.addComponent(QStringLiteral("basic:sheet_port"), port.get(), QPoint(300, 0));
    subSheet.setParameter(p2, QStringLiteral("name"), QStringLiteral("out"));
    CHECK(blockItem->pins().size() == 2);

    // Wiring a pin clears its unconnected marker after the debounced update.
    f.schematic().addWire({f.pin(a1, QStringLiteral("out")), f.pin(block, QStringLiteral("in"))});
    QTRY_VERIFY(f.editor->scene()->componentItem(a1)->unconnectedPins() == QSet<QString>{QStringLiteral("in")});

    // The rule check reports, marks and can zoom to items.
    QSignalSpy finished(f.editor.get(), &SchematicEditor::ruleCheckFinished);
    const QList<RuleViolation> violations = f.editor->runRuleCheck();
    CHECK(finished.count() == 1);
    CHECK_FALSE(violations.isEmpty());
    f.editor->zoomToItem(a1);
    CHECK(f.editor->selectedItemIds() == QStringList{a1});

    // Library reloads refresh component items.
    f.session.libraries()->reload();
    CHECK(f.editor->scene()->componentItem(a1)->resolution().definition);
}

TEST_CASE("a 2,000 component schematic loads and edits quickly")
{
    Fixture f;
    const DefinitionPtr amplifier = f.project->resolve(f.schematic(), Amplifier).definition;
    {
        // A grid of chains of four amplifiers, each wired to the next.
        Schematic::Edit edit(&f.schematic(), QStringLiteral("build"));
        for (int i = 0; i < 2000; ++i) {
            const QPoint at((i % 40) * 100, (i / 40) * 100);
            f.schematic().addComponent(Amplifier, amplifier.get(), at);
            if (i % 4 != 0)
                f.schematic().addWire({at - QPoint(70, 0), at - QPoint(30, 0)});
        }
    }
    QElapsedTimer timer;
    timer.start();
    f.open();
    const Connectivity &connectivity = f.editor->connectivity();
    const qint64 loadMs = timer.elapsed();
    CHECK(connectivity.nets.size() > 1000);

    timer.restart();
    f.editor->zoomToFit();
    f.editor->grab();
    const qint64 paintMs = timer.elapsed();

    const QString first = f.schematic().components().firstKey();
    f.editor->selectItems({first});
    timer.restart();
    f.view()->nudge(QPoint(0, 10));
    const qint64 editMs = timer.elapsed();
    timer.restart();
    f.editor->connectivity();
    const qint64 connectivityMs = timer.elapsed();

    WARN("2000 parts: load " << loadMs << " ms, paint " << paintMs << " ms, move edit " << editMs
                             << " ms, connectivity " << connectivityMs << " ms");
    CHECK(loadMs < 5000);
    CHECK(paintMs < 3000);
    CHECK(editMs < 1500);
}

#include "core/Library.h"
#include "core/Project.h"
#include "schematic_editor/SchematicExport.h"

#include <QFileInfo>
#include <QImage>

TEST_CASE("Schematics export to PDF, SVG and PNG with a title block")
{
    QTemporaryDir dir;
    Session session;
    session.libraries()->setSearchPath(LibraryScope::BuiltIn, QStringLiteral(LVD_SOURCE_TREE "/libraries"));
    QString error;
    Project *project = session.openProject(QStringLiteral(LVD_SOURCE_TREE "/examples/lowpass_filter/lowpass_filter.lvdproj"), &error);
    INFO(error.toStdString());
    REQUIRE(project);
    Document *document = nullptr;
    for (Document *d : project->documents())
        if (d->isSchematic())
            document = d;
    REQUIRE(document);

    document->schematic()->setTitleBlock({{QStringLiteral("revision"), QStringLiteral("B")},
                                          {QStringLiteral("author"), QStringLiteral("  ")}});
    CHECK(document->schematic()->titleBlock().value(QStringLiteral("revision")).toString() == QLatin1String("B"));
    CHECK_FALSE(document->schematic()->titleBlock().contains(QStringLiteral("author"))); // blank fields dropped
    const QStringList lines = SchematicExport::titleBlockLines(project, document);
    REQUIRE(lines.size() >= 4);
    CHECK(lines.first() == document->name());
    CHECK(lines.join(QLatin1Char('\n')).contains(QStringLiteral("Revision: B")));
    CHECK(lines.join(QLatin1Char('\n')).contains(QStringLiteral(LVD_VERSION)));
    // The title block is part of the document and undoable.
    CHECK(document->isModified());
    document->schematic()->undoStack()->undo();
    CHECK(document->schematic()->titleBlock().isEmpty());

    for (const char *name : {"sheet.pdf", "sheet.svg", "sheet.png"}) {
        const QString path = dir.filePath(QString::fromLatin1(name));
        INFO(name);
        REQUIRE(SchematicExport::exportFile(project, document, path, &error));
        CHECK(QFileInfo(path).size() > 1000);
    }
    QFile pdf(dir.filePath(QStringLiteral("sheet.pdf")));
    REQUIRE(pdf.open(QIODevice::ReadOnly));
    CHECK(pdf.read(5) == "%PDF-");
    // The PNG is white around a drawing in dark ink.
    const QImage png(dir.filePath(QStringLiteral("sheet.png")));
    REQUIRE_FALSE(png.isNull());
    CHECK(png.pixelColor(0, 0) == QColor(Qt::white));
    int dark = 0;
    for (int y = 0; y < png.height(); y += 3)
        for (int x = 0; x < png.width(); x += 3)
            dark += png.pixelColor(x, y).lightness() < 128;
    CHECK(dark > 50);
    CHECK_FALSE(SchematicExport::exportFile(project, document, dir.filePath(QStringLiteral("sheet.doc")), &error));
    if (const QByteArray keep = qgetenv("LVD_EXPORT_SAMPLE"); !keep.isEmpty())
        SchematicExport::exportFile(project, document, QString::fromLocal8Bit(keep), &error);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("schematic_editor_tests"));
    return Catch::Session().run(argc, argv);
}

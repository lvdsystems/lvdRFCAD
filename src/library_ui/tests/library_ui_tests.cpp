#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Document.h"
#include "core/Library.h"
#include "core/Project.h"
#include "library_ui/LibraryBrowser.h"
#include "library_ui/LibraryManagerDialog.h"
#include "library_ui/LibraryTargetDialog.h"
#include "library_ui/SymbolCanvas.h"
#include "library_ui/SymbolEditor.h"

#include <QApplication>
#include <QFileInfo>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>

using namespace lvd;

namespace {

// Built-in libraries from the source tree (read-only) and an empty user folder.
struct Libraries {
    QTemporaryDir userDir;
    LibraryManager manager;

    Libraries()
    {
        manager.setSearchPath(LibraryScope::BuiltIn, LibraryManager::defaultBuiltInPath());
        manager.setSearchPath(LibraryScope::User, userDir.path());
    }
};

bool containsProblem(const SymbolEditor &editor, const QString &text)
{
    for (const QString &problem : editor.problemTexts()) {
        if (problem.contains(text, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

void showAndSettle(QWidget &widget, QSize size)
{
    widget.resize(size);
    widget.show();
    QApplication::processEvents();
}

} // namespace

TEST_CASE("the browser lists the built-in components by library and category")
{
    Libraries libraries;
    REQUIRE(libraries.manager.warnings().isEmpty());
    LibraryBrowser browser(&libraries.manager);

    const QStringList keys = browser.componentKeys();
    REQUIRE(keys.size() == 125);
    for (const QString &key : keys) {
        const DefinitionPtr definition = libraries.manager.find(key);
        REQUIRE(definition);
        QString libraryName, id;
        REQUIRE(LibraryManager::splitKey(key, &libraryName, &id));
        CHECK(browser.componentPath(key) == libraryName + QLatin1Char('/') + definition->category + QLatin1Char('/') + definition->name);
    }
    CHECK(browser.componentPath(QStringLiteral("rf-system:mixer")) == QStringLiteral("rf-system/RF/Mixers/Mixer"));
    CHECK(browser.componentPath(QStringLiteral("basic:resistor")) == QStringLiteral("basic/Passive/Resistor"));
    CHECK(browser.componentPath(QStringLiteral("rf-circuit:tline"))
          == QStringLiteral("rf-circuit/Transmission lines/Ideal/Transmission line"));

    // The selection survives a reload.
    REQUIRE(browser.selectComponent(QStringLiteral("rf-system:mixer")));
    libraries.manager.reload();
    CHECK(browser.selectedKey() == QStringLiteral("rf-system:mixer"));
}

TEST_CASE("the browser search filters on name, id, category, description and parameters")
{
    Libraries libraries;
    LibraryBrowser browser(&libraries.manager);

    browser.setFilterText(QStringLiteral("MIXER"));
    QStringList visible = browser.visibleComponentKeys();
    CHECK(visible.contains(QStringLiteral("rf-system:mixer")));
    CHECK(!visible.contains(QStringLiteral("basic:resistor")));

    browser.setFilterText(QStringLiteral("lo_power")); // a parameter name
    CHECK(browser.visibleComponentKeys() == QStringList{QStringLiteral("rf-system:mixer")});
    // Matches are expanded.
    REQUIRE(browser.selectComponent(QStringLiteral("rf-system:mixer")));
    const QModelIndex mixer = browser.treeView()->currentIndex();
    CHECK(browser.treeView()->isExpanded(mixer.parent()));
    CHECK(browser.treeView()->isExpanded(mixer.parent().parent()));

    browser.setFilterText(QStringLiteral("transmission lines")); // every word must match
    visible = browser.visibleComponentKeys();
    CHECK(visible.size() >= 8);
    CHECK(visible.contains(QStringLiteral("rf-circuit:cpw")));
    CHECK(!visible.contains(QStringLiteral("rf-system:mixer")));

    browser.setFilterText(QStringLiteral("no such thing"));
    CHECK(browser.visibleComponentKeys().isEmpty());

    browser.setFilterText(QString());
    CHECK(browser.visibleComponentKeys().size() == 125);
}

TEST_CASE("the browser places components by double-click, Enter and drag")
{
    Libraries libraries;
    LibraryBrowser browser(&libraries.manager);
    showAndSettle(browser, QSize(360, 700));

    REQUIRE(browser.selectComponent(QStringLiteral("rf-system:amplifier")));
    QTreeView *view = browser.treeView();
    const QModelIndex index = view->currentIndex();
    view->scrollTo(index);
    QApplication::processEvents();

    QSignalSpy activated(&browser, &LibraryBrowser::componentActivated);
    // QTest sends only the double-click event to widgets; the first click comes before it.
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->visualRect(index).center());
    CHECK(activated.isEmpty());
    QTest::mouseDClick(view->viewport(), Qt::LeftButton, {}, view->visualRect(index).center());
    QTest::mouseRelease(view->viewport(), Qt::LeftButton, {}, view->visualRect(index).center());
    REQUIRE(activated.count() == 1);
    CHECK(activated.at(0).at(0).toString() == QStringLiteral("rf-system:amplifier"));

    QTest::keyClick(view, Qt::Key_Return);
    REQUIRE(activated.count() == 2);

    // Drag payload: the definition key as UTF-8 under the component MIME type.
    CHECK(index.flags().testFlag(Qt::ItemIsDragEnabled));
    std::unique_ptr<QMimeData> mime(view->model()->mimeData({index}));
    REQUIRE(mime);
    REQUIRE(mime->hasFormat(QStringLiteral("application/x-lvdrfcad-component")));
    CHECK(QString::fromUtf8(mime->data(LibraryBrowser::mimeType())) == QStringLiteral("rf-system:amplifier"));

    // Libraries and categories are not placeable.
    CHECK(!index.parent().flags().testFlag(Qt::ItemIsDragEnabled));
    QSignalSpy edit(&browser, &LibraryBrowser::editComponentRequested);
    CHECK(edit.isValid());
}

TEST_CASE("the browser offers the project's schematics as sheets")
{
    Libraries libraries;
    QTemporaryDir projectDir;
    QString error;
    std::unique_ptr<Project> project = Project::create(projectDir.filePath(QStringLiteral("demo")), QStringLiteral("demo"), &error);
    REQUIRE(project);
    project->setLibraryManager(&libraries.manager);

    LibraryBrowser browser(&libraries.manager);
    browser.setProject(project.get());
    CHECK(browser.componentKeys().size() == 125);

    Document *amp = project->createDocument(DocumentType::RfSchematic, QStringLiteral("amp"), &error);
    REQUIRE(amp);
    CHECK(browser.componentKeys().contains(QStringLiteral("sheet:amp")));
    CHECK(browser.componentPath(QStringLiteral("sheet:amp")) == QStringLiteral("sheet/amp"));

    REQUIRE(project->createDocument(DocumentType::SpiceSchematic, QStringLiteral("bias"), &error));
    REQUIRE(project->createDocument(DocumentType::AntennaArray, QStringLiteral("patch"), &error));
    QStringList keys = browser.componentKeys();
    CHECK(keys.contains(QStringLiteral("sheet:bias")));
    CHECK(!keys.contains(QStringLiteral("sheet:patch"))); // not a schematic
    CHECK(keys.size() == 127);

    // Sheets are placeable like components.
    REQUIRE(browser.selectComponent(QStringLiteral("sheet:amp")));
    CHECK(browser.selectedKey() == QStringLiteral("sheet:amp"));
    browser.setFilterText(QStringLiteral("amp"));
    CHECK(browser.visibleComponentKeys().contains(QStringLiteral("sheet:amp")));
    browser.setFilterText(QString());

    REQUIRE(project->removeDocument(amp, true, &error));
    keys = browser.componentKeys();
    CHECK(!keys.contains(QStringLiteral("sheet:amp")));
    CHECK(keys.contains(QStringLiteral("sheet:bias")));

    // Closing the project removes the sheets.
    project.reset();
    CHECK(browser.componentKeys().size() == 125);
}

TEST_CASE("the symbol editor creates a component through the API and the mouse")
{
    Libraries libraries;
    LibraryBrowser browser(&libraries.manager);
    SymbolEditor editor(&libraries.manager);
    showAndSettle(editor, QSize(1200, 800));

    editor.newComponent(QStringLiteral("test-parts"), LibraryScope::User);
    CHECK(editor.isModified());
    CHECK(editor.problemTexts().isEmpty()); // the starting template is valid
    CHECK(editor.definitionKey() == QStringLiteral("test-parts:new_component"));

    ComponentDefinition definition = editor.definition();
    definition.id = QStringLiteral("lna");
    definition.name = QStringLiteral("Low-noise amplifier");
    definition.category = QStringLiteral("RF/Amplifiers");
    definition.referencePrefix = QStringLiteral("A");
    ParameterDefinition gain;
    gain.name = QStringLiteral("gain");
    gain.unit = QStringLiteral("dB");
    gain.defaultValue = QStringLiteral("15");
    gain.shown = true;
    ParameterDefinition mode;
    mode.name = QStringLiteral("mode");
    mode.choices = {QStringLiteral("low"), QStringLiteral("high")};
    mode.defaultValue = QStringLiteral("low");
    definition.parameters = {gain, mode};
    definition.models = {ModelDefinition{QStringLiteral("rf-behavioural"),
                                         QJsonObject{{QStringLiteral("kind"), QStringLiteral("rf-behavioural")},
                                                     {QStringLiteral("block"), QStringLiteral("amplifier")}}}};
    editor.setDefinition(definition);
    CHECK(editor.canSave());

    // Mouse input on the canvas: a pin, a rectangle and a two-point line.
    SymbolCanvas *canvas = editor.canvas();
    canvas->setTool(SymbolCanvas::Tool::Pin);
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, {}, canvas->mapFromSymbol(QPointF(0, -40)));
    ComponentDefinition current = editor.definition();
    REQUIRE(current.pins.size() == 3);
    const SymbolPin *bias = current.symbol.pin(current.pins.last());
    REQUIRE(bias);
    CHECK(bias->at == QPoint(0, -40));
    CHECK(bias->direction == PinDirection::Down); // toward the body

    canvas->setTool(SymbolCanvas::Tool::Rect);
    const int graphicsBefore = static_cast<int>(current.symbol.graphics.size());
    QTest::mousePress(canvas->viewport(), Qt::LeftButton, {}, canvas->mapFromSymbol(QPointF(-10, -10)));
    QTest::mouseMove(canvas->viewport(), canvas->mapFromSymbol(QPointF(0, 0)));
    QTest::mouseMove(canvas->viewport(), canvas->mapFromSymbol(QPointF(10, 10)));
    QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, {}, canvas->mapFromSymbol(QPointF(10, 10)));
    current = editor.definition();
    REQUIRE(current.symbol.graphics.size() == graphicsBefore + 1);
    CHECK(current.symbol.graphics.last().type == SymbolGraphic::Type::Rect);
    CHECK(current.symbol.graphics.last().from == QPoint(-10, -10));
    CHECK(current.symbol.graphics.last().to == QPoint(10, 10));

    canvas->setTool(SymbolCanvas::Tool::Line);
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, {}, canvas->mapFromSymbol(QPointF(-15, 25)));
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, {}, canvas->mapFromSymbol(QPointF(15, 25)));
    QTest::keyClick(canvas, Qt::Key_Return);
    current = editor.definition();
    REQUIRE(current.symbol.graphics.size() == graphicsBefore + 2);
    CHECK(current.symbol.graphics.last().type == SymbolGraphic::Type::Line);
    CHECK(current.symbol.graphics.last().points == QList<QPoint>{QPoint(-15, 25), QPoint(15, 25)});

    // Validation blocks invalid definitions.
    definition = editor.definition();
    definition.id = QStringLiteral("Bad Id");
    editor.setDefinition(definition);
    CHECK(!editor.canSave());
    CHECK(containsProblem(editor, QStringLiteral("id")));
    QString error;
    CHECK(!editor.save(&error));
    CHECK(!QFileInfo::exists(libraries.userDir.filePath(QStringLiteral("test-parts.lvdlib"))));

    definition.id = QStringLiteral("lna");
    definition.parameters[0].defaultValue = QStringLiteral("15 +");
    editor.setDefinition(definition);
    CHECK(containsProblem(editor, QStringLiteral("not a valid expression")));
    definition.parameters[0].defaultValue = QStringLiteral("15");
    definition.symbol.pins[0].at = QPoint(-35, 0); // off the grid
    editor.setDefinition(definition);
    CHECK(containsProblem(editor, QStringLiteral("10-unit grid")));
    definition.symbol.pins[0].at = QPoint(-30, 0);
    editor.setDefinition(definition);
    REQUIRE(editor.canSave());

    // Saving creates the user library; the browser shows the new component.
    QSignalSpy saved(&editor, &SymbolEditor::componentSaved);
    error.clear();
    REQUIRE(editor.save(&error));
    CHECK(error.isEmpty());
    CHECK(saved.count() == 1);
    CHECK(!editor.isModified());
    CHECK(QFileInfo::exists(libraries.userDir.filePath(QStringLiteral("test-parts.lvdlib"))));
    const DefinitionPtr stored = libraries.manager.find(QStringLiteral("test-parts:lna"));
    REQUIRE(stored);
    CHECK(*stored == editor.definition());
    CHECK(browser.componentKeys().contains(QStringLiteral("test-parts:lna")));
    CHECK(browser.componentPath(QStringLiteral("test-parts:lna")) == QStringLiteral("test-parts/RF/Amplifiers/Low-noise amplifier"));

    // A user component opens editable, and a changed id renames it.
    SymbolEditor second(&libraries.manager);
    REQUIRE(second.openComponent(QStringLiteral("test-parts:lna")));
    CHECK(!second.isReadOnlySource());
    CHECK(!second.isModified());
    definition = second.definition();
    definition.id = QStringLiteral("lna2");
    second.setDefinition(definition);
    REQUIRE(second.save(&error));
    CHECK(libraries.manager.find(QStringLiteral("test-parts:lna2")));
    CHECK(!libraries.manager.find(QStringLiteral("test-parts:lna")));
}

TEST_CASE("the symbol editor undoes and redoes edits")
{
    Libraries libraries;
    SymbolEditor editor(&libraries.manager);
    showAndSettle(editor, QSize(1200, 800));
    editor.newComponent(QStringLiteral("undo-test"), LibraryScope::User);
    const ComponentDefinition initial = editor.definition();
    QUndoStack *undo = editor.undoStack();
    CHECK(undo->count() == 0);

    SymbolCanvas *canvas = editor.canvas();
    canvas->selectAll();
    canvas->deleteSelection();
    CHECK(editor.definition().symbol.graphics.isEmpty());
    CHECK(editor.definition().pins.isEmpty()); // deleting symbol pins removes the component pins
    undo->undo();
    CHECK(editor.definition() == initial);
    undo->redo();
    CHECK(editor.definition().pins.isEmpty());
    undo->undo();
    CHECK(editor.definition() == initial);

    // Drag the rectangle by its top edge.
    canvas->setTool(SymbolCanvas::Tool::Select);
    canvas->setSelection({});
    QTest::mousePress(canvas->viewport(), Qt::LeftButton, {}, canvas->mapFromSymbol(QPointF(0, -20)));
    QTest::mouseMove(canvas->viewport(), canvas->mapFromSymbol(QPointF(0, -15)));
    QTest::mouseMove(canvas->viewport(), canvas->mapFromSymbol(QPointF(0, -10)));
    QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, {}, canvas->mapFromSymbol(QPointF(0, -10)));
    REQUIRE(editor.definition().symbol.graphics.size() == 1);
    CHECK(editor.definition().symbol.graphics.first().from == QPoint(-20, -10));
    CHECK(canvas->selection() == QList<ElementRef>{ElementRef{ElementRef::Kind::Graphic, 0}});
    const int steps = undo->count();
    undo->undo();
    CHECK(editor.definition() == initial);
    undo->redo();
    CHECK(editor.definition().symbol.graphics.first().from == QPoint(-20, -10));
    CHECK(undo->count() == steps);

    // Duplicate, then undo back to the start.
    canvas->duplicateSelection();
    CHECK(editor.definition().symbol.graphics.size() == 2);
    while (undo->canUndo())
        undo->undo();
    CHECK(editor.definition() == initial);
    CHECK(undo->isClean());
}

TEST_CASE("built-in components are read-only and are saved as a copy")
{
    Libraries libraries;
    SymbolEditor editor(&libraries.manager);
    REQUIRE(editor.openComponent(QStringLiteral("rf-system:mixer")));
    CHECK(editor.isReadOnlySource());
    CHECK(editor.definitionKey().isEmpty());
    CHECK(editor.canSave());
    QString error;
    CHECK(!editor.save(&error));
    CHECK(!error.isEmpty());

    // Using the built-in name in the user scope is allowed but hides the whole library.
    QString warning;
    CHECK(LibraryTargetDialog::checkNewLibrary(libraries.manager, QStringLiteral("rf-system"), LibraryScope::User, &error,
                                               &warning));
    CHECK(!warning.isEmpty());

    ComponentDefinition definition = editor.definition();
    definition.name = QStringLiteral("My mixer");
    editor.setDefinition(definition);
    REQUIRE(editor.saveCopy(QStringLiteral("rf-system-custom"), LibraryScope::User, &error));
    CHECK(!editor.isReadOnlySource());
    CHECK(editor.definitionKey() == QStringLiteral("rf-system-custom:mixer"));
    const DefinitionPtr copy = libraries.manager.find(QStringLiteral("rf-system-custom:mixer"));
    REQUIRE(copy);
    CHECK(copy->name == QStringLiteral("My mixer"));
    CHECK(libraries.manager.find(QStringLiteral("rf-system:mixer"))->name == QStringLiteral("Mixer"));
    CHECK(libraries.manager.scopeOf(QStringLiteral("rf-system")) == LibraryScope::BuiltIn);
    CHECK(!editor.saveCopy(QStringLiteral("rf-system"), LibraryScope::BuiltIn, &error));
}

TEST_CASE("the library manager dialog lists and creates libraries")
{
    Libraries libraries;
    LibraryManagerDialog dialog(&libraries.manager);
    CHECK(dialog.libraryCount() == 8);

    QString error;
    REQUIRE(dialog.createLibrary(QStringLiteral("my-parts"), LibraryScope::User, QStringLiteral("Mine"), &error));
    CHECK(dialog.libraryCount() == 9);
    REQUIRE(libraries.manager.library(QStringLiteral("my-parts")));
    CHECK(libraries.manager.library(QStringLiteral("my-parts"))->description == QStringLiteral("Mine"));
    CHECK(!dialog.createLibrary(QStringLiteral("my-parts"), LibraryScope::User, {}, &error));
    CHECK(!dialog.createLibrary(QStringLiteral("bad:name"), LibraryScope::User, {}, &error));
    CHECK(!dialog.createLibrary(QStringLiteral("project-lib"), LibraryScope::Project, {}, &error)); // no project

    QTemporaryDir otherDir;
    QSignalSpy changed(&dialog, &LibraryManagerDialog::userLibraryPathChanged);
    dialog.setUserLibraryPath(otherDir.path());
    REQUIRE(changed.count() == 1);
    CHECK(libraries.manager.searchPath(LibraryScope::User) == QDir::cleanPath(otherDir.path()));
    CHECK(dialog.libraryCount() == 8);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Keep test runs away from the real application's stored settings.
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("library_ui_tests"));
    return Catch::Session().run(argc, argv);
}

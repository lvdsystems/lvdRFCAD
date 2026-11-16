// Antenna workspaces: editing, undo, background results, library entries,
// element documents and pattern import.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "antenna/AntennaLibrary.h"
#include "antenna/PatternFile.h"
#include "antenna_ui/AntennaLibraryPanel.h"
#include "antenna_ui/ArrayWorkspace.h"
#include "antenna_ui/ElementWorkspace.h"
#include "antenna_ui/PositionEditor.h"
#include "core/Library.h"
#include "core/Project.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTest>
#include <QTreeWidget>

using namespace lvd;
using namespace lvd::ant;
using Catch::Approx;

namespace {

struct Fixture {
    QTemporaryDir dir;
    std::unique_ptr<Project> project;
    LibraryManager components;
    std::unique_ptr<AntennaLibraryManager> antennas;

    Fixture()
    {
        components.setSearchPath(LibraryScope::BuiltIn, QStringLiteral(LVD_SOURCE_DIR "/libraries"));
        antennas = std::make_unique<AntennaLibraryManager>(&components);
        QString error;
        project = Project::create(dir.path(), QStringLiteral("ant"), &error);
        REQUIRE(project);
    }
};

} // namespace

TEST_CASE("The built-in antenna libraries load")
{
    Fixture fx;
    REQUIRE(fx.antennas->libraries().size() == 4);
    for (const char *key : {"antenna-elements:patch-x-ro4003", "antenna-patterns:sample-horn-x", "antenna-tr-modules:x-gan-10w",
                            "antenna-arrays:aesa-circular-tri"})
        CHECK(fx.antennas->find(QString::fromLatin1(key)).entry);
    antui::AntennaLibraryPanel panel(fx.antennas.get());
    CHECK(panel.entryCount() >= 28);

    // Every element and pattern entry resolves to a usable element.
    for (const AntennaLibrary &lib : fx.antennas->libraries())
        for (const AntennaLibraryEntry &e : lib.entries) {
            if (e.kind != AntennaEntryKind::Element && e.kind != AntennaEntryKind::Pattern)
                continue;
            ArrayDesign d;
            d.elementRef = QStringLiteral("library:") + AntennaLibraryManager::makeKey(lib.name, e.id);
            const ElementResolution r = resolveElement(d, fx.project.get(), fx.antennas.get());
            INFO(e.id.toStdString() << ": " << r.error.toStdString());
            REQUIRE(r.pattern);
            CHECK(r.pattern->peakDirectivityDbi(r.pattern->spec().frequency) > -1);
        }
}

TEST_CASE("The array workspace computes, edits and undoes")
{
    Fixture fx;
    QString error;
    Document *doc = fx.project->createDocument(DocumentType::AntennaArray, QStringLiteral("array"), &error);
    REQUIRE(doc);
    antui::ArrayWorkspace workspace(fx.project.get(), doc, fx.antennas.get());
    workspace.resize(1300, 800);
    REQUIRE(workspace.waitForResults());
    const antui::ArrayResults *r = workspace.results();
    REQUIRE(r);
    CHECK(r->error.isEmpty());
    CHECK(r->evaluation.layout.elements.size() == 64);
    CHECK(r->metrics.peakGainDbi > 18);
    CHECK(r->metrics.peakThetaDeg == Approx(0).margin(0.05));
    CHECK_FALSE(r->scan.empty());
    CHECK_FALSE(r->band.empty());
    CHECK(r->uv.size > 100);
    CHECK(std::isfinite(r->budget.eirpDbw));

    // An edit through the document (as Python does) reaches the workspace.
    ArrayDesign d = workspace.design();
    d.excitation.steerThetaDeg = 30;
    d.geometry.nx = 12;
    doc->setContent(d.toJson());
    REQUIRE(workspace.waitForResults());
    CHECK(workspace.design().excitation.steerThetaDeg == 30);
    CHECK(workspace.results()->evaluation.layout.elements.size() == 96);
    CHECK(workspace.results()->metrics.peakThetaDeg == Approx(30).margin(0.3));

    // A library preset is an undoable edit.
    workspace.applyLibraryEntry(QStringLiteral("antenna-arrays:ura-16x16-taylor"),
                                *fx.antennas->find(QStringLiteral("antenna-arrays:ura-16x16-taylor")).entry);
    REQUIRE(workspace.waitForResults());
    CHECK(workspace.results()->evaluation.layout.elements.size() == 256);
    CHECK(workspace.results()->metrics.sideLobe1Db < -28);
    QAction *undo = workspace.editActions().front();
    REQUIRE(undo->isEnabled());
    undo->trigger();
    REQUIRE(workspace.waitForResults());
    CHECK(workspace.design().geometry.nx == 12);
    CHECK(ArrayDesign::fromJson(doc->content()).geometry.nx == 12);

    // T/R preset sets the AESA quantisation; the ideal reference is computed.
    workspace.applyLibraryEntry(QStringLiteral("antenna-tr-modules:x-gan-10w"),
                                *fx.antennas->find(QStringLiteral("antenna-tr-modules:x-gan-10w")).entry);
    REQUIRE(workspace.waitForResults());
    CHECK(workspace.design().aesa.phaseBits == 6);
    CHECK_FALSE(workspace.results()->idealX.fields.empty());
    CHECK(workspace.results()->tx.origin.contains(QStringLiteral("X-band GaN")));

    // Synthesis turns the taper into custom weights.
    d = workspace.design();
    d.synthesis.sllDb = -25;
    doc->setContent(d.toJson());
    workspace.synthesise();
    CHECK(workspace.design().excitation.x.kind == TaperKind::Custom);
    CHECK(workspace.design().excitation.weights.size() == 96);
    REQUIRE(workspace.waitForResults());

    for (const char *view : {"cuts", "uv", "3d", "layout", "scan", "summary"}) {
        workspace.showView(QString::fromLatin1(view));
        QApplication::processEvents();
    }
}

TEST_CASE("Arrays use element documents and linked T/R values fall back cleanly")
{
    Fixture fx;
    QString error;
    Document *elementDoc = fx.project->createDocument(DocumentType::AntennaElement, QStringLiteral("patch"), &error);
    REQUIRE(elementDoc);
    antui::ElementWorkspace elementWorkspace(fx.project.get(), elementDoc, fx.antennas.get());
    elementWorkspace.applyLibraryEntry(QStringLiteral("antenna-elements:patch-x-ro4003"),
                                       *fx.antennas->find(QStringLiteral("antenna-elements:patch-x-ro4003")).entry);
    elementWorkspace.recompute();
    CHECK(elementWorkspace.design().element.model == ElementModelKind::Patch);
    CHECK(elementWorkspace.results().directivityDbi > 6);
    CHECK(std::isfinite(elementWorkspace.results().beamwidthEDeg));

    Document *arrayDoc = fx.project->createDocument(DocumentType::AntennaArray, QStringLiteral("array"), &error);
    ArrayDesign d;
    d.elementRef = QStringLiteral("element:patch");
    d.trModule.rx.source = QStringLiteral("linked");
    d.trModule.rx.document = QStringLiteral("missing_chain");
    arrayDoc->setContent(d.toJson());
    antui::ArrayWorkspace workspace(fx.project.get(), arrayDoc, fx.antennas.get());
    REQUIRE(workspace.waitForResults());
    CHECK(workspace.results()->error.isEmpty());
    CHECK(workspace.results()->warnings.join(QLatin1Char(' ')).contains(QStringLiteral("missing_chain")));
    CHECK(workspace.results()->metrics.peakGainDbi > 20);
}

TEST_CASE("Element documents import pattern files into the project")
{
    Fixture fx;
    QString error;
    Document *doc = fx.project->createDocument(DocumentType::AntennaElement, QStringLiteral("horn"), &error);
    antui::ElementWorkspace workspace(fx.project.get(), doc, fx.antennas.get());
    const QString source = QStringLiteral(LVD_SOURCE_DIR "/libraries/patterns/sample-horn-x.csv");
    REQUIRE(workspace.importPattern(source, &error));
    workspace.recompute();
    CHECK(workspace.design().element.model == ElementModelKind::Imported);
    CHECK(workspace.design().element.patternFile == QStringLiteral("patterns/sample-horn-x.csv"));
    CHECK(QFileInfo::exists(QDir(fx.project->directory()).filePath(QStringLiteral("patterns/sample-horn-x.csv"))));
    CHECK(workspace.results().directivityDbi > 13);
    CHECK(workspace.results().directivityDbi < 17);
    QAction *undo = workspace.editActions().front();
    undo->trigger();
    CHECK(workspace.design().element.model != ElementModelKind::Imported);
}


TEST_CASE("The position editor edits elements in the table and on the canvas")
{
    GeometrySpec g;
    g.kind = LatticeKind::Rectangular;
    g.nx = 4;
    g.ny = 3;
    g.dx = g.dy = 0.5;
    antui::PositionEditor editor(g, 10e9);
    editor.resize(1100, 700);
    editor.show();
    QApplication::processEvents();
    antui::LayoutCanvas *canvas = editor.canvas();
    REQUIRE(editor.elements().size() == 12);
    CHECK(editor.table()->rowCount() == 12);
    CHECK(editor.elements()[0].x == Approx(-0.75));
    canvas->fitView();
    QApplication::processEvents();

    // Double-click on empty space adds an element on the grid (0.25 wavelength).
    const QPointF empty = canvas->toPixel(QPointF(2.0, 2.0));
    QTest::mouseDClick(canvas, Qt::LeftButton, Qt::NoModifier, empty.toPoint());
    REQUIRE(editor.elements().size() == 13);
    CHECK(editor.elements().back().x == Approx(2.0).margin(1e-9));
    CHECK(editor.elements().back().y == Approx(2.0).margin(1e-9));
    CHECK(editor.table()->rowCount() == 13);

    // Dragging an element moves it to a grid point.
    const QPoint from = canvas->elementPixel(0).toPoint();
    const QPoint to = canvas->toPixel(QPointF(-1.5, -1.0)).toPoint() + QPoint(2, -1);
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, from);
    QMouseEvent move(QEvent::MouseMove, QPointF(to), canvas->mapToGlobal(QPointF(to)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, to);
    CHECK(editor.elements()[0].x == Approx(-1.5).margin(1e-9));
    CHECK(editor.elements()[0].y == Approx(-1.0).margin(1e-9));
    CHECK(editor.table()->item(0, 1)->text().toDouble() == Approx(-1.5));

    // Rubber band over the top row (y = 0.5) selects four elements.
    const QPoint a = canvas->toPixel(QPointF(-1.0, 0.7)).toPoint(), b = canvas->toPixel(QPointF(1.0, 0.3)).toPoint();
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, a);
    QMouseEvent band(QEvent::MouseMove, QPointF(b), canvas->mapToGlobal(QPointF(b)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &band);
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, b);
    CHECK(canvas->selection().size() == 4);

    editor.toggleSelected();
    CHECK(std::count_if(editor.elements().begin(), editor.elements().end(), [](const CustomElement &e) { return !e.enabled; }) == 4);
    editor.deleteSelected();
    CHECK(editor.elements().size() == 9);

    // Rotating one element by 90 degrees about z turns its polarisation.
    canvas->setSelection({0});
    editor.rotateSelected(90);
    CHECK(editor.elements()[0].rotationDeg == Approx(90).margin(1e-9));
    editor.mirrorSelected(true);
    CHECK(editor.elements()[0].nz == Approx(1));

    // Editing the table moves the element; undo and redo step through it all.
    editor.table()->item(1, 3)->setText(QStringLiteral("0.4")); // z
    CHECK(editor.elements()[1].z == Approx(0.4));
    for (int i = 0; i < 20; ++i)
        editor.undo();
    CHECK(editor.elements().size() == 12);
    CHECK(editor.elements()[0].x == Approx(-0.75));
    editor.redo();
    CHECK(editor.elements().size() == 13);

    // CSV round trip.
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("e.csv"));
    QString error;
    REQUIRE(editor.exportCsv(path, &error));
    antui::PositionEditor other(g, 10e9);
    REQUIRE(other.importCsv(path, &error));
    REQUIRE(other.elements().size() == editor.elements().size());
    CHECK(other.elements().back().x == Approx(editor.elements().back().x));
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("antenna_ui_tests"));
    return Catch::Session().run(argc, argv);
}

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Schematic.h"
#include "core/Session.h"
#include "matching/MatchingWindow.h"
#include "plot/PlotWidget.h"
#include "rfdata/Touchstone.h"

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <cmath>

using namespace lvd;
using namespace lvd::rf;

TEST_CASE("the matching window computes, shows and inserts solutions")
{
    QTemporaryDir dir;
    Session session;
    QString error;
    Project *project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
    REQUIRE(project);
    Document *document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("dut"), &error);
    REQUIRE(document);

    MatchingWindow window;
    window.resize(1200, 800);
    window.show();
    QSignalSpy status(&window, &MatchingWindow::statusMessage);

    window.setImpedances({50, 0}, {12, -30}, 2.4e9);
    REQUIRE(window.result().ok());
    CHECK(window.result().solutions.size() == 4);
    CHECK(window.selectedSolution() == 0);
    auto *tree = window.findChild<QTreeWidget *>();
    REQUIRE(tree);
    CHECK(tree->topLevelItemCount() == 4);
    CHECK_FALSE(window.smithPlot()->traceIds().empty());
    CHECK_FALSE(window.responsePlot()->traceIds().empty());

    // Insertion needs a target.
    CHECK_FALSE(window.canInsert());
    CHECK_FALSE(window.insertSelected());
    window.setTarget(project, document);
    CHECK(window.canInsert());
    window.selectSolution(1);
    CHECK(window.selectedSolution() == 1);
    QSignalSpy inserted(&window, &MatchingWindow::networkInserted);
    REQUIRE(window.insertSelected(&error));
    CHECK(inserted.count() == 1);
    CHECK(document->schematic()->components().size() >= 2);
    CHECK(document->schematic()->labels().size() == 2);
    // A second insertion goes below the first with new labels.
    REQUIRE(window.insertSelected(&error));
    CHECK(inserted.last().at(0).toString() == QStringLiteral("MATCH_IN_2"));
    CHECK(document->schematic()->labels().size() == 4);
    CHECK(status.count() > 0);

    // Topologies and options.
    window.setTopology(MatchTopology::PiNetwork);
    window.setLoadedQ(0.5);
    CHECK_FALSE(window.result().ok());
    CHECK(window.message().contains(QStringLiteral("minimum")));
    CHECK(window.selectedSolution() == -1);
    CHECK_FALSE(window.canInsert());
    window.setLoadedQ(5);
    CHECK(window.result().ok());
    CHECK(window.result().solutions.size() == 4);

    window.setTopology(MatchTopology::SingleStub);
    CHECK(window.result().ok());
    CHECK(window.result().solutions.size() == 4);
    window.setTopology(MatchTopology::DoubleStub);
    window.setStubSpacing(45);
    window.setTopology(MatchTopology::QuarterWave);
    CHECK(window.result().ok());
    window.setTopology(MatchTopology::MultiSection);
    CHECK_FALSE(window.result().ok()); // complex load

    window.setTopology(MatchTopology::LNetwork);
    window.setStandardSeries(ESeries::E12);
    REQUIRE(window.result().ok());
    for (const MatchSolution &s : window.result().solutions) {
        CHECK(s.snapped);
        CHECK(s.mismatch > 0);
    }
    CHECK(window.idealSolutions().size() == window.result().solutions.size());
    window.setStandardSeries(std::nullopt);
    CHECK_FALSE(window.result().solutions.front().snapped);

    // Bad input is reported, not computed.
    auto *frequency = window.findChild<QLineEdit *>(QStringLiteral("frequency"));
    REQUIRE(frequency);
    frequency->setText(QStringLiteral("abc"));
    window.recompute();
    CHECK_FALSE(window.result().ok());
    frequency->setText(QStringLiteral("2.4 GHz"));
    window.recompute();
    CHECK(window.result().ok());

    // Load from a Touchstone one-port: Gamma of 12 - j30 against 50 ohm.
    NetworkData network;
    network.frequencies = {2e9, 3e9};
    network.z0 = {50};
    const Complex gamma = (Complex(12, -30) - 50.0) / (Complex(12, -30) + 50.0);
    for (int i = 0; i < 2; ++i) {
        CMatrix s(1, 1);
        s(0, 0) = gamma;
        network.s.push_back(s);
    }
    const QString path = dir.filePath(QStringLiteral("load.s1p"));
    REQUIRE(writeTouchstone(path, network));
    window.setImpedances({50, 0}, {50, 0}, 2.4e9);
    REQUIRE(window.loadFromTouchstone(path, &error));
    REQUIRE(window.result().ok());
    CHECK(std::abs(window.result().solutions.front().zl - Complex(12, -30)) < 1e-6);
    CHECK_FALSE(window.loadFromTouchstone(dir.filePath(QStringLiteral("missing.s1p")), &error));

    // Removing the target disables insertion.
    window.setTarget(nullptr, nullptr);
    CHECK_FALSE(window.canInsert());
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("matching_ui_tests"));
    return Catch::Session().run(argc, argv);
}

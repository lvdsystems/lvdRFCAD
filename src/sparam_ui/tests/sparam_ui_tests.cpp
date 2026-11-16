#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "plot/PlotWidget.h"
#include "rfdata/NetworkMath.h"
#include "rfdata/NetworkOps.h"
#include "rfdata/Touchstone.h"
#include "sparam_ui/SParamDialogs.h"
#include "sparam_ui/SParamFormat.h"
#include "sparam_ui/TouchstoneEditor.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QMenu>
#include <QPlainTextEdit>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>
#include <cstdio>
#include <string>

namespace Catch {
template<>
struct StringMaker<QString> {
    static std::string convert(const QString &value) { return value.toStdString(); }
};
} // namespace Catch

using namespace lvd;
using Catch::Approx;

namespace {

QString dataFile(const char *name)
{
    return QDir(QStringLiteral(LVD_RFDATA_TEST_DATA)).filePath(QString::fromLatin1(name));
}

rf::NetworkData readFile(const char *name)
{
    rf::TouchstoneReadResult result = rf::readTouchstone(dataFile(name));
    REQUIRE(result.network);
    return *result.network;
}

// Largest absolute difference of the S matrices (and same layout).
double maxDifference(const rf::NetworkData &a, const rf::NetworkData &b)
{
    if (a.frequencies.size() != b.frequencies.size() || a.portCount() != b.portCount())
        return INFINITY;
    double worst = 0;
    for (std::size_t k = 0; k < a.frequencies.size(); ++k) {
        worst = std::max(worst, std::abs(a.frequencies[k] - b.frequencies[k]) / std::max(1.0, a.frequencies[k]));
        worst = std::max(worst, (a.s[k] - b.s[k]).cwiseAbs().maxCoeff());
    }
    for (std::size_t i = 0; i < a.z0.size(); ++i)
        worst = std::max(worst, std::abs(a.z0[i] - b.z0[i]));
    return worst;
}

void requireSame(const rf::NetworkData &a, const rf::NetworkData &b, double tolerance = 1e-12)
{
    REQUIRE(maxDifference(a, b) <= tolerance);
    REQUIRE(a.noise.size() == b.noise.size());
    for (std::size_t k = 0; k < a.noise.size(); ++k) {
        REQUIRE(a.noise[k].fminDb == Approx(b.noise[k].fminDb).margin(tolerance * 10));
        REQUIRE(std::abs(a.noise[k].gammaOpt - b.noise[k].gammaOpt) <= tolerance * 10);
        REQUIRE(a.noise[k].rn == Approx(b.noise[k].rn).margin(tolerance * 10));
    }
}

QStringList traceNames(const PlotWidget *plot)
{
    QStringList names;
    for (const int id : plot->traceIds())
        names << plot->traceName(id);
    return names;
}

double cell(const TouchstoneEditor &editor, int row, int column)
{
    const QAbstractItemModel *model = editor.tableModel();
    const QVariant value = model->data(model->index(row, column), Qt::UserRole);
    REQUIRE(value.isValid());
    return value.toDouble();
}

// A copy of an example file in a temporary folder.
QString copyToTemp(const QTemporaryDir &dir, const char *name)
{
    const QString target = dir.filePath(QString::fromLatin1(name));
    QFile::remove(target);
    REQUIRE(QFile::copy(dataFile(name), target));
    QFile(target).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return target;
}

QAction *findAction(const TouchstoneEditor &editor, const QString &textPart)
{
    for (QAction *action : editor.actions()) {
        if (action->text().remove(QLatin1Char('&')).contains(textPart, Qt::CaseInsensitive))
            return action;
    }
    return nullptr;
}

} // namespace

TEST_CASE("every example file opens with the data of the reader")
{
    const char *files[] = {"one_port.s1p", "amp_noise.s2p", "three_port.s3p", "four_port.s4p", "two_port_v2.ts",
                           "three_port_v2.ts"};
    for (const char *name : files) {
        INFO(name);
        TouchstoneEditor editor;
        QSignalSpy titles(&editor, &TouchstoneEditor::titleChanged);
        QString error;
        REQUIRE(editor.openFile(dataFile(name), &error));
        CHECK(error.isEmpty());
        const rf::NetworkData expected = readFile(name);
        requireSame(editor.network(), expected, 0);
        CHECK(editor.network().comment == expected.comment);
        CHECK(!editor.isModified());
        CHECK(editor.title() == QString::fromLatin1(name));
        CHECK(titles.count() >= 1);
        CHECK(editor.filePath() == QFileInfo(dataFile(name)).absoluteFilePath());
        CHECK(editor.summaryText(QStringLiteral("ports")) == QString::number(expected.portCount()));
        CHECK(editor.summaryText(QStringLiteral("frequencies")).startsWith(QString::number(expected.frequencyCount())));
        CHECK(editor.tableModel()->rowCount() == expected.frequencyCount());
        CHECK(!editor.rectangularPlot()->traceIds().empty());
        CHECK(!editor.smithChart()->traceIds().empty());
    }
    TouchstoneEditor editor;
    QString error;
    CHECK(!editor.openFile(dataFile("missing.s2p"), &error));
    CHECK(!error.isEmpty());
}

TEST_CASE("the summary shows ports, frequencies, references, noise and comment")
{
    TouchstoneEditor editor;
    REQUIRE(editor.openFile(dataFile("amp_noise.s2p")));
    CHECK(editor.summaryText(QStringLiteral("file")) == QStringLiteral("amp_noise.s2p"));
    CHECK(editor.summaryText(QStringLiteral("ports")) == QStringLiteral("2"));
    CHECK(editor.summaryText(QStringLiteral("frequencies")) == QStringLiteral("11 points, 1 GHz – 6 GHz"));
    CHECK(editor.summaryText(QStringLiteral("references")) == QStringLiteral("50 Ω (all ports)"));
    CHECK(editor.summaryText(QStringLiteral("noise")).contains(QStringLiteral("4 frequencies")));
    CHECK(editor.summaryText(QStringLiteral("noise")).contains(QStringLiteral("1 GHz – 5 GHz")));
    CHECK(editor.summaryText(QStringLiteral("comment")).contains(QStringLiteral("amplifier with noise parameters")));
    CHECK(editor.summaryText(QStringLiteral("checks")) == QStringLiteral("Not checked"));

    REQUIRE(editor.openFile(dataFile("two_port_v2.ts")));
    CHECK(editor.summaryText(QStringLiteral("references")) == QStringLiteral("P1 50 Ω, P2 75 Ω"));
    CHECK(editor.summaryText(QStringLiteral("noise")).contains(QStringLiteral("3 frequencies")));
    CHECK(editor.writeOptions().version == 2);

    REQUIRE(editor.openFile(dataFile("three_port.s3p")));
    CHECK(editor.summaryText(QStringLiteral("noise")) == QStringLiteral("None"));
    CHECK(editor.writeOptions().version == 1);
}

TEST_CASE("the parameter selection drives the rectangular plot, the Smith chart and the table")
{
    TouchstoneEditor editor;
    REQUIRE(editor.openFile(dataFile("amp_noise.s2p")));
    using P = std::vector<std::pair<int, int>>;
    CHECK(editor.selectedParameters() == P{{0, 0}, {1, 0}});
    CHECK(traceNames(editor.rectangularPlot()) == QStringList({QStringLiteral("S11"), QStringLiteral("S21")}));
    CHECK(traceNames(editor.smithChart()) == QStringList({QStringLiteral("S11"), QStringLiteral("Γopt")}));
    CHECK(editor.tableModel()->columnCount() == 5);

    editor.setSelectedParameters({{0, 0}, {0, 1}, {1, 0}, {1, 1}});
    CHECK(traceNames(editor.rectangularPlot()).size() == 4);
    CHECK(traceNames(editor.smithChart())
          == QStringList({QStringLiteral("S11"), QStringLiteral("S22"), QStringLiteral("Γopt")}));
    CHECK(editor.tableModel()->columnCount() == 9);

    editor.setGammaOptVisible(false);
    CHECK(traceNames(editor.smithChart()) == QStringList({QStringLiteral("S11"), QStringLiteral("S22")}));

    // The check boxes act like setSelectedParameters.
    auto *box = editor.findChild<QCheckBox *>(QStringLiteral("select_1_2"));
    REQUIRE(box);
    box->setChecked(false);
    CHECK(editor.selectedParameters() == P{{0, 0}, {1, 0}, {1, 1}});
    CHECK(traceNames(editor.rectangularPlot()).size() == 3);

    // Display forms.
    editor.setPlotQuantity(SParamQuantity::Vswr);
    CHECK(traceNames(editor.rectangularPlot()) == QStringList({QStringLiteral("VSWR1"), QStringLiteral("VSWR2")}));
    editor.setPlotQuantity(SParamQuantity::ReturnLoss);
    CHECK(traceNames(editor.rectangularPlot()) == QStringList({QStringLiteral("RL1"), QStringLiteral("RL2")}));
    editor.setPlotQuantity(SParamQuantity::StabilityK);
    CHECK(traceNames(editor.rectangularPlot()) == QStringList({QStringLiteral("K"), QStringLiteral("|Δ|")}));
    editor.setPlotQuantity(SParamQuantity::StabilityMu);
    CHECK(traceNames(editor.rectangularPlot()).size() == 2);
    editor.setPlotQuantity(SParamQuantity::MaximumGain);
    CHECK(traceNames(editor.rectangularPlot()) == QStringList({QStringLiteral("MAG/MSG")}));
    editor.setPlotQuantity(SParamQuantity::NoiseFigureMin);
    CHECK(traceNames(editor.rectangularPlot()) == QStringList({QStringLiteral("NFmin")}));
    editor.setPlotQuantity(SParamQuantity::NoiseResistance);
    CHECK(traceNames(editor.rectangularPlot()) == QStringList({QStringLiteral("Rn")}));
    for (const auto q : {SParamQuantity::MagnitudeDb, SParamQuantity::Magnitude, SParamQuantity::PhaseDeg,
                         SParamQuantity::UnwrappedPhaseDeg, SParamQuantity::GroupDelay, SParamQuantity::Real,
                         SParamQuantity::Imaginary}) {
        editor.setPlotQuantity(q);
        CHECK(editor.plotQuantity() == q);
        CHECK(traceNames(editor.rectangularPlot()).size() == 3);
    }
    // Magnitude in dB of S21 at the first point: |S21| = 5.
    editor.setSelectedParameters({{1, 0}});
    editor.setPlotQuantity(SParamQuantity::MagnitudeDb);
    PlotWidget *plot = editor.rectangularPlot();
    const int marker = plot->addMarker(plot->traceIds().front(), 1e9);
    REQUIRE(plot->marker(marker));
    CHECK(plot->marker(marker)->y == Approx(20 * std::log10(5.0)).margin(1e-6));
    // The trace keeps its marker when the data change in place.
    REQUIRE(editor.setSParameter(0, 1, 0, rf::Complex(10, 0)));
    REQUIRE(plot->marker(marker));
    CHECK(plot->marker(marker)->y == Approx(20.0).margin(1e-6));

    // Stability and noise need a two-port with noise data.
    REQUIRE(editor.openFile(dataFile("three_port.s3p")));
    CHECK(!editor.isQuantityAvailable(SParamQuantity::StabilityK));
    CHECK(!editor.isQuantityAvailable(SParamQuantity::NoiseFigureMin));
    editor.setPlotQuantity(SParamQuantity::StabilityK);
    CHECK(editor.plotQuantity() == SParamQuantity::MagnitudeDb);
    CHECK(traceNames(editor.smithChart()) == QStringList({QStringLiteral("S11")}));
}

TEST_CASE("the table shows the network in RI, MA and dB-angle form")
{
    TouchstoneEditor editor;
    REQUIRE(editor.openFile(dataFile("four_port.s4p")));
    const rf::NetworkData &n = editor.network();
    editor.setSelectedParameters({{0, 0}, {2, 3}});
    const QAbstractItemModel *model = editor.tableModel();
    REQUIRE(model->rowCount() == n.frequencyCount());
    REQUIRE(model->columnCount() == 5);
    CHECK(model->headerData(3, Qt::Horizontal).toString() == QStringLiteral("S34 (dB)"));
    for (int k = 0; k < n.frequencyCount(); ++k) {
        const rf::Complex s11 = n.s[static_cast<std::size_t>(k)](0, 0);
        const rf::Complex s34 = n.s[static_cast<std::size_t>(k)](2, 3);
        CHECK(cell(editor, k, 0) == n.frequencies[static_cast<std::size_t>(k)]);
        CHECK(cell(editor, k, 1) == Approx(rf::db20(s11)));
        CHECK(cell(editor, k, 2) == Approx(std::arg(s11) * 180 / rf::Pi));
        CHECK(cell(editor, k, 3) == Approx(rf::db20(s34)));
    }
    editor.setTableForm(SParamTableForm::RealImaginary);
    CHECK(model->headerData(1, Qt::Horizontal).toString() == QStringLiteral("Re S11"));
    CHECK(cell(editor, 2, 3) == Approx(n.s[2](2, 3).real()));
    CHECK(cell(editor, 2, 4) == Approx(n.s[2](2, 3).imag()));
    editor.setTableForm(SParamTableForm::MagnitudeAngle);
    CHECK(cell(editor, 1, 1) == Approx(std::abs(n.s[1](0, 0))));
    // Frequencies are shown in a unit suited to the data (GHz here).
    CHECK(model->headerData(0, Qt::Horizontal).toString() == QStringLiteral("Frequency (GHz)"));

    // ABCD and T exist for two-ports only.
    editor.setTableRepresentation(SParamRepresentation::Abcd);
    CHECK(editor.tableRepresentation() == SParamRepresentation::S);
    editor.setTableRepresentation(SParamRepresentation::Z);
    CHECK(editor.tableRepresentation() == SParamRepresentation::Z);
    const std::optional<rf::CMatrix> z = rf::sToZ(n.s[0], n.z0);
    REQUIRE(z);
    CHECK(cell(editor, 0, 3) == Approx(std::abs((*z)(2, 3))));

    // CSV of the view.
    const QStringList lines = editor.tableCsv().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    CHECK(lines.size() == n.frequencyCount() + 1);
    CHECK(lines.front().startsWith(QStringLiteral("Frequency (Hz),|Z11| (Ω)")));
    QTemporaryDir dir;
    QString error;
    REQUIRE(editor.exportTableCsv(dir.filePath(QStringLiteral("table.csv")), &error));
    QFile csv(dir.filePath(QStringLiteral("table.csv")));
    REQUIRE(csv.open(QIODevice::ReadOnly));
    CHECK(QString::fromUtf8(csv.readAll()) == editor.tableCsv());
}

TEST_CASE("the table converts a two-port to Z, Y, ABCD and T")
{
    TouchstoneEditor editor;
    REQUIRE(editor.openFile(dataFile("two_port_v2.ts")));
    const rf::NetworkData &n = editor.network();
    editor.setSelectedParameters({{0, 0}, {0, 1}, {1, 0}, {1, 1}});
    editor.setTableForm(SParamTableForm::RealImaginary);
    const auto compare = [&](SParamRepresentation representation, auto convert) {
        editor.setTableRepresentation(representation);
        REQUIRE(editor.tableRepresentation() == representation);
        // Converted views are read only.
        const QModelIndex index = editor.tableModel()->index(0, 1);
        CHECK(!(editor.tableModel()->flags(index) & Qt::ItemIsEditable));
        for (int k = 0; k < n.frequencyCount(); ++k) {
            const std::optional<rf::CMatrix> m = convert(n.s[static_cast<std::size_t>(k)]);
            REQUIRE(m);
            for (int p = 0; p < 4; ++p) {
                const rf::Complex value = (*m)(p / 2, p % 2);
                CHECK(cell(editor, k, 1 + 2 * p) == Approx(value.real()).margin(1e-12));
                CHECK(cell(editor, k, 2 + 2 * p) == Approx(value.imag()).margin(1e-12));
            }
        }
    };
    compare(SParamRepresentation::Z, [&](const rf::CMatrix &s) { return rf::sToZ(s, n.z0); });
    CHECK(editor.tableModel()->headerData(3, Qt::Horizontal).toString() == QStringLiteral("Re Z12 (Ω)"));
    compare(SParamRepresentation::Y, [&](const rf::CMatrix &s) { return rf::sToY(s, n.z0); });
    CHECK(editor.tableModel()->headerData(1, Qt::Horizontal).toString() == QStringLiteral("Re Y11 (S)"));
    compare(SParamRepresentation::Abcd, [&](const rf::CMatrix &s) { return rf::sToAbcd(s, n.z0[0], n.z0[1]); });
    CHECK(editor.tableModel()->headerData(3, Qt::Horizontal).toString() == QStringLiteral("Re B (Ω)"));
    compare(SParamRepresentation::T, [&](const rf::CMatrix &s) { return rf::sToT(s); });
    editor.setTableRepresentation(SParamRepresentation::S);
    CHECK(editor.tableModel()->flags(editor.tableModel()->index(0, 1)) & Qt::ItemIsEditable);
}

TEST_CASE("editing a table cell is undoable and updates the plots")
{
    TouchstoneEditor editor;
    REQUIRE(editor.openFile(dataFile("amp_noise.s2p")));
    const rf::NetworkData original = editor.network();
    QSignalSpy modified(&editor, &TouchstoneEditor::modifiedChanged);
    editor.setTableForm(SParamTableForm::RealImaginary);
    QAbstractItemModel *model = editor.tableModel();
    const QString plotBefore = editor.rectangularPlot()->csvText();
    // Column 3 is Re S21.
    REQUIRE(model->setData(model->index(4, 3), QStringLiteral("0.25")));
    CHECK(editor.network().s[4](1, 0).real() == 0.25);
    CHECK(editor.network().s[4](1, 0).imag() == original.s[4](1, 0).imag());
    CHECK(editor.isModified());
    CHECK(modified.count() == 1);
    CHECK(editor.title().endsWith(QLatin1Char('*')));
    CHECK(editor.rectangularPlot()->csvText() != plotBefore);
    CHECK(cell(editor, 4, 3) == 0.25);
    // Bad input is refused.
    CHECK(!model->setData(model->index(4, 3), QStringLiteral("abc")));

    // dB-angle edit keeps the angle.
    editor.setTableForm(SParamTableForm::DecibelAngle);
    REQUIRE(model->setData(model->index(0, 1), QStringLiteral("-6")));
    CHECK(rf::db20(editor.network().s[0](0, 0)) == Approx(-6));
    CHECK(std::arg(editor.network().s[0](0, 0)) == Approx(std::arg(original.s[0](0, 0))));
    CHECK(editor.undoStack()->count() == 2);

    editor.undoStack()->undo();
    editor.undoStack()->undo();
    requireSame(editor.network(), original, 0);
    CHECK(!editor.isModified());
    CHECK(modified.count() == 2);
    CHECK(editor.rectangularPlot()->csvText() == plotBefore);
    editor.undoStack()->redo();
    CHECK(editor.network().s[4](1, 0).real() == 0.25);
}

TEST_CASE("the comment is editable and undoable")
{
    TouchstoneEditor editor;
    REQUIRE(editor.openFile(dataFile("amp_noise.s2p")));
    const QString original = editor.network().comment;
    editor.setComment(QStringLiteral("measured on bench 3"));
    CHECK(editor.network().comment == QStringLiteral("measured on bench 3"));
    CHECK(editor.summaryText(QStringLiteral("comment")) == QStringLiteral("measured on bench 3"));
    CHECK(editor.isModified());
    editor.undoStack()->undo();
    CHECK(editor.network().comment == original);
    CHECK(editor.summaryText(QStringLiteral("comment")) == original);
    // Typing in the field merges into one command.
    auto *field = editor.findChild<QPlainTextEdit *>(QStringLiteral("comment"));
    REQUIRE(field);
    field->setPlainText(QStringLiteral("a"));
    field->setPlainText(QStringLiteral("ab"));
    CHECK(editor.network().comment == QStringLiteral("ab"));
    CHECK(editor.undoStack()->count() == 1);
}

TEST_CASE("operations are undoable: references, renumbering, resampling, cropping")
{
    TouchstoneEditor editor;
    REQUIRE(editor.openFile(dataFile("three_port.s3p")));
    const rf::NetworkData original = editor.network();
    QUndoStack *undo = editor.undoStack();
    QString error;

    SECTION("reference impedances")
    {
        REQUIRE(editor.setReferenceImpedances({50, 75, 100}, &error));
        requireSame(editor.network(), rf::renormalized(original, {50, 75, 100}));
        CHECK(editor.summaryText(QStringLiteral("references")) == QStringLiteral("P1 50 Ω, P2 75 Ω, P3 100 Ω"));
        undo->undo();
        requireSame(editor.network(), original, 0);
        CHECK(editor.summaryText(QStringLiteral("references")) == sparam::referenceImpedanceText(original.z0));
        undo->redo();
        CHECK(editor.network().z0 == std::vector<double>{50, 75, 100});
        CHECK(!editor.setReferenceImpedances({50, -1, 50}, &error));
        CHECK(!editor.setReferenceImpedances({50, 50}, &error));
        CHECK(undo->count() == 1);
    }
    SECTION("renumbering")
    {
        REQUIRE(editor.renumberPorts({2, 0, 1}, &error));
        const std::optional<rf::NetworkData> expected = rf::renumberPorts(original, {2, 0, 1});
        REQUIRE(expected);
        requireSame(editor.network(), *expected);
        CHECK(editor.network().s[0](0, 1) == original.s[0](2, 0));
        undo->undo();
        requireSame(editor.network(), original, 0);
        CHECK(!editor.renumberPorts({0, 0, 1}, &error));
        CHECK(!error.isEmpty());
    }
    SECTION("resampling")
    {
        const double f0 = original.frequencies.front();
        const double f1 = original.frequencies.back();
        const std::vector<double> grid = sparam::linearGrid(f0, f1, 21);
        REQUIRE(editor.resample(grid, rf::Extrapolation::Error, &error));
        CHECK(editor.network().frequencyCount() == 21);
        CHECK(editor.tableModel()->rowCount() == 21);
        const auto expected = rf::interpolate(original, grid, rf::Extrapolation::Error);
        REQUIRE(expected);
        requireSame(editor.network(), *expected);
        undo->undo();
        requireSame(editor.network(), original, 0);
        CHECK(editor.tableModel()->rowCount() == original.frequencyCount());

        // Outside the data: Error refuses, Hold keeps the end values with a warning.
        const std::vector<double> wide = sparam::logGrid(f0 / 2, f1 * 2, 15);
        CHECK(!editor.resample(wide, rf::Extrapolation::Error, &error));
        CHECK(!error.isEmpty());
        QString warning;
        REQUIRE(editor.resample(wide, rf::Extrapolation::Hold, &error, &warning));
        CHECK(!warning.isEmpty());
        CHECK(editor.network().s.front() == original.s.front());
        CHECK(undo->count() == 1);
    }
    SECTION("cropping")
    {
        const double lo = original.frequencies[1];
        const double hi = original.frequencies[2];
        REQUIRE(editor.cropFrequencies(lo, hi, &error));
        CHECK(editor.network().frequencies == std::vector<double>{lo, hi});
        CHECK(editor.network().s[0] == original.s[1]);
        CHECK(editor.summaryText(QStringLiteral("frequencies")).startsWith(QStringLiteral("2 points")));
        undo->undo();
        requireSame(editor.network(), original, 0);
        CHECK(!editor.cropFrequencies(1, 2, &error));
        CHECK(!editor.cropFrequencies(hi, lo, &error));
    }
}

TEST_CASE("cascading and de-embedding two-ports are undoable")
{
    TouchstoneEditor editor;
    REQUIRE(editor.openFile(dataFile("amp_noise.s2p")));
    const rf::NetworkData fixture = readFile("two_port_v2.ts"); // 2 to 4 GHz, references 50 and 75
    QString error;
    // The fixture does not cover 1 to 6 GHz.
    CHECK(!editor.cascadeWith(fixture, true, &error));
    CHECK(error.contains(QStringLiteral("cover")));
    REQUIRE(editor.cropFrequencies(2e9, 4e9, &error));
    const rf::NetworkData amp = editor.network();

    REQUIRE(editor.cascadeWith(fixture, true, &error));
    const auto after = rf::cascade(amp, fixture);
    REQUIRE(after);
    requireSame(editor.network(), *after);
    CHECK(editor.network().comment == amp.comment);
    CHECK(editor.network().z0 == std::vector<double>{50, 75});

    // De-embedding the same fixture on the right gives the amplifier back.
    REQUIRE(editor.deembed(std::nullopt, fixture, &error));
    CHECK(maxDifference(editor.network(), rf::renormalized(amp, {50, 50})) < 1e-9);
    editor.undoStack()->undo();
    requireSame(editor.network(), *after);
    editor.undoStack()->undo();
    requireSame(editor.network(), amp, 0);

    // Before this network: the fixture's port 2 (75 ohm) meets the amplifier's port 1.
    REQUIRE(editor.cascadeWith(fixture, false, &error));
    const auto before = rf::cascade(fixture, amp);
    REQUIRE(before);
    CHECK(maxDifference(editor.network(), *before) < 1e-12);
    REQUIRE(editor.deembed(fixture, std::nullopt, &error));
    CHECK(editor.network().z0 == std::vector<double>{75, 50}); // inner reference of the fixture
    CHECK(maxDifference(editor.network(), rf::renormalized(amp, {75, 50})) < 1e-9);
    CHECK(!editor.deembed(std::nullopt, std::nullopt, &error));

    // Three-ports cannot be cascaded.
    CHECK(!editor.cascadeWith(readFile("three_port.s3p"), true, &error));
    CHECK(editor.undoStack()->count() == 3);
}

TEST_CASE("the passivity and reciprocity check reports and highlights failing frequencies")
{
    TouchstoneEditor editor;
    // An amplifier: |S21| up to 5, so active and non-reciprocal.
    REQUIRE(editor.openFile(dataFile("amp_noise.s2p")));
    const rf::NetworkData original = editor.network();
    const std::size_t tracesBefore = editor.rectangularPlot()->traceIds().size();
    const SParamCheckReport report = editor.checkPassivityAndReciprocity();
    CHECK(!report.passive());
    CHECK(!report.reciprocal());
    CHECK(report.nonPassive == rf::nonPassiveFrequencies(original));
    CHECK(!report.nonPassive.empty());
    CHECK(report.text.contains(QStringLiteral("Not passive")));
    CHECK(editor.summaryText(QStringLiteral("checks")).contains(QStringLiteral("Not passive")));
    CHECK(editor.highlightedFrequencies().size() == 11);
    CHECK(editor.rectangularPlot()->traceIds().size() > tracesBefore);
    CHECK(traceNames(editor.rectangularPlot()).contains(QStringLiteral("Not passive")));
    CHECK(traceNames(editor.smithChart()).contains(QStringLiteral("Not reciprocal")));
    requireSame(editor.network(), original, 0); // nothing enforced
    CHECK(!editor.isModified());
    editor.clearHighlights();
    CHECK(editor.highlightedFrequencies().empty());
    CHECK(editor.rectangularPlot()->traceIds().size() == tracesBefore);

    // Active at one frequency only.
    rf::NetworkData network;
    network.z0 = {50, 50};
    for (int k = 0; k < 5; ++k) {
        network.frequencies.push_back(1e9 * (k + 1));
        rf::CMatrix s(2, 2);
        const double t = k == 3 ? 1.2 : 0.5;
        s << rf::Complex(0.1, 0), rf::Complex(t, 0), rf::Complex(t, 0), rf::Complex(0.1, 0);
        network.s.push_back(s);
    }
    editor.setNetwork(network, QStringLiteral("attenuator"));
    const SParamCheckReport one = editor.checkPassivityAndReciprocity();
    CHECK(one.nonPassive == std::vector<int>{3});
    CHECK(one.reciprocal());
    CHECK(one.text.contains(QStringLiteral("1 of 5")));
    CHECK(one.text.contains(QStringLiteral("4 GHz")));
    CHECK(editor.highlightedFrequencies() == std::vector<int>{3});
    // Any change of the data drops the stale highlights.
    REQUIRE(editor.setSParameter(3, 1, 0, rf::Complex(0.5, 0)));
    CHECK(editor.highlightedFrequencies().empty());
    REQUIRE(editor.setSParameter(3, 0, 1, rf::Complex(0.5, 0)));
    CHECK(editor.checkPassivityAndReciprocity().passive());
}

TEST_CASE("saving as Touchstone 1 and 2 re-reads the same data")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    TouchstoneEditor editor;
    const QString path = copyToTemp(dir, "amp_noise.s2p");
    REQUIRE(editor.openFile(path));
    const rf::NetworkData original = editor.network();
    QString error;

    for (const auto format : {rf::TouchstoneFormat::RealImaginary, rf::TouchstoneFormat::MagnitudeAngle,
                              rf::TouchstoneFormat::DecibelAngle}) {
        for (const int version : {1, 2}) {
            INFO("version " << version << " format " << static_cast<int>(format));
            rf::TouchstoneWriteOptions options;
            options.version = version;
            options.format = format;
            options.frequencyUnit = QStringLiteral("MHz");
            options.precision = 15;
            const QString target = dir.filePath(version == 1 ? QStringLiteral("out_v1.s2p") : QStringLiteral("out_v2.ts"));
            REQUIRE(editor.saveAs(target, options, &error));
            CHECK(editor.filePath() == QFileInfo(target).absoluteFilePath());
            const rf::TouchstoneReadResult back = rf::readTouchstone(target);
            REQUIRE(back.network);
            requireSame(*back.network, original, 1e-12);
            CHECK(back.network->comment.contains(QStringLiteral("amplifier with noise parameters")));
        }
    }

    // Modify, save in place, re-open.
    REQUIRE(editor.setSParameter(0, 0, 0, rf::Complex(0.3, -0.2)));
    QSignalSpy modified(&editor, &TouchstoneEditor::modifiedChanged);
    REQUIRE(editor.save(&error));
    CHECK(!editor.isModified());
    CHECK(modified.count() == 1);
    TouchstoneEditor reopened;
    REQUIRE(reopened.openFile(editor.filePath()));
    CHECK(std::abs(reopened.network().s[0](0, 0) - rf::Complex(0.3, -0.2)) < 1e-12);

    // Different references: version 1 is refused, save() switches to version 2.
    REQUIRE(editor.saveAs(dir.filePath(QStringLiteral("amp.s2p")), rf::TouchstoneWriteOptions(), &error));
    REQUIRE(editor.setReferenceImpedances({50, 75}, &error));
    rf::TouchstoneWriteOptions v1;
    v1.version = 1;
    CHECK(!editor.saveAs(dir.filePath(QStringLiteral("amp2.s2p")), v1, &error));
    CHECK(error.contains(QStringLiteral("version 2")));
    REQUIRE(editor.save(&error));
    const rf::TouchstoneReadResult back = rf::readTouchstone(editor.filePath());
    REQUIRE(back.network);
    CHECK(back.network->z0 == std::vector<double>{50, 75});
    CHECK(maxDifference(*back.network, editor.network()) < 1e-9);
}

TEST_CASE("the file extension must match the port count")
{
    CHECK(sparam::extensionMatches(QStringLiteral("a.s2p"), 2, 1));
    CHECK(sparam::extensionMatches(QStringLiteral("a.S2P"), 2, 1));
    CHECK(!sparam::extensionMatches(QStringLiteral("a.s3p"), 2, 1));
    CHECK(!sparam::extensionMatches(QStringLiteral("a.ts"), 2, 1));
    CHECK(sparam::extensionMatches(QStringLiteral("a.ts"), 2, 2));
    CHECK(sparam::extensionMatches(QStringLiteral("a.s2p"), 2, 2));
    CHECK(!sparam::extensionMatches(QStringLiteral("a.txt"), 2, 2));
    CHECK(sparam::withMatchingExtension(QStringLiteral("C:/x/amp.s3p"), 2) == QStringLiteral("C:/x/amp.s2p"));
    CHECK(sparam::withMatchingExtension(QStringLiteral("C:/x/amp"), 4) == QStringLiteral("C:/x/amp.s4p"));
    CHECK(sparam::withMatchingExtension(QStringLiteral("C:/x/my.amp.ts"), 12) == QStringLiteral("C:/x/my.amp.s12p"));

    QTemporaryDir dir;
    TouchstoneEditor editor;
    REQUIRE(editor.openFile(dataFile("three_port.s3p")));
    QString error;
    rf::TouchstoneWriteOptions options;
    CHECK(!editor.saveAs(dir.filePath(QStringLiteral("x.s2p")), options, &error));
    CHECK(error.contains(QStringLiteral(".s3p")));
    CHECK(!QFile::exists(dir.filePath(QStringLiteral("x.s2p"))));
    CHECK(editor.saveAs(dir.filePath(QStringLiteral("x.s3p")), options, &error));
}

TEST_CASE("simulation results are shown as unsaved data")
{
    TouchstoneEditor editor;
    rf::NetworkData network = readFile("amp_noise.s2p");
    network.noise.clear();
    network.noiseFrequencies.clear();
    // Noise correlation of a matched attenuator-like network at T0.
    for (const rf::CMatrix &s : network.s)
        network.noiseCorrelation.push_back(rf::correlationFromNoiseParameters(s, {1.5, 0.2, 20}, 50));
    QSignalSpy modified(&editor, &TouchstoneEditor::modifiedChanged);
    editor.setNetwork(network, QStringLiteral("LNA sweep"));
    CHECK(editor.isModified());
    CHECK(modified.count() == 1);
    CHECK(editor.title() == QStringLiteral("LNA sweep*"));
    CHECK(editor.filePath().isEmpty());
    CHECK(editor.summaryText(QStringLiteral("noise")).contains(QStringLiteral("correlation")));
    CHECK(editor.isQuantityAvailable(SParamQuantity::NoiseFigureMin));
    editor.setPlotQuantity(SParamQuantity::NoiseFigureMin);
    PlotWidget *plot = editor.rectangularPlot();
    REQUIRE(plot->traceIds().size() == 1);
    const int marker = plot->addMarker(plot->traceIds().front(), network.frequencies[2]);
    CHECK(plot->marker(marker)->y == Approx(1.5).margin(1e-9));
    CHECK(traceNames(editor.smithChart()).contains(QStringLiteral("Γopt")));
    // An invalid network is refused.
    rf::NetworkData bad = network;
    bad.s.pop_back();
    editor.setNetwork(bad, QStringLiteral("bad"));
    CHECK(editor.network().frequencyCount() == network.frequencyCount());
}

TEST_CASE("actions are provided for the main window")
{
    TouchstoneEditor editor;
    CHECK(findAction(editor, QStringLiteral("Save As")));
    CHECK(findAction(editor, QStringLiteral("Reference Impedances")));
    CHECK(findAction(editor, QStringLiteral("Renumber")));
    CHECK(findAction(editor, QStringLiteral("Resample")));
    CHECK(findAction(editor, QStringLiteral("Crop")));
    CHECK(findAction(editor, QStringLiteral("Cascade")));
    CHECK(findAction(editor, QStringLiteral("De-embed")));
    CHECK(findAction(editor, QStringLiteral("Passivity")));
    CHECK(findAction(editor, QStringLiteral("CSV")));
    QAction *plotMenu = findAction(editor, QStringLiteral("Plot"));
    REQUIRE(plotMenu);
    REQUIRE(plotMenu->menu());
    CHECK(!plotMenu->menu()->actions().isEmpty());
    // Nothing to save or operate on without data.
    CHECK(!findAction(editor, QStringLiteral("Resample"))->isEnabled());
    REQUIRE(editor.openFile(dataFile("three_port.s3p")));
    CHECK(findAction(editor, QStringLiteral("Resample"))->isEnabled());
    CHECK(!findAction(editor, QStringLiteral("Cascade"))->isEnabled());
    CHECK(editor.maybeSave()); // not modified: no question
}

TEST_CASE("dialogs return what they show")
{
    ReferenceImpedanceDialog references({50, 75});
    CHECK(references.impedances() == std::vector<double>{50, 75});
    RenumberPortsDialog renumber(3);
    CHECK(renumber.order() == std::vector<int>{0, 1, 2});
    renumber.setOrder({2, 1, 0});
    CHECK(renumber.order() == std::vector<int>{2, 1, 0});
    ResampleDialog resample(1e9, 2e9, 11);
    const auto grid = resample.frequencies();
    REQUIRE(grid);
    CHECK(grid->size() == 11);
    CHECK(grid->front() == Approx(1e9));
    CHECK(grid->back() == Approx(2e9));
    CHECK(resample.extrapolation() == rf::Extrapolation::Error);
    CropDialog crop(1e9, 3e9);
    const auto range = crop.range();
    REQUIRE(range);
    CHECK(range->first == Approx(1e9));
    CHECK(range->second == Approx(3e9));
    rf::TouchstoneWriteOptions options;
    options.version = 2;
    options.format = rf::TouchstoneFormat::DecibelAngle;
    options.frequencyUnit = QStringLiteral("MHz");
    SaveOptionsDialog save(options, true);
    CHECK(save.options().version == 2);
    CHECK(save.options().format == rf::TouchstoneFormat::DecibelAngle);
    CHECK(save.options().frequencyUnit == QStringLiteral("MHz"));
    SaveOptionsDialog save2(rf::TouchstoneWriteOptions(), false);
    CHECK(save2.options().version == 2); // version 1 not allowed

    CHECK(sparam::parseFrequency(QStringLiteral("1.5 GHz")) == Approx(1.5e9));
    CHECK(sparam::parseFrequency(QStringLiteral("100M")) == Approx(1e8));
    CHECK(sparam::parseFrequency(QStringLiteral("2e9 Hz")) == Approx(2e9));
    CHECK(sparam::parseFrequency(QStringLiteral("3"), 1e9) == Approx(3e9));
    CHECK(!sparam::parseFrequency(QStringLiteral("abc")));
    const auto list = sparam::parseFrequencyList(QStringLiteral("1 GHz 1.5GHz\n2 GHz; 2500 MHz"), 1e9);
    REQUIRE(list);
    CHECK(*list == std::vector<double>{1e9, 1.5e9, 2e9, 2.5e9});
}

TEST_CASE("a four-port with 10001 points loads and plots quickly")
{
    // Synthetic data: a lossy four-port with frequency dependent phase.
    rf::NetworkData network;
    network.z0 = {50, 50, 50, 50};
    const int points = 10001;
    for (int k = 0; k < points; ++k) {
        const double f = 1e7 + k * 2e6; // 10 MHz to 20.01 GHz
        network.frequencies.push_back(f);
        rf::CMatrix s(4, 4);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                s(i, j) = std::polar(i == j ? 0.3 + 0.1 * std::sin(f / 1e9) : 0.2 / (1 + std::abs(i - j)),
                                     -2 * rf::Pi * f * 1e-10 * (1 + i + j));
        network.s.push_back(s);
    }
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("big.s4p"));
    REQUIRE(rf::writeTouchstone(path, network));
    std::printf("big.s4p: %.1f MB\n", static_cast<double>(QFileInfo(path).size()) / 1e6);

    TouchstoneEditor editor;
    editor.resize(1440, 900);
    editor.show();
    QApplication::processEvents();

    QElapsedTimer timer;
    timer.start();
    REQUIRE(editor.openFile(path));
    const qint64 open = timer.elapsed();
    timer.restart();
    const QImage first = editor.grab().toImage();
    const qint64 firstPaint = timer.elapsed();
    timer.restart();
    std::vector<std::pair<int, int>> all;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            all.emplace_back(i, j);
    editor.setSelectedParameters(all);
    editor.grab();
    const qint64 allSixteen = timer.elapsed();
    timer.restart();
    editor.setPlotQuantity(SParamQuantity::GroupDelay);
    editor.grab();
    const qint64 groupDelay = timer.elapsed();
    timer.restart();
    REQUIRE(editor.setSParameter(5000, 1, 0, rf::Complex(0.1, 0.1)));
    editor.grab();
    const qint64 edit = timer.elapsed();
    timer.restart();
    for (int i = 0; i < 10; ++i)
        editor.rectangularPlot()->repaint();
    const qint64 repaint = timer.elapsed() / 10;
    timer.restart();
    editor.tableView()->scrollToBottom();
    editor.grab();
    const qint64 tableScroll = timer.elapsed();
    timer.restart();
    REQUIRE(editor.resample(sparam::linearGrid(1e8, 2e10, 10001), rf::Extrapolation::Error));
    editor.grab();
    const qint64 resample = timer.elapsed();

    std::printf("4-port x 10001: open %lld ms, first paint %lld ms, 16 traces %lld ms, group delay %lld ms, "
                "cell edit %lld ms, plot repaint %lld ms, table scroll %lld ms, resample %lld ms\n",
                static_cast<long long>(open), static_cast<long long>(firstPaint), static_cast<long long>(allSixteen),
                static_cast<long long>(groupDelay), static_cast<long long>(edit), static_cast<long long>(repaint),
                static_cast<long long>(tableScroll), static_cast<long long>(resample));
    CHECK(!first.isNull());
    CHECK(editor.tableModel()->rowCount() == points);
    CHECK(open < 5000);
    CHECK(allSixteen < 3000);
    CHECK(edit < 1000);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Keep test runs away from the real application's stored settings.
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("sparam_ui_tests"));
    return Catch::Session().run(argc, argv);
}

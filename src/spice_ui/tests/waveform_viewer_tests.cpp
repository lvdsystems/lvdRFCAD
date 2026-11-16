#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "spice_ui/MeasurementPanel.h"
#include "spice_ui/WaveformPane.h"
#include "spice_ui/WaveformViewer.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDialog>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTest>
#include <QWheelEvent>

#include <cmath>
#include <numbers>

using namespace lvd;
using namespace lvd::spiceui;
using lvd::spice::SpicePlot;
using lvd::spice::SpiceRunResult;
using lvd::spice::SpiceVector;
using lvd::spice::VectorKind;
using Catch::Approx;

namespace {

constexpr double kPi = std::numbers::pi;

SpiceVector realVector(const QString &name, VectorKind kind, std::vector<double> values)
{
    SpiceVector v;
    v.name = name;
    v.kind = kind;
    v.real = std::move(values);
    return v;
}

// 5 ms of a 1 kHz sine (amplitude a), a +-1 V square wave with 1 us edges,
// a distorted sine with 10 % third harmonic and the source current.
std::shared_ptr<const SpiceRunResult> makeTran(double a = 1.0, int n = 20001)
{
    std::vector<double> t(static_cast<std::size_t>(n)), in(t.size()), out(t.size()), dist(t.size()), i(t.size());
    const double f = 1e3;
    for (std::size_t k = 0; k < t.size(); ++k) {
        const double x = 5e-3 * double(k) / double(n - 1);
        t[k] = x;
        in[k] = a * std::sin(2 * kPi * f * x);
        const double phase = std::fmod(x * f, 1.0);
        // Square: rises over the first microsecond of a period, falls at the middle.
        const double edge = 1e-6 * f; // 1 us in periods
        double sq;
        if (phase < edge)
            sq = -1 + 2 * phase / edge;
        else if (phase < 0.5)
            sq = 1;
        else if (phase < 0.5 + edge)
            sq = 1 - 2 * (phase - 0.5) / edge;
        else
            sq = -1;
        out[k] = sq;
        dist[k] = std::sin(2 * kPi * f * x) + 0.1 * std::sin(3 * 2 * kPi * f * x);
        i[k] = -in[k] / 1000.0;
    }
    auto result = std::make_shared<SpiceRunResult>();
    result->ok = true;
    SpicePlot plot;
    plot.name = QStringLiteral("tran1");
    plot.type = QStringLiteral("tran");
    plot.scaleName = QStringLiteral("time");
    plot.vectors.push_back(realVector(QStringLiteral("time"), VectorKind::Time, t));
    plot.vectors.push_back(realVector(QStringLiteral("v(in)"), VectorKind::Voltage, in));
    plot.vectors.push_back(realVector(QStringLiteral("v(out)"), VectorKind::Voltage, out));
    plot.vectors.push_back(realVector(QStringLiteral("v(dist)"), VectorKind::Voltage, dist));
    plot.vectors.push_back(realVector(QStringLiteral("i(vv1)"), VectorKind::Current, i));
    result->plots << plot;
    return result;
}

// First-order low pass, 1 kHz corner, 1 Hz to 1 MHz.
std::shared_ptr<const SpiceRunResult> makeAc()
{
    auto result = std::make_shared<SpiceRunResult>();
    result->ok = true;
    SpicePlot plot;
    plot.name = QStringLiteral("ac1");
    plot.type = QStringLiteral("ac");
    plot.scaleName = QStringLiteral("frequency");
    std::vector<double> f;
    SpiceVector out;
    out.name = QStringLiteral("v(out)");
    out.kind = VectorKind::Voltage;
    out.isComplex = true;
    for (int k = 0; k <= 600; ++k) {
        const double x = std::pow(10.0, k / 100.0);
        f.push_back(x);
        out.complex.push_back(1.0 / std::complex<double>(1.0, x / 1e3));
    }
    plot.vectors.push_back(realVector(QStringLiteral("frequency"), VectorKind::Frequency, f));
    plot.vectors.push_back(out);
    result->plots << plot;
    return result;
}

std::shared_ptr<const SpiceRunResult> makeOp()
{
    auto result = std::make_shared<SpiceRunResult>();
    result->ok = true;
    SpicePlot plot;
    plot.name = QStringLiteral("op1");
    plot.type = QStringLiteral("op");
    plot.vectors.push_back(realVector(QStringLiteral("v(out)"), VectorKind::Voltage, {2.5}));
    result->plots << plot;
    return result;
}

WaveformPaneDefinition paneDef(const QString &id, const QStringList &traces)
{
    WaveformPaneDefinition d;
    d.id = id;
    d.traces = traces;
    return d;
}

struct Viewer {
    WaveformViewer viewer;
    Viewer()
    {
        viewer.resize(1200, 800);
        viewer.show();
        QApplication::processEvents();
    }
};

} // namespace

TEST_CASE("Pane definitions round-trip through JSON")
{
    WaveformPaneDefinition a = paneDef(QStringLiteral("p1"), {QStringLiteral("v(out)"), QStringLiteral("i(vv1)*1000")});
    a.name = QStringLiteral("Output");
    a.logX = true;
    a.complexParts.insert(QStringLiteral("v(out)"), QStringLiteral("phase"));
    WaveformPaneDefinition b;
    b.id = QStringLiteral("p2");
    b.fftSource = QStringLiteral("v(in)");
    b.fftWindow = fftWindowKey(spice::WindowType::BlackmanHarris);
    b.fftLinear = true;
    CHECK(WaveformPaneDefinition::fromJson(a.toJson()) == a);
    CHECK(WaveformPaneDefinition::fromJson(b.toJson()) == b);
    const QList<WaveformPaneDefinition> list{a, b};
    const QByteArray text = QJsonDocument(WaveformViewer::panesToJson(list)).toJson();
    CHECK(WaveformViewer::panesFromJson(QJsonDocument::fromJson(text).array()) == list);
    for (spice::WindowType w : spice::allWindows())
        CHECK(fftWindowFromKey(fftWindowKey(w)) == w);
}

TEST_CASE("setPanes round-trips and does not emit panesEdited")
{
    Viewer v;
    QSignalSpy edited(&v.viewer, &WaveformViewer::panesEdited);
    v.viewer.setData(makeTran(), 0);
    const QList<WaveformPaneDefinition> defs{paneDef(QStringLiteral("p1"), {QStringLiteral("v(in)")}),
                                             paneDef(QStringLiteral("p3"), {QStringLiteral("v(out)")})};
    v.viewer.setPanes(defs);
    CHECK(v.viewer.panes() == defs);
    CHECK(edited.count() == 0);
    CHECK(v.viewer.currentPaneId() == QStringLiteral("p1"));
    CHECK(v.viewer.availableVectors().first() == QStringLiteral("time"));
    CHECK(v.viewer.availableVectors().contains(QStringLiteral("i(vv1)")));
    // New ids continue after the highest.
    CHECK(v.viewer.addPane() == QStringLiteral("p4"));
    CHECK(edited.count() == 1);
}

TEST_CASE("addTrace adds to the current pane and reports unknown vectors")
{
    Viewer v;
    v.viewer.setData(makeTran(), 0);
    v.viewer.setPanes({paneDef(QStringLiteral("p1"), {})});
    QSignalSpy edited(&v.viewer, &WaveformViewer::panesEdited);
    QString error;
    CHECK(v.viewer.addTrace(QStringLiteral("v(in)"), QString(), &error));
    CHECK(edited.count() == 1);
    CHECK(v.viewer.panes().front().traces == QStringList{QStringLiteral("v(in)")});
    REQUIRE(edited.front().front().value<QList<WaveformPaneDefinition>>().front().traces.size() == 1);

    CHECK_FALSE(v.viewer.addTrace(QStringLiteral("v(nope)"), QString(), &error));
    CHECK(error.contains(QStringLiteral("v(nope)")));
    CHECK(error.contains(QStringLiteral("unknown vector")));
    CHECK_FALSE(v.viewer.addTrace(QStringLiteral("v(in"), QString(), &error)); // syntax
    CHECK_FALSE(v.viewer.addTrace(QStringLiteral("v(in)"), QString(), &error)); // duplicate
    CHECK(edited.count() == 1);
    CHECK(v.viewer.panes().front().traces.size() == 1);

    // Expressions on traces.
    CHECK(v.viewer.addTrace(QStringLiteral("i(vv1)*1000")));
    WaveformPane *p = v.viewer.currentPane();
    REQUIRE(p->traceCount() == 2);
    REQUIRE(p->traceWaveform(1));
    CHECK(p->traceWaveform(1)->size() == 20001);
}

TEST_CASE("A failing stored trace shows its error without breaking the others")
{
    Viewer v;
    v.viewer.setData(makeTran(), 0);
    v.viewer.setPanes({paneDef(QStringLiteral("p1"), {QStringLiteral("v(in)"), QStringLiteral("v(nope)")})});
    WaveformPane *p = v.viewer.pane(QStringLiteral("p1"));
    REQUIRE(p);
    CHECK(p->traceWaveform(0));
    CHECK(p->plotTraceId(0) >= 0);
    CHECK_FALSE(p->traceWaveform(1));
    CHECK(p->traceError(1).contains(QStringLiteral("unknown vector")));
    CHECK(p->noteText().startsWith(QStringLiteral("v(nope): ")));
    CHECK(p->plot()->traceIds().size() == 1);
}

TEST_CASE("Complex traces default to dB and phase goes to the right axis")
{
    Viewer v;
    v.viewer.setData(makeAc(), 0);
    v.viewer.setPanes({});
    REQUIRE(v.viewer.currentPane());
    CHECK(v.viewer.currentPane()->definition().logX); // ac default
    REQUIRE(v.viewer.addTrace(QStringLiteral("v(out)")));
    WaveformPane *p = v.viewer.currentPane();
    CHECK(p->traceComplexPart(0) == ComplexPart::MagnitudeDb);
    const auto shown = p->shownWaveform(0);
    REQUIRE(shown);
    CHECK(shown->unit == QStringLiteral("dB"));
    CHECK(shown->y.front().real() == Approx(0).margin(1e-3));
    CHECK(shown->y.back().real() == Approx(-60).margin(0.01));
    CHECK(p->plot()->xScale().isLog());

    QSignalSpy edited(&v.viewer, &WaveformViewer::panesEdited);
    REQUIRE(v.viewer.addTrace(QStringLiteral("v(out)*1")));
    p->setTraceComplexPart(1, ComplexPart::PhaseDeg);
    CHECK(edited.count() == 2);
    CHECK(p->definition().complexParts.value(QStringLiteral("v(out)*1")) == QStringLiteral("phase"));
    CHECK(p->traceAxis(0) == PlotYAxis::Left);
    CHECK(p->traceAxis(1) == PlotYAxis::Right);
    CHECK(p->shownWaveform(1)->y.back().real() == Approx(-90).margin(0.2));
}

TEST_CASE("Currents go to the right axis of a voltage pane")
{
    Viewer v;
    v.viewer.setData(makeTran(), 0);
    v.viewer.setPanes({paneDef(QStringLiteral("p1"), {QStringLiteral("v(in)"), QStringLiteral("i(vv1)"),
                                                      QStringLiteral("v(out)")})});
    WaveformPane *p = v.viewer.currentPane();
    CHECK(p->traceAxis(0) == PlotYAxis::Left);
    CHECK(p->traceAxis(1) == PlotYAxis::Right);
    CHECK(p->traceAxis(2) == PlotYAxis::Left);
    CHECK(p->plot()->hasRightAxis());
    CHECK(p->xUnit() == QStringLiteral("s"));
}

TEST_CASE("Cursors give the deltas")
{
    Viewer v;
    v.viewer.setData(makeTran(), 0);
    v.viewer.setPanes({paneDef(QStringLiteral("p1"), {QStringLiteral("v(in)")})});
    WaveformPane *p = v.viewer.currentPane();
    p->setCursorX(0, 0.25e-3);
    p->setCursorX(1, 0.75e-3);
    const WaveformCursorReadout r = p->cursorReadout();
    REQUIRE(r.hasDelta());
    CHECK(r.yA == Approx(1).margin(1e-6));
    CHECK(r.yB == Approx(-1).margin(1e-6));
    CHECK(r.dx == Approx(0.5e-3));
    CHECK(r.dy == Approx(-2).margin(1e-6));
    CHECK(r.inverseDx == Approx(2000));
    CHECK(p->cursorText().contains(QStringLiteral("2 kHz")));
    CHECK(p->plot()->markers().size() == 2);

    // Toolbar-style toggling places cursors inside the view.
    p->setCursorX(0, std::nullopt);
    p->setCursorX(1, std::nullopt);
    v.viewer.setCursorShown(0, true);
    REQUIRE(p->cursorX(0));
    CHECK(*p->cursorX(0) == Approx(5e-3 / 3).epsilon(1e-6));
}

TEST_CASE("Measurements of a known sine")
{
    Viewer v;
    v.viewer.setData(makeTran(), 0);
    v.viewer.setPanes({paneDef(QStringLiteral("p1"), {QStringLiteral("v(out)"), QStringLiteral("v(in)")})});
    WaveformPane *p = v.viewer.currentPane();
    p->setSelectedTrace(1);
    MeasurementPanel *m = v.viewer.measurementPanel();
    REQUIRE(m->hasSource());
    CHECK(m->value(QStringLiteral("frequency")).value_or(0) == Approx(1000).epsilon(1e-4));
    CHECK(m->value(QStringLiteral("rms")).value_or(0) == Approx(1 / std::sqrt(2.0)).epsilon(1e-4));
    CHECK(m->value(QStringLiteral("pkpk")).value_or(0) == Approx(2).epsilon(1e-4));
    CHECK(m->valueText(QStringLiteral("frequency")).contains(QStringLiteral("kHz")));
    CHECK(m->valueText(QStringLiteral("rms")).contains(QStringLiteral("mV")));
    // The model shows them too.
    QAbstractItemModel *model = m->model();
    bool found = false;
    for (int r = 0; r < model->rowCount(); ++r) {
        if (model->index(r, 0).data(Qt::UserRole + 1).toString() == QStringLiteral("frequency")) {
            CHECK(model->index(r, 1).data().toString() == m->valueText(QStringLiteral("frequency")));
            found = true;
        }
    }
    CHECK(found);
    CHECK(m->isRowShown(QStringLiteral("thd")));

    // Between cursors: one half period.
    p->setCursorX(0, 0.0);
    p->setCursorX(1, 0.5e-3);
    CHECK(m->value(QStringLiteral("mean")).value_or(0) == Approx(2 / kPi).epsilon(1e-4));
    CHECK(m->valueText(QStringLiteral("frequency")) == QString(QChar(0x2014)));
    CHECK_FALSE(m->model()->index(5, 1).data(Qt::ToolTipRole).toString().isEmpty());

    // Square wave: rise time 0.8 us, duty cycle 50 %.
    p->setCursorX(0, std::nullopt);
    p->setCursorX(1, std::nullopt);
    p->setSelectedTrace(0);
    CHECK(m->value(QStringLiteral("rise")).value_or(0) == Approx(0.8e-6).epsilon(1e-3));
    CHECK(m->value(QStringLiteral("duty")).value_or(0) == Approx(50).epsilon(1e-3));
}

TEST_CASE("THD of a known signal and the Fourier table")
{
    Viewer v;
    v.viewer.setData(makeTran(), 0);
    v.viewer.setPanes({paneDef(QStringLiteral("p1"), {QStringLiteral("v(dist)")})});
    MeasurementPanel *m = v.viewer.measurementPanel();
    CHECK(m->value(QStringLiteral("thd")).value_or(0) == Approx(10).epsilon(1e-3));
    m->setFundamental(1000);
    CHECK(m->value(QStringLiteral("thd")).value_or(0) == Approx(10).epsilon(1e-3));
    const auto fourier = m->fourier();
    REQUIRE(fourier);
    REQUIRE(fourier->harmonics.size() == 9);
    CHECK(fourier->harmonics[2].normalizedMagnitude == Approx(0.1).epsilon(1e-3));
    QDialog *dialog = m->showFourierDialog();
    REQUIRE(dialog);
    auto *table = dialog->findChild<QTableWidget *>(QStringLiteral("harmonics"));
    REQUIRE(table);
    CHECK(table->rowCount() == 9);
    CHECK(table->item(2, 1)->text().contains(QStringLiteral("3 kHz")));
    dialog->close();
}

TEST_CASE("FFT pane shows the spectrum with its peak at the sine frequency")
{
    Viewer v;
    v.viewer.setData(makeTran(), 0);
    v.viewer.setPanes({paneDef(QStringLiteral("p1"), {QStringLiteral("v(in)")})});
    QSignalSpy edited(&v.viewer, &WaveformViewer::panesEdited);
    QString error;
    const QString id = v.viewer.fftOfSelectedTrace(&error);
    REQUIRE_FALSE(id.isEmpty());
    CHECK(edited.count() == 1);
    WaveformPane *p = v.viewer.pane(id);
    REQUIRE(p);
    CHECK(p->isFft());
    CHECK(p->definition().fftSource == QStringLiteral("v(in)"));
    CHECK(v.viewer.currentPaneId() == id);
    const spice::Waveform *w = p->traceWaveform(0);
    REQUIRE(w);
    CHECK(w->unit == QStringLiteral("dBV"));
    std::size_t peak = 0;
    for (std::size_t k = 1; k < w->size(); ++k)
        if (w->y[k].real() > w->y[peak].real())
            peak = k;
    CHECK(w->x[peak] == Approx(1000).margin(1.0));
    CHECK(w->y[peak].real() == Approx(0).margin(0.1));

    // Window and linear scale are pane options.
    p->setFftWindow(spice::WindowType::FlatTop);
    p->setFftLinear(true);
    CHECK(edited.count() == 3);
    CHECK(p->traceWaveform(0)->unit == QStringLiteral("V"));
    CHECK(v.viewer.panes().back().fftWindow == QStringLiteral("flattop"));
    // Not on AC data.
    v.viewer.setData(makeAc(), 0);
    CHECK(v.viewer.addFftPane(QStringLiteral("v(out)"), &error).isEmpty());
    CHECK(error.contains(QStringLiteral("transient")));
}

TEST_CASE("A new run keeps panes, zoom and cursors")
{
    Viewer v;
    v.viewer.setData(makeTran(), 0);
    const QList<WaveformPaneDefinition> defs{paneDef(QStringLiteral("p1"), {QStringLiteral("v(in)")}),
                                             paneDef(QStringLiteral("p2"), {QStringLiteral("v(out)")})};
    v.viewer.setPanes(defs);
    QSignalSpy edited(&v.viewer, &WaveformViewer::panesEdited);
    WaveformPane *p1 = v.viewer.pane(QStringLiteral("p1"));
    p1->plot()->setXRange(1e-3, 2e-3);
    p1->setCursorX(0, 1.25e-3);
    v.viewer.setData(makeTran(2.0), 0);
    CHECK(v.viewer.panes() == defs);
    CHECK(edited.count() == 0);
    p1 = v.viewer.pane(QStringLiteral("p1"));
    REQUIRE(p1);
    CHECK(p1->xRange().first == Approx(1e-3));
    CHECK(p1->xRange().second == Approx(2e-3));
    CHECK(v.viewer.pane(QStringLiteral("p2"))->xRange().first == Approx(1e-3));
    REQUIRE(p1->cursorX(0));
    CHECK(*p1->cursorX(0) == Approx(1.25e-3));
    CHECK(p1->cursorReadout().yA == Approx(2).margin(1e-5));
    CHECK(v.viewer.measurementPanel()->value(QStringLiteral("max")).value_or(0) == Approx(2).margin(1e-5));

    // Another analysis: panes stay, view resets.
    v.viewer.setData(makeAc(), 0);
    CHECK(v.viewer.panes().size() == 2);
    CHECK_FALSE(v.viewer.pane(QStringLiteral("p1"))->cursorX(0));
}

TEST_CASE("Op results show a placeholder")
{
    Viewer v;
    v.viewer.setPanes({paneDef(QStringLiteral("p1"), {QStringLiteral("v(out)")})});
    v.viewer.setData(makeOp(), 0);
    CHECK_FALSE(v.viewer.hasWaveforms());
    CHECK(v.viewer.placeholderText().contains(QStringLiteral("operating point")));
    CHECK(v.viewer.panes().size() == 1);
    v.viewer.clearData();
    CHECK_FALSE(v.viewer.hasWaveforms());
    CHECK(v.viewer.placeholderText().contains(QStringLiteral("No simulation results")));
    v.viewer.setData(makeTran(), 0);
    CHECK(v.viewer.hasWaveforms());
    CHECK(v.viewer.pane(QStringLiteral("p1"))->traceWaveform(0));
}

TEST_CASE("Zooming one pane moves the others")
{
    Viewer v;
    v.viewer.setData(makeTran(), 0);
    v.viewer.setPanes({paneDef(QStringLiteral("p1"), {QStringLiteral("v(in)")}),
                       paneDef(QStringLiteral("p2"), {QStringLiteral("i(vv1)")})});
    WaveformPane *p1 = v.viewer.pane(QStringLiteral("p1"));
    WaveformPane *p2 = v.viewer.pane(QStringLiteral("p2"));
    p2->plot()->setXRange(2e-3, 2.2e-3);
    CHECK(p1->xRange().first == Approx(2e-3));
    CHECK(p1->xRange().second == Approx(2.2e-3));
    // Wheel zoom on the plot as the user does it.
    const QPointF centre = p1->plot()->plotArea().center();
    QWheelEvent wheel(centre, p1->plot()->mapToGlobal(centre), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(p1->plot(), &wheel);
    CHECK(p1->xRange().second - p1->xRange().first < 0.19e-3);
    CHECK(p2->xRange().first == Approx(p1->xRange().first));
    CHECK(p2->xRange().second == Approx(p1->xRange().second));
    // Measurements follow the visible range.
    v.viewer.setCurrentPane(QStringLiteral("p1"));
    CHECK(v.viewer.measurementPanel()->value(QStringLiteral("max")).value_or(0) < 1.0);
    // Clicking a pane makes it current.
    QTest::mouseClick(p2->plot(), Qt::LeftButton, Qt::NoModifier, p2->plot()->plotArea().center().toPoint());
    CHECK(v.viewer.currentPaneId() == QStringLiteral("p2"));
    CHECK(p2->isCurrent());
    CHECK_FALSE(p1->isCurrent());
}

TEST_CASE("User edits emit panesEdited")
{
    Viewer v;
    v.viewer.setData(makeTran(), 0);
    v.viewer.setPanes({paneDef(QStringLiteral("p1"), {QStringLiteral("v(in)"), QStringLiteral("v(out)")})});
    QSignalSpy edited(&v.viewer, &WaveformViewer::panesEdited);
    CHECK(v.viewer.removeTrace(QStringLiteral("p1"), 0));
    CHECK(edited.count() == 1);
    CHECK(v.viewer.panes().front().traces == QStringList{QStringLiteral("v(out)")});
    v.viewer.currentPane()->setLogX(true);
    CHECK(edited.count() == 2);
    v.viewer.currentPane()->rename(QStringLiteral("Output"));
    CHECK(edited.count() == 3);
    const QString id = v.viewer.addPane();
    CHECK(edited.count() == 4);
    CHECK(v.viewer.removePane(id));
    CHECK(edited.count() == 5);
    CHECK(v.viewer.panes().size() == 1);
    CHECK(v.viewer.panes().front().name == QStringLiteral("Output"));
    CHECK(v.viewer.panes().front().logX);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("waveform_viewer_tests"));
    return Catch::Session().run(argc, argv);
}

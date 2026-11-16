#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "plot/ColorMapPlot.h"
#include "plot/ColorMaps.h"
#include "plot/PlotAxis.h"
#include "plot/PlotMath.h"
#include "plot/PlotWidget.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <tuple>

using namespace lvd;
using Catch::Approx;

namespace {

struct Shown {
    PlotWidget plot;
    explicit Shown(PlotKind kind = PlotKind::Rectangular)
    {
        plot.setKind(kind);
        plot.resize(800, 500);
        plot.show();
        QApplication::processEvents();
    }
};

std::vector<double> range(double from, double to, int n)
{
    std::vector<double> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = from + (to - from) * i / (n - 1);
    return v;
}

QPoint pt(const QPointF &p)
{
    return p.toPoint();
}

} // namespace

TEST_CASE("Linear ticks are nice and cover the range")
{
    const AxisTicks t = linearTicks(0, 1, 5);
    CHECK(t.step == Approx(0.2));
    REQUIRE(t.major.size() == 6);
    CHECK(t.major.front() == 0.0);
    CHECK(t.major.back() == Approx(1.0));
    CHECK(!t.minor.empty());

    const AxisTicks f = linearTicks(1.8e9, 3.0e9, 6);
    CHECK(f.step == Approx(2e8));
    CHECK(f.major.front() == Approx(1.8e9));
    CHECK(f.major.back() == Approx(3.0e9));

    const AxisTicks n = linearTicks(-47.3, 3.1, 6);
    CHECK(n.step == Approx(10));
    CHECK(n.major.front() == Approx(-40));
    // No "-0" from rounding.
    for (double v : linearTicks(-1, 1, 4).major)
        if (std::abs(v) < 1e-12)
            CHECK(!std::signbit(v));
}

TEST_CASE("Log ticks use decades, adding 2 and 5 on short ranges")
{
    const AxisTicks wide = logTicks(1e6, 1e10, 8);
    REQUIRE(wide.major.size() == 5);
    CHECK(wide.major.front() == Approx(1e6));
    CHECK(wide.major.back() == Approx(1e10));
    CHECK(!wide.minor.empty());

    const AxisTicks narrow = logTicks(10, 1000, 10);
    CHECK(std::find_if(narrow.major.begin(), narrow.major.end(), [](double v) { return std::abs(v - 20) < 1e-9; })
          != narrow.major.end());
    CHECK(std::find_if(narrow.major.begin(), narrow.major.end(), [](double v) { return std::abs(v - 500) < 1e-9; })
          != narrow.major.end());

    const AxisTicks many = logTicks(1e-3, 1e12, 5); // 15 decades, 5 labels
    CHECK(many.major.size() <= 6);
    CHECK(many.major.size() >= 3);

    const AxisTicks short_ = logTicks(2, 8, 5); // under a decade: linear ticks
    CHECK(short_.step > 0);
}

TEST_CASE("Tick and value formatting uses engineering prefixes")
{
    CHECK(formatTick(1e9, 5e8, QStringLiteral("Hz")) == QStringLiteral("1 GHz"));
    CHECK(formatTick(5e8, 5e8, QStringLiteral("Hz")) == QStringLiteral("500 MHz"));
    CHECK(formatTick(1.5e9, 5e8, QStringLiteral("Hz")) == QStringLiteral("1.5 GHz"));
    CHECK(formatTick(2.4e9, 2e8, QStringLiteral("Hz")) == QStringLiteral("2.4 GHz"));
    CHECK(formatTick(0, 2e8, QStringLiteral("Hz")) == QStringLiteral("0 Hz"));
    CHECK(formatTick(1e4, 0, QStringLiteral("Hz")) == QStringLiteral("10 kHz"));
    CHECK(formatTick(-10, 10, QStringLiteral("dB")) == QStringLiteral("-10"));
    CHECK(formatTick(-2.5, 0.5, QStringLiteral("dB")) == QStringLiteral("-2.5"));
    CHECK(formatTick(2.5e-10, 5e-11, QStringLiteral("s")) == QStringLiteral("250 ps"));
    CHECK(formatTick(1.2e-9, 2e-10, QStringLiteral("s")) == QStringLiteral("1.2 ns"));
    CHECK(formatTick(1e-16, 0.2, QString()) == QStringLiteral("0"));

    CHECK(formatValue(50, QStringLiteral("ohm"), 4) == QStringLiteral("50 ") + QChar(0x03A9));
    CHECK(formatValue(2.41234567e9, QStringLiteral("Hz"), 6) == QStringLiteral("2.41235 GHz"));
    CHECK(formatValue(-3.0103, QStringLiteral("dB"), 4) == QStringLiteral("-3.01 dB"));
    CHECK(formatValue(45.25, QStringLiteral("deg"), 4) == QStringLiteral("45.25") + QChar(0x00B0));
    CHECK(unitTakesPrefix(QStringLiteral("Hz")));
    CHECK(!unitTakesPrefix(QStringLiteral("dB")));
}

TEST_CASE("Axis scale maps linear and log values")
{
    AxisScale lin(0, 10, false);
    lin.setPixels(100, 300);
    CHECK(lin.toPixel(5) == Approx(200));
    CHECK(lin.fromPixel(150) == Approx(2.5));

    AxisScale log(1, 100, true);
    log.setPixels(0, 200);
    CHECK(log.toPixel(10) == Approx(100));
    CHECK(log.fromPixel(100) == Approx(10));
    CHECK(std::isnan(log.toPixel(-1)));

    // y axes run upwards.
    AxisScale y(-20, 0, false);
    y.setPixels(400, 0);
    CHECK(y.toPixel(-20) == Approx(400));
    CHECK(y.toPixel(0) == Approx(0));

    log.zoom(0.5, 10); // keeps 10 in place
    CHECK(log.toPixel(10) == Approx(100));
    CHECK(log.min() == Approx(std::sqrt(10.0)));
}

TEST_CASE("Smith chart math converts between Gamma and Z")
{
    CHECK(std::abs(gammaFromImpedance(50.0, 50)) < 1e-12);
    CHECK(gammaFromImpedance(100.0, 50).real() == Approx(1.0 / 3));
    const std::complex<double> z(30, -20);
    const auto back = impedanceFromGamma(gammaFromImpedance(z, 50), 50);
    CHECK(back.real() == Approx(30));
    CHECK(back.imag() == Approx(-20));
    CHECK(vswrFromGamma(1.0 / 3) == Approx(2.0));
    CHECK(std::isinf(vswrFromGamma(1.0)));
}

TEST_CASE("Smith and polar pixel mapping round-trips")
{
    Shown s(PlotKind::Smith);
    const QPointF centre = s.plot.gammaToPixel(0.0);
    CHECK(centre.x() == Approx(s.plot.plotArea().center().x()));
    CHECK(centre.y() == Approx(s.plot.plotArea().center().y()));
    const std::complex<double> g(0.3, -0.4);
    const auto back = s.plot.pixelToGamma(s.plot.gammaToPixel(g));
    CHECK(back.real() == Approx(0.3));
    CHECK(back.imag() == Approx(-0.4));
    // Positive reactance is drawn in the upper half.
    CHECK(s.plot.gammaToPixel({0, 0.5}).y() < centre.y());
}

TEST_CASE("Autoscale fits the data with a margin on y")
{
    Shown s;
    const int id = s.plot.addTrace(QStringLiteral("a"), QStringLiteral("dB"));
    const std::vector<double> x = {1, 4, 10};
    const std::vector<double> y = {-5, 5, 0};
    s.plot.setTraceData(id, x, y);
    CHECK(s.plot.xScale().min() == Approx(1));
    CHECK(s.plot.xScale().max() == Approx(10));
    CHECK(s.plot.yScale().min() == Approx(-5.5));
    CHECK(s.plot.yScale().max() == Approx(5.5));

    // Log y skips non-positive values.
    s.plot.setYAxis(QStringLiteral("v"), QStringLiteral("V"), true);
    CHECK(s.plot.yScale().min() > 0);
    CHECK(s.plot.yScale().max() > 5);

    // Right-axis traces scale their own axis.
    s.plot.setYAxis(QStringLiteral("v"), QStringLiteral("dB"), false);
    TraceStyle right;
    right.yAxis = PlotYAxis::Right;
    const int r = s.plot.addTrace(QStringLiteral("delay"), QStringLiteral("s"), right);
    const std::vector<double> d = {1e-9, 2e-9, 3e-9};
    s.plot.setTraceData(r, x, d);
    CHECK(s.plot.hasRightAxis());
    CHECK(s.plot.yScale(PlotYAxis::Right).max() == Approx(3.1e-9));
    CHECK(s.plot.yScale(PlotYAxis::Left).min() == Approx(-5.5));
}

TEST_CASE("Rubber-band zoom, wheel zoom and double-click reset")
{
    Shown s;
    const int id = s.plot.addTrace(QStringLiteral("a"));
    const auto x = range(0, 100, 101);
    s.plot.setTraceData(id, x, x);
    REQUIRE(s.plot.isAutoscaled());

    const QPointF p1 = s.plot.dataToPixel(20, 60);
    const QPointF p2 = s.plot.dataToPixel(40, 30);
    QTest::mousePress(&s.plot, Qt::LeftButton, {}, pt(p1));
    QTest::mouseMove(&s.plot, pt((p1 + p2) / 2));
    QTest::mouseMove(&s.plot, pt(p2));
    QTest::mouseRelease(&s.plot, Qt::LeftButton, {}, pt(p2));
    CHECK(!s.plot.isAutoscaled());
    CHECK(s.plot.xScale().min() == Approx(20).margin(0.5));
    CHECK(s.plot.xScale().max() == Approx(40).margin(0.5));
    CHECK(s.plot.yScale().min() == Approx(30).margin(0.5));
    CHECK(s.plot.yScale().max() == Approx(60).margin(0.5));

    // Wheel zoom keeps the value under the cursor.
    const QPointF c = s.plot.dataToPixel(25, 50);
    const QPointF before = s.plot.pixelToData(c);
    QWheelEvent wheel(c, s.plot.mapToGlobal(c), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(&s.plot, &wheel);
    CHECK(s.plot.xScale().max() - s.plot.xScale().min() < 20);
    const QPointF after = s.plot.pixelToData(c);
    CHECK(after.x() == Approx(before.x()).margin(1e-6));
    CHECK(after.y() == Approx(before.y()).margin(1e-6));

    // Middle-button pan.
    const double minBefore = s.plot.xScale().min();
    QTest::mousePress(&s.plot, Qt::MiddleButton, {}, pt(c));
    QTest::mouseMove(&s.plot, pt(c + QPointF(50, 0)));
    QTest::mouseRelease(&s.plot, Qt::MiddleButton, {}, pt(c + QPointF(50, 0)));
    CHECK(s.plot.xScale().min() < minBefore);

    QTest::mouseDClick(&s.plot, Qt::LeftButton, {}, pt(c));
    CHECK(s.plot.isAutoscaled());
    CHECK(s.plot.xScale().min() == Approx(0));
    CHECK(s.plot.xScale().max() == Approx(100));

    s.plot.setXRange(10, 20);
    CHECK(!s.plot.isAutoscaled());
    s.plot.autoscaleAction()->trigger();
    CHECK(s.plot.isAutoscaled());
}

TEST_CASE("Markers interpolate along their trace")
{
    Shown s;
    const int id = s.plot.addTrace(QStringLiteral("line"), QStringLiteral("dB"));
    const std::vector<double> x = {0, 1, 2, 3, 4, 10};
    const std::vector<double> y = {0, 2, 4, 6, 8, 20};
    s.plot.setTraceData(id, x, y);
    QSignalSpy changed(&s.plot, &PlotWidget::markersChanged);
    const int m = s.plot.addMarker(id, 2.5);
    CHECK(changed.count() == 1);
    auto info = s.plot.marker(m);
    REQUIRE(info);
    CHECK(info->valid);
    CHECK(info->y == Approx(5));
    CHECK(info->name == QStringLiteral("M1"));
    s.plot.setMarkerX(m, 7);
    CHECK(s.plot.marker(m)->y == Approx(14));
    s.plot.setMarkerX(m, 11);
    CHECK(!s.plot.marker(m)->valid);

    // Complex traces interpolate the complex value too.
    const int c = s.plot.addTrace(QStringLiteral("S11"));
    const std::vector<double> f = {1e9, 2e9};
    const std::vector<std::complex<double>> g = {{0.5, 0.0}, {0.0, 0.5}};
    s.plot.setTraceData(c, f, g);
    const int mc = s.plot.addMarker(c, 1.5e9);
    const auto ci = s.plot.marker(mc);
    REQUIRE(ci);
    CHECK(ci->isComplex);
    CHECK(ci->value.real() == Approx(0.25));
    CHECK(ci->value.imag() == Approx(0.25));
    CHECK(ci->name == QStringLiteral("M2"));

    // Delta to a reference marker.
    s.plot.setMarkerX(m, 1);
    const int m3 = s.plot.addMarker(id, 3);
    s.plot.setReferenceMarker(m);
    const QStringList table = s.plot.markerTable();
    REQUIRE(table.size() == 3);
    CHECK(table[2].contains(QChar(0x0394)));
    CHECK(table[2].contains(QStringLiteral("4 dB")));
    CHECK(table[0].endsWith(QStringLiteral("ref")));
    s.plot.removeMarker(m3);
    CHECK(s.plot.markers().size() == 2);
    s.plot.clearMarkers();
    CHECK(s.plot.markers().empty());
}

TEST_CASE("Peak search finds the maximum and minimum")
{
    Shown s;
    const int id = s.plot.addTrace(QStringLiteral("sin"));
    const auto x = range(0, 2 * std::numbers::pi, 1001);
    std::vector<double> y;
    for (double v : x)
        y.push_back(std::sin(v));
    s.plot.setTraceData(id, x, y);
    const auto peak = s.plot.findPeak(id, true);
    REQUIRE(peak);
    CHECK(*peak == Approx(std::numbers::pi / 2).margin(0.01));
    const auto dip = s.plot.findPeak(id, false);
    REQUIRE(dip);
    CHECK(*dip == Approx(3 * std::numbers::pi / 2).margin(0.01));
    const auto limited = s.plot.findPeak(id, true, 3.0, 6.0);
    REQUIRE(limited);
    CHECK(*limited == Approx(3.0).margin(0.01));
    const int m = s.plot.addPeakMarker(id, true);
    CHECK(s.plot.marker(m)->y == Approx(1).margin(1e-4));
}

TEST_CASE("Markers can be dragged along their trace")
{
    Shown s;
    const int id = s.plot.addTrace(QStringLiteral("line"));
    const auto x = range(0, 10, 101);
    std::vector<double> y;
    for (double v : x)
        y.push_back(v * v);
    s.plot.setTraceData(id, x, y);
    const int m = s.plot.addMarker(id, 3);
    const QPointF tip = s.plot.dataToPixel(3, 9);
    const QPointF grab = tip - QPointF(0, 7);
    const QPointF target = s.plot.dataToPixel(6, 10);
    QTest::mousePress(&s.plot, Qt::LeftButton, {}, pt(grab));
    QTest::mouseMove(&s.plot, pt(target));
    QTest::mouseRelease(&s.plot, Qt::LeftButton, {}, pt(target));
    const auto info = s.plot.marker(m);
    REQUIRE(info);
    CHECK(info->x == Approx(6).margin(0.05));
    CHECK(info->y == Approx(info->x * info->x).margin(0.1));
    // A drag is not a zoom.
    CHECK(s.plot.isAutoscaled());

    // On a Smith chart the marker follows the closest point of the trace.
    Shown smith(PlotKind::Smith);
    const int t = smith.plot.addTrace(QStringLiteral("S11"));
    const auto f = range(1e9, 2e9, 201);
    std::vector<std::complex<double>> g;
    for (std::size_t i = 0; i < f.size(); ++i)
        g.push_back(std::polar(0.5, std::numbers::pi * static_cast<double>(i) / 200.0)); // half circle
    smith.plot.setTraceData(t, f, g);
    const int sm = smith.plot.addMarker(t, 1e9);
    const QPointF from = smith.plot.gammaToPixel(0.5) - QPointF(0, 7);
    const QPointF to = smith.plot.gammaToPixel(std::polar(0.55, std::numbers::pi / 2));
    QTest::mousePress(&smith.plot, Qt::LeftButton, {}, pt(from));
    QTest::mouseMove(&smith.plot, pt(to));
    QTest::mouseRelease(&smith.plot, Qt::LeftButton, {}, pt(to));
    CHECK(smith.plot.marker(sm)->x == Approx(1.5e9).margin(0.02e9));
}

TEST_CASE("Clicking the legend hides and shows a trace")
{
    Shown s;
    const int a = s.plot.addTrace(QStringLiteral("S21"));
    const int b = s.plot.addTrace(QStringLiteral("S11"));
    const std::vector<double> x = {1, 2};
    const std::vector<double> ya = {0, 1};
    const std::vector<double> yb = {10, 20};
    s.plot.setTraceData(a, x, ya);
    s.plot.setTraceData(b, x, yb);
    CHECK(s.plot.yScale().max() > 20);
    const QRectF item = s.plot.legendItemRect(b);
    REQUIRE(item.isValid());
    QSignalSpy spy(&s.plot, &PlotWidget::traceVisibilityChanged);
    QTest::mouseClick(&s.plot, Qt::LeftButton, {}, pt(item.center()));
    CHECK(!s.plot.isTraceVisible(b));
    REQUIRE(spy.count() == 1);
    CHECK(spy.at(0).at(0).toInt() == b);
    CHECK(spy.at(0).at(1).toBool() == false);
    CHECK(s.plot.yScale().max() < 2); // hidden traces leave the autoscale
    QTest::mouseClick(&s.plot, Qt::LeftButton, {}, pt(s.plot.legendItemRect(b).center()));
    CHECK(s.plot.isTraceVisible(b));
    // Automatic colours differ and survive removing another trace.
    const QColor colorB = s.plot.traceColor(b);
    CHECK(s.plot.traceColor(a) != colorB);
    s.plot.removeTrace(a);
    CHECK(s.plot.traceColor(b) == colorB);
}

TEST_CASE("Hovering reports the nearest trace value")
{
    Shown s;
    const int id = s.plot.addTrace(QStringLiteral("S21"), QString());
    const std::vector<double> x = {1e9, 2e9, 3e9};
    const std::vector<double> y = {-10, -1, -10};
    s.plot.setTraceData(id, x, y);
    QSignalSpy spy(&s.plot, &PlotWidget::hoverReadoutChanged);
    QTest::mouseMove(&s.plot, pt(s.plot.dataToPixel(2e9, -1)));
    REQUIRE(spy.count() >= 1);
    const QString text = spy.last().at(0).toString();
    CHECK(text.contains(QStringLiteral("S21")));
    CHECK(text.contains(QStringLiteral("GHz")));
    CHECK(s.plot.hoverReadout().traceId == id);
    CHECK(s.plot.hoverReadout().x == Approx(2e9).margin(1e7));
    CHECK(s.plot.hoverReadout().y == Approx(-1).margin(0.05));
}

TEST_CASE("Hovering a Smith chart reports Gamma, Z and VSWR")
{
    Shown smith(PlotKind::Smith);
    QSignalSpy smithSpy(&smith.plot, &PlotWidget::hoverReadoutChanged);
    QTest::mouseMove(&smith.plot, pt(smith.plot.gammaToPixel(1.0 / 3)));
    REQUIRE(smithSpy.count() >= 1);
    const QString smithText = smithSpy.last().at(0).toString();
    INFO(smithText.toStdString());
    CHECK((smithText.contains(QStringLiteral("VSWR = 2")) || smithText.contains(QStringLiteral("VSWR = 1.99"))));
    CHECK((smithText.contains(QStringLiteral("Z = 10")) || smithText.contains(QStringLiteral("Z = 99."))));
    CHECK(smithText.contains(QChar(0x03A9)));
    CHECK(smith.plot.hoverReadout().value.real() == Approx(1.0 / 3).margin(0.01));
}

TEST_CASE("CSV export contains all traces")
{
    PlotWidget plot;
    const int a = plot.addTrace(QStringLiteral("S21"), QStringLiteral("dB"));
    const int b = plot.addTrace(QStringLiteral("S11"));
    const std::vector<double> x = {1e9, 2e9};
    const std::vector<double> y = {-1.5, -0.5};
    const std::vector<std::complex<double>> z = {{0.1, 0.2}, {0.3, -0.4}};
    plot.setTraceData(a, x, y);
    plot.setTraceData(b, x, z);
    QStringList lines = plot.csvText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == QStringLiteral("Frequency (Hz),S21 (dB),S11 re,S11 im"));
    CHECK(lines[1] == QStringLiteral("1000000000,-1.5,0.1,0.2"));
    CHECK(lines[2] == QStringLiteral("2000000000,-0.5,0.3,-0.4"));

    // Different x values: one x column per trace.
    const int c = plot.addTrace(QStringLiteral("NF, dB"));
    const std::vector<double> x3 = {5e8, 1e9, 1.5e9};
    plot.setTraceData(c, x3, x3);
    lines = plot.csvText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].startsWith(QStringLiteral("S21 Frequency (Hz),S21 (dB),")));
    CHECK(lines[0].endsWith(QStringLiteral("\"NF, dB\"")));
    CHECK(lines[3] == QStringLiteral(",,,,,1500000000,1500000000"));

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("plot.csv"));
    REQUIRE(plot.exportCsv(path));
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(QString::fromUtf8(file.readAll()) == plot.csvText());
}

TEST_CASE("Image and SVG export")
{
    PlotWidget plot;
    plot.setTitle(QStringLiteral("S-parameters"));
    const int a = plot.addTrace(QStringLiteral("S21"));
    const auto x = range(1e9, 3e9, 201);
    std::vector<double> y;
    for (double v : x)
        y.push_back(-std::abs(v - 2e9) / 1e8);
    plot.setTraceData(a, x, y);
    plot.addMarker(a, 2e9);
    const QImage image = plot.renderImage(QSize(1000, 600));
    CHECK(image.size() == QSize(1000, 600));
    QTemporaryDir dir;
    CHECK(plot.exportImage(dir.filePath(QStringLiteral("p.png")), QSize(640, 400)));
    CHECK(QImage(dir.filePath(QStringLiteral("p.png"))).size() == QSize(640, 400));
    if (PlotWidget::svgSupported()) {
        const QString svg = dir.filePath(QStringLiteral("p.svg"));
        REQUIRE(plot.exportSvg(svg, QSize(640, 400)));
        QFile file(svg);
        REQUIRE(file.open(QIODevice::ReadOnly));
        CHECK(file.readAll().contains("<svg"));
    }
    for (PlotKind kind : {PlotKind::Smith, PlotKind::Polar}) {
        plot.setKind(kind);
        CHECK(!plot.renderImage(QSize(500, 500)).isNull());
    }
}

TEST_CASE("The theme follows the palette")
{
    PlotWidget plot;
    QPalette light;
    light.setColor(QPalette::Window, QColor(240, 240, 240));
    light.setColor(QPalette::Base, Qt::white);
    light.setColor(QPalette::WindowText, Qt::black);
    plot.setPalette(light);
    CHECK(!plot.theme().dark);
    const int id = plot.addTrace(QStringLiteral("a"));
    const QColor lightColor = plot.traceColor(id);
    QPalette dark;
    dark.setColor(QPalette::Window, QColor(30, 30, 30));
    dark.setColor(QPalette::Base, QColor(20, 20, 20));
    dark.setColor(QPalette::WindowText, QColor(230, 230, 230));
    plot.setPalette(dark);
    CHECK(plot.theme().dark);
    CHECK(plot.traceColor(id) != lightColor);
}

TEST_CASE("Min/max decimation preserves every column's extremes")
{
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0, 1);
    const int n = 100000;
    std::vector<double> x(n), y(n);
    for (int i = 0; i < n; ++i) {
        x[static_cast<std::size_t>(i)] = i;
        y[static_cast<std::size_t>(i)] = noise(rng) + (i == 12345 ? 50 : 0) - (i == 77777 ? 60 : 0);
    }
    y[50000] = NAN; // a gap
    const double columns = 500;
    const auto mapX = [&](double v) { return v / n * columns; };
    const auto mapY = [](double v) { return -v; };
    std::vector<QPolygonF> segments;
    decimateMinMax(x.data(), y.data(), 0, static_cast<std::size_t>(n), mapX, mapY, segments);
    CHECK(segments.size() == 2);
    std::size_t total = 0;
    for (const auto &s : segments)
        total += static_cast<std::size_t>(s.size());
    CHECK(total <= 4 * 501);
    CHECK(total >= 500);

    // For each column, the smallest and largest y of the input are in the output.
    std::vector<double> lo(501, INFINITY), hi(501, -INFINITY);
    for (int i = 0; i < n; ++i) {
        const double v = y[static_cast<std::size_t>(i)];
        if (std::isnan(v))
            continue;
        const auto c = static_cast<std::size_t>(std::floor(mapX(i)));
        lo[c] = std::min(lo[c], -v);
        hi[c] = std::max(hi[c], -v);
    }
    std::vector<double> outLo(501, INFINITY), outHi(501, -INFINITY);
    for (const auto &s : segments) {
        for (const QPointF &p : s) {
            const auto c = static_cast<std::size_t>(std::floor(p.x()));
            outLo[c] = std::min(outLo[c], p.y());
            outHi[c] = std::max(outHi[c], p.y());
        }
    }
    bool same = true;
    for (std::size_t c = 0; c < 500; ++c)
        same = same && lo[c] == outLo[c] && hi[c] == outHi[c];
    CHECK(same);
    CHECK(*std::min_element(outLo.begin(), outLo.end()) == *std::min_element(lo.begin(), lo.end()));
}

TEST_CASE("Ten traces of 100k points render quickly")
{
    PlotWidget plot;
    plot.setTitle(QStringLiteral("Performance"));
    const std::size_t n = 100000;
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = 1e9 + 1e4 * static_cast<double>(i);
    std::vector<int> ids;
    std::vector<std::vector<double>> ys(10, std::vector<double>(n));
    for (int t = 0; t < 10; ++t) {
        ids.push_back(plot.addTrace(QStringLiteral("trace %1").arg(t + 1)));
        for (std::size_t i = 0; i < n; ++i)
            ys[static_cast<std::size_t>(t)][i] = std::sin(static_cast<double>(i) * 1e-3 * (t + 1)) * 10 - t * 3
                                                 + std::sin(static_cast<double>(i) * 0.7) ;
    }
    QElapsedTimer timer;
    timer.start();
    for (int t = 0; t < 10; ++t)
        plot.setTraceData(ids[static_cast<std::size_t>(t)], x, ys[static_cast<std::size_t>(t)]);
    const double setMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;

    QImage image(1200, 800, QImage::Format_ARGB32_Premultiplied);
    {
        QPainter warm(&image);
        plot.render(&warm, QRectF(0, 0, 1200, 800));
    }
    const int frames = 10;
    timer.restart();
    for (int f = 0; f < frames; ++f) {
        QPainter painter(&image);
        plot.render(&painter, QRectF(0, 0, 1200, 800));
    }
    const double frameMs = static_cast<double>(timer.nsecsElapsed()) / 1e6 / frames;

    // Zoomed in (fewer points per column, still decimated).
    plot.setXRange(1.2e9, 1.3e9);
    timer.restart();
    for (int f = 0; f < frames; ++f) {
        QPainter painter(&image);
        plot.render(&painter, QRectF(0, 0, 1200, 800));
    }
    const double zoomedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6 / frames;
    std::printf("PERF 10 x 100k points: setTraceData (all 10) %.1f ms, frame 1200x800 %.1f ms, zoomed frame %.1f ms\n",
                setMs, frameMs, zoomedMs);
    CHECK(frameMs < 100);
    CHECK(zoomedMs < 100);
    CHECK(setMs < 200);
}

// ---------------------------------------------------------------- ColorMapPlot

namespace {

struct ShownMap {
    ColorMapPlot plot;
    explicit ShownMap(QSize size = QSize(700, 500))
    {
        plot.resize(size);
        plot.show();
        QApplication::processEvents();
    }
};

// Uniform linear-array factor (normalised), N elements at spacing d (wavelengths).
double arrayFactor(double u, double u0, int n, double d)
{
    const double x = std::numbers::pi * d * (u - u0);
    if (std::abs(std::sin(x)) < 1e-12)
        return 1.0;
    return std::abs(std::sin(n * x) / (n * std::sin(x)));
}

// UV map of an 8 x 8 array at 0.7 wavelength spacing steered to u = 0.3:
// gain in dBi, NaN outside the visible region.
std::vector<double> uvPattern(int n)
{
    std::vector<double> values(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    for (int iy = 0; iy < n; ++iy) {
        const double v = -1.0 + 2.0 * iy / (n - 1);
        for (int ix = 0; ix < n; ++ix) {
            const double u = -1.0 + 2.0 * ix / (n - 1);
            const double af = arrayFactor(u, 0.3, 8, 0.7) * arrayFactor(v, 0.0, 8, 0.7);
            values[static_cast<std::size_t>(iy * n + ix)] =
                u * u + v * v > 1.0 ? NAN : std::max(-40.0, 20 * std::log10(std::max(af, 1e-6))) + 21.0;
        }
    }
    return values;
}

} // namespace

TEST_CASE("Colour maps clamp their ends and give NaN no colour")
{
    for (ColorMap map : {ColorMap::Viridis, ColorMap::Jet, ColorMap::Phase, ColorMap::Gray}) {
        CHECK(colorMapColor(map, -1.0) == colorMapColor(map, 0.0));
        CHECK(colorMapColor(map, 2.0) == colorMapColor(map, 1.0));
        CHECK(!colorMapColor(map, NAN).isValid());
        CHECK(colorMapFromName(colorMapName(map)) == map);
    }
    const QColor low = colorMapColor(ColorMap::Viridis, 0.0);
    CHECK(low.red() == 68); // matplotlib's viridis starts at #440154
    CHECK(low.blue() == 84);
    CHECK(colorMapColor(ColorMap::Viridis, 1.0).green() > 200);
    CHECK(colorMapColor(ColorMap::Phase, 0.0) == colorMapColor(ColorMap::Phase, 1.0)); // cyclic
    CHECK(colorMapFromName(QStringLiteral("unknown")) == ColorMap::Viridis);
}

TEST_CASE("Colour-map image read-out and pixel mapping")
{
    ShownMap shown;
    ColorMapPlot &plot = shown.plot;
    // 5 columns (x 0..4) by 3 rows (y 10..12): value = 10 * row + column.
    std::vector<double> values(15);
    for (int i = 0; i < 15; ++i)
        values[static_cast<std::size_t>(i)] = 10 * (i / 5) + (i % 5);
    values[6] = NAN; // row 1, column 1
    plot.setImage(5, 3, 0, 4, 10, 12, values);
    REQUIRE(plot.hasImage());

    CHECK(plot.valueAt({2, 11}) == 12.0);
    CHECK(plot.valueAt({4.4, 12.4}) == 24.0); // nearest cell, inside the edge cell
    CHECK(plot.valueAt({-0.4, 9.6}) == 0.0);
    CHECK(plot.valueAt({2.6, 10.4}) == 3.0);
    CHECK(!plot.valueAt({5, 11}).has_value()); // outside
    CHECK(!plot.valueAt({1, 11}).has_value()); // NaN cell

    // Autoscale frames the cell edges.
    const QRectF view = plot.viewRange();
    CHECK(view.left() == Approx(-0.5));
    CHECK(view.right() == Approx(4.5));
    CHECK(view.top() == Approx(9.5));
    CHECK(view.bottom() == Approx(12.5));

    const QPointF p = plot.dataToPixel({1.3, 10.7});
    CHECK(plot.plotArea().contains(p));
    const QPointF back = plot.pixelToData(p);
    CHECK(back.x() == Approx(1.3));
    CHECK(back.y() == Approx(10.7));
    // y grows upwards.
    CHECK(plot.dataToPixel({0, 12}).y() < plot.dataToPixel({0, 10}).y());

    // Descending axes are accepted: the first row is now at y = 12.
    plot.setImage(5, 3, 0, 4, 12, 10, values);
    CHECK(plot.valueAt({0, 12}) == 0.0);
    CHECK(plot.valueAt({4, 10}) == 24.0);
    // Mismatched sizes and empty data stay safe.
    plot.setImage(4, 4, -1, 1, -1, 1, {1.0, 2.0});
    CHECK(plot.valueAt({-1, -1}) == 1.0);
    CHECK(!plot.valueAt({1, 1}).has_value());
    plot.clearImage();
    CHECK(!plot.hasImage());
    CHECK(!plot.valueAt({0, 0}).has_value());
    CHECK(!plot.renderImage(QSize(300, 200)).isNull());
}

TEST_CASE("Colour-map aspect lock gives equal scales")
{
    ShownMap shown(QSize(900, 400));
    ColorMapPlot &plot = shown.plot;
    plot.setImage(3, 3, -1, 1, -1, 1, std::vector<double>(9, 1.0));
    plot.setRange(-3, 3, -1, 1);
    const auto scales = [&] {
        const QPointF o = plot.dataToPixel({0, 0});
        return std::pair{plot.dataToPixel({1, 0}).x() - o.x(), o.y() - plot.dataToPixel({0, 1}).y()};
    };
    plot.setAspectLocked(false);
    auto [kx, ky] = scales();
    CHECK(std::abs(kx - ky) > 5); // the free plot fills the widget
    plot.setAspectLocked(true);
    std::tie(kx, ky) = scales();
    CHECK(kx == Approx(ky).epsilon(1e-6));
    CHECK(plot.plotArea().width() / plot.plotArea().height() == Approx(3.0).epsilon(1e-6));
    // Zoom keeps the aspect.
    plot.zoomAt(plot.dataToPixel({0.5, 0.2}), 0.5);
    std::tie(kx, ky) = scales();
    CHECK(kx == Approx(ky).epsilon(1e-6));
    CHECK(!plot.isAutoscaled());
    QTest::mouseDClick(&plot, Qt::LeftButton, {}, pt(plot.plotArea().center()));
    CHECK(plot.isAutoscaled());
    // The colour bar sits right of the plot.
    CHECK(plot.colorBarArea().left() > plot.plotArea().right());
}

TEST_CASE("Colour-map scatter read-out and clicks")
{
    ShownMap shown(QSize(600, 600));
    ColorMapPlot &plot = shown.plot;
    plot.setAspectLocked(true);
    plot.setXAxis(QStringLiteral("x"), QStringLiteral("m"));
    plot.setYAxis(QStringLiteral("y"), QStringLiteral("m"));
    plot.setColorBar(QStringLiteral("Amplitude"), QStringLiteral("dB"));
    std::vector<QPointF> positions;
    std::vector<double> values;
    for (int i = 0; i < 16; ++i) {
        positions.emplace_back(i % 4, i / 4);
        values.push_back(-i);
    }
    values[9] = NAN; // hollow marker
    plot.setScatter(positions, values, 0.6);
    REQUIRE(plot.scatterCount() == 16);

    CHECK(plot.valueAt(positions[5]) == -5.0);
    CHECK(plot.valueAt(positions[5] + QPointF(0.2, -0.1)) == -5.0); // inside the marker
    CHECK(!plot.valueAt({0.5, 0.5}).has_value());                    // between markers
    CHECK(!plot.valueAt(positions[9]).has_value());
    CHECK(plot.scatterIndexAt(plot.dataToPixel(positions[9])) == 9);

    QSignalSpy clicks(&plot, &ColorMapPlot::pointClicked);
    QTest::mouseClick(&plot, Qt::LeftButton, {}, pt(plot.dataToPixel(positions[6])));
    REQUIRE(clicks.count() == 1);
    CHECK(clicks[0][1].toInt() == 6);
    const QPointF at = clicks[0][0].toPointF();
    CHECK(at.x() == Approx(2).margin(0.02));
    CHECK(at.y() == Approx(1).margin(0.02));
    QTest::mouseClick(&plot, Qt::LeftButton, {}, pt(plot.dataToPixel({0.5, 0.5})));
    REQUIRE(clicks.count() == 2);
    CHECK(clicks[1][1].toInt() == -1);

    QSignalSpy hover(&plot, &ColorMapPlot::hoverReadoutChanged);
    QTest::mouseMove(&plot, pt(plot.dataToPixel(positions[6])));
    QApplication::processEvents();
    CHECK(plot.hoverText().contains(QStringLiteral("[#6]")));
    CHECK(plot.hoverText().endsWith(QStringLiteral(": -6 dB")));
    CHECK(plot.hoverText().startsWith(QStringLiteral("x 2 m, y 1 m")));
    CHECK(hover.count() >= 1);

    plot.setScatterCrossed({3, 99, -1}); // out-of-range indices are ignored
    CHECK(!plot.renderImage(QSize(400, 400)).isNull());
    const QStringList lines = plot.csvText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    REQUIRE(lines.size() == 17);
    CHECK(lines[0] == QStringLiteral("x (m),y (m),Amplitude (dB)"));
    CHECK(lines[7] == QStringLiteral("2,1,-6"));
    CHECK(lines[10] == QStringLiteral("1,2,")); // NaN value
}

TEST_CASE("Colour-map overlays are added and removed")
{
    ShownMap shown;
    ColorMapPlot &plot = shown.plot;
    // Overlays alone frame an empty plot.
    const int circle = plot.addCircle({0, 0}, 1, {}, QStringLiteral("Visible"));
    CHECK(plot.viewRange().left() == Approx(-1));
    CHECK(plot.viewRange().right() == Approx(1));
    const int line = plot.addPolyline({{0, 0}, {0.5, 0.5}, {NAN, NAN}, {0.6, 0.1}},
                                      {QColor(Qt::red), Qt::DashLine, 1.0}, QStringLiteral("-3 dB"));
    const int marker = plot.addMarker({0.3, 0}, QStringLiteral("Beam"));
    CHECK(circle != line);
    CHECK(line != marker);
    CHECK(plot.overlayIds() == std::vector<int>{circle, line, marker});
    CHECK(!plot.renderImage(QSize(500, 400)).isNull());
    plot.removeOverlay(line);
    CHECK(plot.overlayIds() == std::vector<int>{circle, marker});
    plot.removeOverlay(12345); // unknown ids are ignored
    CHECK(plot.overlayIds().size() == 2);
    // With data, far overlays (grating-lobe circles) do not widen the view.
    plot.setImage(3, 3, -1, 1, -1, 1, std::vector<double>(9, 0.0));
    plot.addCircle({2.4, 0}, 1, {}, QStringLiteral("GL"));
    CHECK(plot.viewRange().right() < 2);
    plot.clearOverlays();
    CHECK(plot.overlayIds().empty());
}

TEST_CASE("Colour-map auto range ignores NaN and handles flat data")
{
    ColorMapPlot plot;
    CHECK(plot.colorRange() == std::pair{0.0, 1.0}); // no data
    plot.setImage(3, 2, 0, 2, 0, 1, {NAN, 1.0, 5.0, NAN, -2.0, INFINITY});
    CHECK(plot.isColorAutoRange());
    CHECK(plot.colorRange() == std::pair{-2.0, 5.0});
    plot.setScatter(std::vector<QPointF>{QPointF(0, 0)}, std::vector<double>{9.0});
    CHECK(plot.colorRange() == std::pair{-2.0, 9.0}); // image and scatter together
    plot.setColorRange(10, 0);
    CHECK(!plot.isColorAutoRange());
    CHECK(plot.colorRange() == std::pair{0.0, 10.0});
    plot.setColorAutoRange();
    CHECK(plot.colorRange() == std::pair{-2.0, 9.0});

    plot.clearScatter();
    plot.setImage(2, 2, 0, 1, 0, 1, std::vector<double>(4, NAN));
    CHECK(plot.colorRange() == std::pair{0.0, 1.0});
    CHECK(!plot.renderImage(QSize(300, 200)).isNull());
    plot.setImage(2, 2, 0, 1, 0, 1, std::vector<double>(4, 3.0));
    const auto [lo, hi] = plot.colorRange();
    CHECK(lo < 3.0);
    CHECK(hi > 3.0);
    CHECK(!plot.renderImage(QSize(300, 200)).isNull());
    // A single cell and a single scatter point still give a usable view.
    plot.setImage(1, 1, 5, 5, 5, 5, {1.0});
    CHECK(plot.viewRange().width() > 0);
    CHECK(plot.valueAt({5, 5}) == 1.0);
}

TEST_CASE("Colour-map export, CSV and theme")
{
    ColorMapPlot plot;
    plot.setTitle(QStringLiteral("UV pattern"));
    plot.setXAxis(QStringLiteral("u"), {});
    plot.setYAxis(QStringLiteral("v"), {});
    plot.setColorBar(QStringLiteral("Gain"), QStringLiteral("dBi"));
    plot.setImage(2, 2, -1, 1, 0, 1, {1.0, 2.0, NAN, 4.5});
    const QImage image = plot.renderImage(QSize(640, 480), 2.0);
    CHECK(image.size() == QSize(1280, 960));
    QTemporaryDir dir;
    CHECK(plot.exportImage(dir.filePath(QStringLiteral("map.png")), QSize(500, 400)));
    CHECK(QImage(dir.filePath(QStringLiteral("map.png"))).size() == QSize(500, 400));

    const QStringList lines = plot.csvText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == QStringLiteral("u,v,Gain (dBi)"));
    CHECK(lines[1] == QStringLiteral("-1,0,1"));
    CHECK(lines[2] == QStringLiteral("1,0,2"));
    CHECK(lines[3] == QStringLiteral("-1,1,"));
    CHECK(lines[4] == QStringLiteral("1,1,4.5"));
    REQUIRE(plot.exportCsv(dir.filePath(QStringLiteral("map.csv"))));

    CHECK(plot.plotActions().size() == 5);
    plot.setColorMap(ColorMap::Jet);
    CHECK(plot.colorMap() == ColorMap::Jet);

    QPalette dark;
    dark.setColor(QPalette::Window, QColor(30, 30, 30));
    dark.setColor(QPalette::Base, QColor(20, 20, 20));
    dark.setColor(QPalette::WindowText, QColor(230, 230, 230));
    plot.setPalette(dark);
    CHECK(plot.theme().dark);
}

TEST_CASE("A 401 x 401 UV map repaints quickly")
{
    ColorMapPlot plot;
    plot.setAspectLocked(true);
    plot.setColorBar(QStringLiteral("Gain"), QStringLiteral("dBi"));
    const int n = 401;
    auto values = uvPattern(n);
    QElapsedTimer timer;
    timer.start();
    plot.setImage(n, n, -1, 1, -1, 1, std::move(values));
    plot.addCircle({0, 0}, 1);
    const double setMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;

    QImage image(1000, 800, QImage::Format_ARGB32_Premultiplied);
    {
        QPainter warm(&image);
        plot.render(&warm, QRectF(0, 0, 1000, 800)); // builds the cached raster
    }
    const int frames = 10;
    timer.restart();
    for (int f = 0; f < frames; ++f) {
        QPainter painter(&image);
        plot.render(&painter, QRectF(0, 0, 1000, 800));
    }
    const double frameMs = static_cast<double>(timer.nsecsElapsed()) / 1e6 / frames;
    plot.setColorMap(ColorMap::Jet); // rebuilds the raster
    timer.restart();
    {
        QPainter painter(&image);
        plot.render(&painter, QRectF(0, 0, 1000, 800));
    }
    const double rebuildMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    std::printf("PERF 401x401 colour map: setImage %.1f ms, frame %.1f ms, frame with raster rebuild %.1f ms\n",
                setMs, frameMs, rebuildMs);
    CHECK(frameMs < 50);
    CHECK(rebuildMs < 50);
}

// Not run by default ("[.demo]"): renders example colour maps for a visual
// check. Output folder: LVD_COLORMAP_DEMO_DIR, else the working folder.
TEST_CASE("Colour-map demo images", "[.demo]")
{
    const QString folder = qEnvironmentVariableIsSet("LVD_COLORMAP_DEMO_DIR")
                               ? qEnvironmentVariable("LVD_COLORMAP_DEMO_DIR")
                               : QStringLiteral(".");
    for (const bool dark : {false, true}) {
        QPalette palette = QApplication::palette();
        if (dark) {
            palette.setColor(QPalette::Window, QColor(0x2B, 0x2B, 0x2A));
            palette.setColor(QPalette::Base, QColor(0x1E, 0x1E, 0x1D));
            palette.setColor(QPalette::WindowText, QColor(0xE6, 0xE5, 0xE0));
        }
        const QString suffix = dark ? QStringLiteral("_dark.png") : QStringLiteral("_light.png");

        ColorMapPlot uv;
        uv.setPalette(palette);
        uv.setTitle(QStringLiteral("8 x 8 array, d = 0.7 wavelength, steered to u = 0.3"));
        uv.setXAxis(QStringLiteral("u"), {});
        uv.setYAxis(QStringLiteral("v"), {});
        uv.setAspectLocked(true);
        uv.setColorBar(QStringLiteral("Gain"), QStringLiteral("dBi"));
        uv.setImage(401, 401, -1, 1, -1, 1, uvPattern(401));
        uv.setColorRange(-19, 21);
        uv.addCircle({0, 0}, 1, {}, QStringLiteral("Visible region"));
        const double g = 1.0 / 0.7;
        uv.addCircle({0.3 - g, 0}, 1, {QColor(0xE3, 0x49, 0x48), Qt::DashLine, 1.5}, QStringLiteral("Grating lobe"));
        uv.addCircle({0.3 + g, 0}, 1, {QColor(0xE3, 0x49, 0x48), Qt::DashLine, 1.5}, QStringLiteral("Grating lobe"));
        std::vector<QPointF> contour;
        for (int i = 0; i <= 64; ++i) {
            const double a = 2 * std::numbers::pi * i / 64;
            contour.emplace_back(0.3 + 0.09 * std::cos(a), 0.09 * std::sin(a));
        }
        uv.addPolyline(contour, {QColor(Qt::white), Qt::SolidLine, 1.0}, QStringLiteral("-3 dB"));
        uv.addMarker({0.3, 0}, QStringLiteral("Beam"));
        CHECK(uv.exportImage(folder + QStringLiteral("/colormap_uv") + suffix, QSize(760, 600)));

        ColorMapPlot layout;
        layout.setPalette(palette);
        layout.setTitle(QStringLiteral("16 x 16 element layout"));
        layout.setXAxis(QStringLiteral("x"), QStringLiteral("m"));
        layout.setYAxis(QStringLiteral("y"), QStringLiteral("m"));
        layout.setAspectLocked(true);
        layout.setColorBar(QStringLiteral("Amplitude"), QStringLiteral("dB"));
        std::vector<QPointF> positions;
        std::vector<double> amplitudes;
        const double spacing = 0.0625;
        for (int iy = 0; iy < 16; ++iy) {
            for (int ix = 0; ix < 16; ++ix) {
                const double x = (ix - 7.5) * spacing;
                const double y = (iy - 7.5) * spacing;
                positions.emplace_back(x, y);
                const double r = std::hypot(x, y) / (8 * spacing * std::sqrt(2.0));
                amplitudes.push_back(20 * std::log10(0.3 + 0.7 * std::cos(r * std::numbers::pi / 2)));
            }
        }
        amplitudes[40] = NAN;
        layout.setScatter(positions, amplitudes, spacing * 0.8);
        layout.setScatterCrossed({17, 100, 201});
        layout.addMarker({0, 0}, QStringLiteral("Phase centre"));
        CHECK(layout.exportImage(folder + QStringLiteral("/colormap_layout") + suffix, QSize(700, 600)));

        ColorMapPlot raster;
        raster.setPalette(palette);
        raster.setTitle(QStringLiteral("Pattern over theta and phi"));
        raster.setXAxis(QStringLiteral("Phi"), QStringLiteral("deg"));
        raster.setYAxis(QStringLiteral("Theta"), QStringLiteral("deg"));
        raster.setColorBar(QStringLiteral("Gain"), QStringLiteral("dBi"));
        raster.setColorMap(ColorMap::Jet);
        const int nphi = 361;
        const int ntheta = 181;
        std::vector<double> values;
        for (int it = 0; it < ntheta; ++it) {
            for (int ip = 0; ip < nphi; ++ip) {
                const double theta = it * std::numbers::pi / 180;
                const double phi = (ip - 180) * std::numbers::pi / 180;
                const double u = std::sin(theta) * std::cos(phi);
                const double v = std::sin(theta) * std::sin(phi);
                const double af = arrayFactor(u, 0.3, 8, 0.7) * arrayFactor(v, 0.0, 8, 0.7);
                values.push_back(std::max(-40.0, 20 * std::log10(std::max(af, 1e-6))) + 21.0
                                 + (theta > std::numbers::pi / 2 ? -15.0 : 0.0));
            }
        }
        raster.setImage(nphi, ntheta, -180, 180, 0, 180, std::move(values));
        CHECK(raster.exportImage(folder + QStringLiteral("/colormap_thetaphi") + suffix, QSize(800, 450)));
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("plot_tests"));
    return Catch::Session().run(argc, argv);
}

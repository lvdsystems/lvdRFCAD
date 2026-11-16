// Renders example RF plots on the native platform, saves screenshots and
// quits. Not a test: used to check the look in the light and dark themes.
//
// Usage: plot_demo <output folder> [light|dark]

#include "plot/PlotMath.h"
#include "plot/PlotWidget.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QStyleHints>
#include <QThread>

#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace lvd;

namespace {

constexpr double kPi = std::numbers::pi;

void settle()
{
    for (int i = 0; i < 10; ++i) {
        QApplication::processEvents();
        QThread::msleep(20);
    }
}

bool save(QWidget &widget, const QString &path)
{
    settle();
    const bool ok = widget.grab().save(path);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", qPrintable(QDir::toNativeSeparators(path)));
    return ok;
}

std::vector<double> linspace(double from, double to, int n)
{
    std::vector<double> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = from + (to - from) * i / (n - 1);
    return v;
}

std::vector<double> logspace(double from, double to, int n)
{
    std::vector<double> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = from * std::pow(to / from, static_cast<double>(i) / (n - 1));
    return v;
}

// Chebyshev polynomial for |x| > 1 too.
double chebyshev(int n, double x)
{
    if (std::abs(x) <= 1)
        return std::cos(n * std::acos(x));
    return (x > 0 ? 1 : (n % 2 ? -1 : 1)) * std::cosh(n * std::acosh(std::abs(x)));
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("plot_demo"));
    if (argc < 2) {
        std::fprintf(stderr, "usage: plot_demo <output folder> [light|dark]\n");
        return 2;
    }
    const QString output = QString::fromLocal8Bit(argv[1]);
    const QString theme = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("light");
    QGuiApplication::styleHints()->setColorScheme(theme == QLatin1String("dark") ? Qt::ColorScheme::Dark
                                                                                  : Qt::ColorScheme::Light);
    QDir().mkpath(output);
    const auto file = [&](const QString &name) { return QDir(output).filePath(QStringLiteral("%1-%2.png").arg(name, theme)); };
    bool ok = true;

    // 1. Bandpass filter: 5th-order 0.1 dB Chebyshev, 2.4 GHz, 10 % bandwidth.
    {
        PlotWidget plot;
        plot.setTitle(QStringLiteral("Bandpass filter, 2.4 GHz"));
        plot.setXAxis(QStringLiteral("Frequency"), QStringLiteral("Hz"));
        plot.setYAxis(QStringLiteral("Magnitude"), QStringLiteral("dB"));
        const auto f = linspace(1.8e9, 3.0e9, 1201);
        const double f0 = 2.4e9, fbw = 0.1;
        const double eps2 = std::pow(10.0, 0.1 / 10) - 1;
        std::vector<double> s21, s11;
        for (double v : f) {
            const double w = (v / f0 - f0 / v) / fbw;
            const double t = chebyshev(5, w);
            const double p = 1 / (1 + eps2 * t * t);
            s21.push_back(10 * std::log10(p));
            s11.push_back(10 * std::log10(std::max(1 - p, 1e-12)));
        }
        const int a = plot.addTrace(QStringLiteral("|S21|"), QStringLiteral("dB"));
        const int b = plot.addTrace(QStringLiteral("|S11|"), QStringLiteral("dB"));
        plot.setTraceData(a, f, s21);
        plot.setTraceData(b, f, s11);
        const int m1 = plot.addMarker(a, 2.4e9);
        plot.addMarker(a, 2.6e9);
        plot.setReferenceMarker(m1);
        plot.resize(900, 560);
        plot.show();
        ok = save(plot, file(QStringLiteral("bandpass"))) && ok;
        // Hover read-out and crosshair as the user sees them.
        QMouseEvent move(QEvent::MouseMove, plot.dataToPixel(2.25e9, -20), plot.mapToGlobal(plot.dataToPixel(2.25e9, -20)),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&plot, &move);
        ok = save(plot, file(QStringLiteral("bandpass-hover"))) && ok;
        ok = plot.exportImage(file(QStringLiteral("bandpass-export")), QSize(1200, 700)) && ok;
        if (PlotWidget::svgSupported())
            ok = plot.exportSvg(QDir(output).filePath(QStringLiteral("bandpass-%1.svg").arg(theme))) && ok;
    }

    // 2. Smith chart: S11 of a matched amplifier input, with a stability circle.
    {
        PlotWidget plot;
        plot.setKind(PlotKind::Smith);
        plot.setTitle(QStringLiteral("Amplifier input match"));
        const auto f = linspace(1.0e9, 4.0e9, 301);
        std::vector<std::complex<double>> s11, raw;
        for (double v : f) {
            const double w = 2 * kPi * v;
            // Transistor input: 12 ohm + 1.2 pF in series, matched at 2.4 GHz
            // by a series inductor and a shunt capacitor.
            const std::complex<double> j(0, 1);
            const std::complex<double> zin = 12.0 + 1.0 / (j * w * 1.2e-12);
            raw.push_back(gammaFromImpedance(zin, 50));
            // L network from 12 to 50 ohm: the series inductor also resonates
            // the input capacitance; the shunt capacitor is on the 50 ohm side.
            const double w0 = 2 * kPi * 2.4e9;
            const double q = std::sqrt(50.0 / 12 - 1);
            const double ls = 1 / (w0 * w0 * 1.2e-12) + q * 12 / w0;
            const double cp = q / 50 / w0;
            const std::complex<double> z1 = zin + j * w * ls;
            const std::complex<double> y1 = 1.0 / z1 + j * w * cp;
            s11.push_back(gammaFromImpedance(1.0 / y1, 50));
        }
        const int m = plot.addTrace(QStringLiteral("S11 matched"));
        const int r = plot.addTrace(QStringLiteral("S11 device"));
        plot.setTraceData(m, f, s11);
        plot.setTraceData(r, f, raw);
        plot.addMarker(m, 2.4e9);
        plot.addMarker(r, 2.4e9);
        PlotCircle stability;
        stability.name = QStringLiteral("Input stability");
        stability.centre = std::polar(1.45, 2.3);
        stability.radius = 0.62;
        stability.color = plot.theme().dark ? QColor(0xE6, 0x67, 0x67) : QColor(0xD0, 0x3B, 0x3B);
        plot.addCircle(stability);
        PlotCircle gain;
        gain.name = QStringLiteral("G = 14 dB");
        gain.centre = std::polar(0.35, -0.6);
        gain.radius = 0.3;
        gain.lineStyle = Qt::DotLine;
        plot.addCircle(gain);
        plot.resize(760, 760);
        plot.show();
        ok = save(plot, file(QStringLiteral("smith"))) && ok;
        plot.setAdmittanceGridVisible(true);
        plot.resize(1100, 1000);
        ok = save(plot, file(QStringLiteral("smith-admittance-large"))) && ok;
    }

    // 3. Polar antenna pattern: 4-element array of patches, dB, compass view.
    {
        PlotWidget plot;
        plot.setKind(PlotKind::Polar);
        plot.setTitle(QStringLiteral("Antenna pattern, 4 x 1 patch array"));
        plot.setPolarScale(PolarScale::Decibel, -30);
        plot.setPolarOrientation(PolarOrientation::Compass);
        const auto angle = linspace(0, 360, 721);
        std::vector<double> e, h;
        for (double deg : angle) {
            const double th = deg * kPi / 180;
            const double element = 0.05 + 0.95 * (1 + std::cos(th)) / 2; // patch with a small back lobe
            const double psi = kPi * std::sin(th);
            const double af = std::abs(std::sin(2 * psi) / (4 * std::sin(psi / 2) + 1e-12));
            const double afv = std::abs(std::sin(psi / 2)) < 1e-9 ? 1.0 : af;
            e.push_back(std::max(12 + 20 * std::log10(std::max(element * afv, 1e-6)), -40.0));
            h.push_back(std::max(6 + 20 * std::log10(std::max(std::pow(element, 1.5), 1e-6)),
                                 -40.0));
        }
        const int a = plot.addTrace(QStringLiteral("E-plane"), QStringLiteral("dBi"));
        const int b = plot.addTrace(QStringLiteral("H-plane"), QStringLiteral("dBi"));
        plot.setTraceData(a, angle, e);
        plot.setTraceData(b, angle, h);
        plot.addPeakMarker(a, true);
        plot.resize(720, 760);
        plot.show();
        ok = save(plot, file(QStringLiteral("polar"))) && ok;
    }

    // 4. Log-frequency: amplifier gain and noise figure, group delay on the right.
    {
        PlotWidget plot;
        plot.setTitle(QStringLiteral("LNA, 10 MHz to 10 GHz"));
        plot.setXAxis(QStringLiteral("Frequency"), QStringLiteral("Hz"), true);
        plot.setYAxis(QStringLiteral("Gain, NF"), QStringLiteral("dB"));
        plot.setYAxis(QStringLiteral("Group delay"), QStringLiteral("s"), false, PlotYAxis::Right);
        const auto f = logspace(1e7, 1e10, 801);
        std::vector<double> gain, nf, gd;
        for (double v : f) {
            const double lo = 1 / (1 + std::pow(5e7 / v, 2));
            const double hi = 1 / (1 + std::pow(v / 4e9, 4));
            gain.push_back(10 * std::log10(lo * hi * std::pow(10.0, 2.2)));
            nf.push_back(0.6 + 0.4 * std::pow(v / 4e9, 2) + 2.0 * std::pow(5e7 / v, 1.5));
            gd.push_back(0.15e-9 + 0.4e-9 * std::pow(v / 4e9, 2) / (1 + std::pow(v / 4e9, 4)) + 3e-9 * std::pow(5e7 / v, 2) / (1 + std::pow(5e7 / v, 2)));
        }
        const int g = plot.addTrace(QStringLiteral("Gain"), QStringLiteral("dB"));
        const int n = plot.addTrace(QStringLiteral("NF"), QStringLiteral("dB"));
        TraceStyle right;
        right.yAxis = PlotYAxis::Right;
        right.lineStyle = Qt::DashLine;
        const int d = plot.addTrace(QStringLiteral("Group delay"), QStringLiteral("s"), right);
        plot.setTraceData(g, f, gain);
        plot.setTraceData(n, f, nf);
        plot.setTraceData(d, f, gd);
        plot.addMarker(n, 1e9);
        plot.resize(900, 560);
        plot.show();
        ok = save(plot, file(QStringLiteral("logfreq"))) && ok;
    }

    // 5. Ten traces of 100k points (decimated), with a frame time.
    {
        PlotWidget plot;
        plot.setTitle(QStringLiteral("10 traces x 100 000 points"));
        plot.setXAxis(QStringLiteral("Time"), QStringLiteral("s"));
        plot.setYAxis(QStringLiteral("Voltage"), QStringLiteral("V"));
        const std::size_t count = 100000;
        std::vector<double> t(count), y(count);
        for (std::size_t i = 0; i < count; ++i)
            t[i] = static_cast<double>(i) * 1e-11;
        for (int k = 0; k < 10; ++k) {
            const int id = plot.addTrace(QStringLiteral("v%1").arg(k + 1), QStringLiteral("V"));
            for (std::size_t i = 0; i < count; ++i)
                y[i] = std::sin(2 * kPi * 1e6 * (k + 2) * t[i] + k) * (1 + 0.1 * k) + 0.05 * std::sin(static_cast<double>(i) * 0.9) + 0.5 * k;
            plot.setTraceData(id, t, y);
        }
        plot.resize(1000, 600);
        plot.show();
        settle();
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 20; ++i)
            plot.repaint();
        std::printf("native repaint 10x100k: %.1f ms per frame\n", static_cast<double>(timer.nsecsElapsed()) / 1e6 / 20);
        ok = save(plot, file(QStringLiteral("dense"))) && ok;
    }
    return ok ? 0 : 1;
}

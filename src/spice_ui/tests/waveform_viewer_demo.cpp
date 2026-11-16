// Shows the waveform viewer with synthetic SPICE results on the native
// platform, saves screenshots in the light and dark themes and quits. Not a
// test: used to check the look.
//
// Usage: waveform_viewer_demo <output folder>

#include "spice_ui/MeasurementPanel.h"
#include "spice_ui/WaveformPane.h"
#include "spice_ui/WaveformViewer.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QStyleHints>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <numbers>

using namespace lvd;
using namespace lvd::spiceui;
using lvd::spice::SpicePlot;
using lvd::spice::SpiceRunResult;
using lvd::spice::SpiceVector;
using lvd::spice::VectorKind;

namespace {

constexpr double kPi = std::numbers::pi;

void settle()
{
    for (int i = 0; i < 15; ++i) {
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

SpiceVector realVector(const QString &name, VectorKind kind, std::vector<double> values)
{
    SpiceVector v;
    v.name = name;
    v.kind = kind;
    v.real = std::move(values);
    return v;
}

// A class-D-like output stage: 1 kHz sine input, a filtered PWM output with
// some ringing, the supply current. 100k points.
std::shared_ptr<const SpiceRunResult> makeTran()
{
    const int n = 100001;
    const double tstop = 4e-3;
    std::vector<double> t(static_cast<std::size_t>(n)), in(t.size()), out(t.size()), clk(t.size()), i(t.size());
    for (std::size_t k = 0; k < t.size(); ++k) {
        const double x = tstop * double(k) / double(n - 1);
        t[k] = x;
        in[k] = 0.8 * std::sin(2 * kPi * 1e3 * x);
        // Output: sine with 3rd harmonic distortion and a decaying 40 kHz ripple.
        out[k] = 2.4 * std::sin(2 * kPi * 1e3 * x) + 0.12 * std::sin(3 * 2 * kPi * 1e3 * x)
                 + 0.05 * std::sin(2 * kPi * 40e3 * x) * std::exp(-x / 1.5e-3);
        // Square clock 10 kHz with 2 us edges and overshoot.
        const double phase = std::fmod(x * 1e4, 1.0);
        double sq = phase < 0.5 ? 3.3 : 0.0;
        const double since = phase < 0.5 ? phase / 1e4 : (phase - 0.5) / 1e4;
        sq += (phase < 0.5 ? -3.3 : 3.3) * std::exp(-since / 0.4e-6) * std::cos(2 * kPi * 1.5e6 * since);
        clk[k] = sq;
        i[k] = -(0.15 + 0.02 * out[k] * out[k]);
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
    plot.vectors.push_back(realVector(QStringLiteral("v(clk)"), VectorKind::Voltage, clk));
    plot.vectors.push_back(realVector(QStringLiteral("i(vcc)"), VectorKind::Current, i));
    for (int k = 1; k <= 12; ++k)
        plot.vectors.push_back(realVector(QStringLiteral("v(n%1)").arg(k), VectorKind::Voltage,
                                          std::vector<double>(t.size(), 0.1 * k)));
    result->plots << plot;
    return result;
}

// Second-order band pass around 10 kHz, 10 Hz to 10 MHz.
std::shared_ptr<const SpiceRunResult> makeAc()
{
    auto result = std::make_shared<SpiceRunResult>();
    result->ok = true;
    SpicePlot plot;
    plot.name = QStringLiteral("ac1");
    plot.type = QStringLiteral("ac");
    plot.scaleName = QStringLiteral("frequency");
    std::vector<double> f;
    SpiceVector out, in;
    out.name = QStringLiteral("v(out)");
    in.name = QStringLiteral("v(in)");
    out.kind = in.kind = VectorKind::Voltage;
    out.isComplex = in.isComplex = true;
    for (int k = 0; k <= 600; ++k) {
        const double x = 10 * std::pow(10.0, k / 100.0);
        f.push_back(x);
        const std::complex<double> s(0, x / 1e4);
        out.complex.push_back(10.0 * s / 2.0 / (s * s + s / 2.0 + 1.0));
        in.complex.push_back(1.0);
    }
    plot.vectors.push_back(realVector(QStringLiteral("frequency"), VectorKind::Frequency, f));
    plot.vectors.push_back(in);
    plot.vectors.push_back(out);
    result->plots << plot;
    return result;
}

std::shared_ptr<const SpiceRunResult> makeOp()
{
    auto result = std::make_shared<SpiceRunResult>();
    SpicePlot plot;
    plot.name = QStringLiteral("op1");
    plot.type = QStringLiteral("op");
    plot.vectors.push_back(realVector(QStringLiteral("v(out)"), VectorKind::Voltage, {2.5}));
    result->plots << plot;
    return result;
}

WaveformPaneDefinition pane(const QString &id, const QStringList &traces)
{
    WaveformPaneDefinition d;
    d.id = id;
    d.traces = traces;
    return d;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("waveform_viewer_demo"));
    if (argc < 2) {
        std::fprintf(stderr, "usage: waveform_viewer_demo <output folder>\n");
        return 2;
    }
    const QString output = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(output);
    bool ok = true;
    const auto tran = makeTran();
    const auto ac = makeAc();

    for (const QString theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
        QGuiApplication::styleHints()->setColorScheme(theme == QLatin1String("dark") ? Qt::ColorScheme::Dark
                                                                                      : Qt::ColorScheme::Light);
        settle();
        const auto file = [&](const QString &name) {
            return QDir(output).filePath(QStringLiteral("%1-%2.png").arg(name, theme));
        };

        // 1. Transient: three panes, cursors on the output, an FFT pane.
        {
            WaveformViewer viewer;
            viewer.setWindowTitle(QStringLiteral("Waveform viewer"));
            viewer.resize(1400, 900);
            viewer.setData(tran, 0);
            viewer.setPanes({pane(QStringLiteral("p1"), {QStringLiteral("v(in)"), QStringLiteral("v(out)"),
                                                         QStringLiteral("i(vcc)*1000"), QStringLiteral("v(nope)")}),
                             pane(QStringLiteral("p2"), {QStringLiteral("v(clk)"), QStringLiteral("i(vcc)")})});
            viewer.show();
            settle();
            WaveformPane *p1 = viewer.pane(QStringLiteral("p1"));
            p1->setSelectedTrace(1);
            p1->setCursorX(0, 0.25e-3);
            p1->setCursorX(1, 1.25e-3);
            viewer.fftOfSelectedTrace();
            if (WaveformPane *fft = viewer.currentPane())
                fft->plot()->setXRange(0, 20e3);
            viewer.setCurrentPane(QStringLiteral("p1"));
            ok = save(viewer, file(QStringLiteral("waveform-tran"))) && ok;

            // Zoomed on the clock edge, square wave measurements.
            viewer.pane(QStringLiteral("p2"))->plot()->setXRange(0.045e-3, 0.065e-3);
            viewer.setCurrentPane(QStringLiteral("p2"));
            ok = save(viewer, file(QStringLiteral("waveform-tran-zoom"))) && ok;
            if (QDialog *dialog = viewer.measurementPanel()->showFourierDialog()) {
                viewer.setCurrentPane(QStringLiteral("p1"));
                dialog = viewer.measurementPanel()->showFourierDialog();
                ok = save(*dialog, file(QStringLiteral("waveform-fourier"))) && ok;
                dialog->close();
            }
        }

        // 2. AC: magnitude in dB and phase on the right axis.
        {
            WaveformViewer viewer;
            viewer.resize(1200, 700);
            viewer.setData(ac, 0);
            WaveformPaneDefinition bode = pane(QStringLiteral("p1"), {QStringLiteral("v(out)"),
                                                                     QStringLiteral("v(out)/v(in)")});
            bode.logX = true;
            bode.name = QStringLiteral("Band pass");
            bode.complexParts.insert(QStringLiteral("v(out)/v(in)"), QStringLiteral("phase"));
            viewer.setPanes({bode});
            viewer.show();
            settle();
            viewer.currentPane()->showCursor(0, true);
            viewer.currentPane()->showCursor(1, true);
            ok = save(viewer, file(QStringLiteral("waveform-ac"))) && ok;
        }

        // 3. An operating point: placeholder.
        {
            WaveformViewer viewer;
            viewer.resize(800, 400);
            viewer.setData(makeOp(), 0);
            viewer.show();
            ok = save(viewer, file(QStringLiteral("waveform-op"))) && ok;
        }
    }
    return ok ? 0 : 1;
}

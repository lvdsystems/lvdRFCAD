#include "spice_ui/InstrumentsView.h"

#include "core/Connectivity.h"
#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "plot/PlotWidget.h"
#include "spice/Fourier.h"
#include "spice/SpiceWaveforms.h"
#include "spice/WaveformMath.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <cmath>

namespace lvd::spiceui {

namespace {

// "v(a,b)" with ground written as 0, which the deck resolver understands.
QString differential(const spice::SpiceElement &element, const char *pin, const char *reference)
{
    const QString a = element.pinNodes.value(QString::fromLatin1(pin));
    const QString b = element.pinNodes.value(QString::fromLatin1(reference));
    return b.isEmpty() || b == QLatin1String("0") ? QStringLiteral("v(%1)").arg(a) : QStringLiteral("v(%1,%2)").arg(a, b);
}

const spice::SpicePlot *plotOf(const std::shared_ptr<const spice::SpiceSimulation> &simulation, const char *type)
{
    if (!simulation)
        return nullptr;
    for (const spice::SpicePlot &plot : simulation->run.plots)
        if (plot.type == QLatin1String(type) && (plot.scale() || plot.type == QLatin1String("op")))
            return &plot;
    return nullptr;
}

} // namespace

InstrumentsView::InstrumentsView(Project *project, Document *document, QWidget *parent)
    : QWidget(parent), m_project(project), m_document(document)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_placeholder = new QLabel(tr("Place instruments from the \"Instruments\" category of the spice-instruments library "
                                  "(oscilloscope, voltmeter, ammeter, function generator, spectrum analyser, Bode plotter); "
                                  "they show the results of the analyses here."),
                               this);
    m_placeholder->setWordWrap(true);
    m_placeholder->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_placeholder);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_host = new QWidget(m_scroll);
    m_grid = new QGridLayout(m_host);
    m_grid->setColumnStretch(0, 1);
    m_grid->setColumnStretch(1, 1);
    m_grid->setAlignment(Qt::AlignTop);
    m_scroll->setWidget(m_host);
    m_scroll->hide();
    layout->addWidget(m_scroll, 1);
}

void InstrumentsView::clear()
{
    m_latest.clear();
    m_newest.reset();
    rebuild();
}

void InstrumentsView::addSimulation(std::shared_ptr<const spice::SpiceSimulation> simulation)
{
    if (!simulation)
        return;
    for (const spice::SpicePlot &plot : simulation->run.plots)
        if (plot.type == QLatin1String("op") || plot.type == QLatin1String("tran") || plot.type == QLatin1String("ac"))
            m_latest.insert(plot.type, simulation);
    m_newest = simulation;
    rebuild();
}

QStringList InstrumentsView::instrumentReferences() const
{
    QStringList list = m_panels.keys();
    list.sort();
    return list;
}

PlotWidget *InstrumentsView::plot(const QString &reference) const
{
    return m_panels.value(reference).plot;
}

QString InstrumentsView::readout(const QString &reference) const
{
    const QLabel *label = m_panels.value(reference).readout;
    return label ? label->text() : QString();
}

QString InstrumentsView::parameterText(const QString &componentId, const QString &name) const
{
    const Schematic *schematic = m_document->schematic();
    const PlacedComponent *component = schematic ? schematic->component(componentId) : nullptr;
    if (!component)
        return {};
    const DefinitionPtr definition = m_project->resolve(*schematic, component->definition).definition;
    return definition ? lvd::parameterText(*component, *definition, name) : QString();
}

double InstrumentsView::parameter(const QString &componentId, const QString &name, double fallback) const
{
    const QString text = parameterText(componentId, name);
    if (text.isEmpty())
        return fallback;
    const auto value = Expression::parse(text).evaluate(spice::sheetVariables(*m_document));
    return value ? *value : fallback;
}

void InstrumentsView::rebuild()
{
    for (const Panel &panel : std::as_const(m_panels))
        delete panel.box;
    m_panels.clear();
    if (!m_newest) {
        m_scroll->hide();
        m_placeholder->show();
        return;
    }
    QList<const spice::SpiceElement *> instruments;
    for (const spice::SpiceElement &element : m_newest->deck.elements)
        if (!element.instrument.isEmpty() && element.document == m_document->name())
            instruments << &element;
    // Meters first (they share a row), then the plotting instruments.
    const auto isMeter = [](const spice::SpiceElement *e) {
        return e->instrument == QLatin1String("voltmeter") || e->instrument == QLatin1String("ammeter");
    };
    std::sort(instruments.begin(), instruments.end(), [&](const spice::SpiceElement *a, const spice::SpiceElement *b) {
        if (isMeter(a) != isMeter(b))
            return isMeter(a);
        return a->path < b->path;
    });
    m_placeholder->setVisible(instruments.isEmpty());
    m_scroll->setVisible(!instruments.isEmpty());
    int index = 0;
    for (const spice::SpiceElement *element : std::as_const(instruments)) {
        Panel panel;
        auto *box = new QGroupBox(m_host);
        auto *layout = new QVBoxLayout(box);
        panel.box = box;
        panel.readout = new QLabel(box);
        panel.readout->setWordWrap(true);
        panel.readout->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(panel.readout);
        const QString kind = element->instrument;
        const bool meter = kind == QLatin1String("voltmeter") || kind == QLatin1String("ammeter");
        if (!meter) {
            panel.plot = new PlotWidget(box);
            panel.plot->setMarkerTableVisible(false);
            panel.plot->setMinimumHeight(220);
            layout->addWidget(panel.plot, 1);
        }
        if (kind == QLatin1String("oscilloscope")) {
            box->setTitle(tr("%1 — oscilloscope").arg(element->path));
            fillOscilloscope(panel, *element);
        } else if (kind == QLatin1String("voltmeter")) {
            box->setTitle(tr("%1 — voltmeter").arg(element->path));
            fillMeter(panel, *element, false);
        } else if (kind == QLatin1String("ammeter")) {
            box->setTitle(tr("%1 — ammeter").arg(element->path));
            fillMeter(panel, *element, true);
        } else if (kind == QLatin1String("function-generator")) {
            box->setTitle(tr("%1 — function generator").arg(element->path));
            fillGenerator(panel, *element);
        } else if (kind == QLatin1String("spectrum-analyzer")) {
            box->setTitle(tr("%1 — spectrum analyser").arg(element->path));
            fillSpectrum(panel, *element);
        } else if (kind == QLatin1String("bode-plotter")) {
            box->setTitle(tr("%1 — Bode plotter").arg(element->path));
            fillBode(panel, *element);
            if (!plotOf(m_latest.value(QStringLiteral("ac")), "ac")) {
                auto *button = new QPushButton(tr("Add an AC Analysis"), box);
                connect(button, &QPushButton::clicked, this, &InstrumentsView::acAnalysisRequested);
                layout->addWidget(button, 0, Qt::AlignLeft);
            }
        }
        // Two columns; meters are small, so they share rows too.
        m_grid->addWidget(box, index / 2, index % 2);
        ++index;
        m_panels.insert(element->path, panel);
    }
}

void InstrumentsView::fillOscilloscope(Panel &panel, const spice::SpiceElement &element)
{
    const auto simulation = m_latest.value(QStringLiteral("tran"));
    const spice::SpicePlot *tran = plotOf(simulation, "tran");
    const double tdiv = parameter(element.componentId, QStringLiteral("tdiv"), 1e-4);
    const double vdiv1 = parameter(element.componentId, QStringLiteral("vdiv1"), 1);
    const double vdiv2 = parameter(element.componentId, QStringLiteral("vdiv2"), 1);
    const QString triggerSource = parameterText(element.componentId, QStringLiteral("trigger"));
    const double level = parameter(element.componentId, QStringLiteral("level"), 0);
    PlotWidget *p = panel.plot;
    p->setXAxis(tr("Time"), QStringLiteral("s"));
    p->setYAxis(tr("Channel 1"), QStringLiteral("V"));
    p->setYAxis(tr("Channel 2"), QStringLiteral("V"), false, PlotYAxis::Right);
    if (!tran) {
        panel.readout->setText(tr("Run a transient analysis; %1/div, CH1 %2/div, CH2 %3/div.")
                                   .arg(formatEngineering(tdiv, QStringLiteral("s")), formatEngineering(vdiv1, QStringLiteral("V")),
                                        formatEngineering(vdiv2, QStringLiteral("V"))));
        return;
    }
    std::optional<spice::Waveform> channels[2];
    QStringList notes;
    for (int c = 0; c < 2; ++c) {
        const char *pin = c == 0 ? "ch1" : "ch2";
        if (element.pinNodes.value(QString::fromLatin1(pin)).startsWith(QLatin1String("nc_")))
            continue; // channel not connected
        QString error;
        channels[c] = spice::evaluateOnPlot(differential(element, pin, "ref"), *tran, &simulation->deck, &error);
        if (!channels[c])
            notes << error;
    }
    // Like a real scope, the latest sweep: the last rising crossing of the
    // trigger level that still leaves ten divisions of data; free running
    // (or without a crossing), the last ten divisions.
    const double start = tran->scale()->real.front();
    const double end = tran->scale()->real.back();
    double t0 = std::max(start, end - 10 * tdiv);
    const int triggerChannel = triggerSource == QLatin1String("ch2") ? 1 : 0;
    bool triggered = false;
    if (triggerSource != QLatin1String("none") && channels[triggerChannel]) {
        const std::vector<double> times = spice::crossings(*channels[triggerChannel], level, spice::Edge::Rising);
        for (auto it = times.rbegin(); it != times.rend(); ++it)
            if (*it + 10 * tdiv <= end || it + 1 == times.rend()) {
                t0 = *it;
                triggered = true;
                break;
            }
    }
    const double t1 = t0 + 10 * tdiv;
    const double offset1 = parameter(element.componentId, QStringLiteral("offset1"), 0);
    const double offset2 = parameter(element.componentId, QStringLiteral("offset2"), 0);
    for (int c = 0; c < 2; ++c) {
        if (!channels[c])
            continue;
        TraceStyle style;
        style.yAxis = c == 0 ? PlotYAxis::Left : PlotYAxis::Right;
        style.color = c == 0 ? QColor(0xf9, 0xa8, 0x25) : QColor(0x26, 0xa6, 0x9a);
        const int trace = p->addTrace(c == 0 ? tr("CH1") : tr("CH2"), QStringLiteral("V"), style);
        p->setTraceData(trace, channels[c]->x, channels[c]->real());
    }
    p->setXRange(t0, t1);
    p->setYRange(offset1 - 4 * vdiv1, offset1 + 4 * vdiv1, PlotYAxis::Left);
    p->setYRange(offset2 - 4 * vdiv2, offset2 + 4 * vdiv2, PlotYAxis::Right);
    QString text = tr("%1/div · CH1 %2/div · CH2 %3/div")
                       .arg(formatEngineering(tdiv, QStringLiteral("s")), formatEngineering(vdiv1, QStringLiteral("V")),
                            formatEngineering(vdiv2, QStringLiteral("V")));
    text += triggerSource == QLatin1String("none") ? tr(" · free run")
            : triggered ? tr(" · triggered on %1 rising through %2").arg(triggerSource.toUpper(), formatEngineering(level, QStringLiteral("V")))
                        : tr(" · no trigger (%1 never rises through %2)").arg(triggerSource.toUpper(), formatEngineering(level, QStringLiteral("V")));
    for (int c = 0; c < 2; ++c)
        if (channels[c]) {
            spice::XWindow window{t0, t1};
            const auto f = spice::frequency(*channels[c], window);
            const auto pp = spice::peakToPeak(*channels[c], window);
            text += QStringLiteral("\n%1: %2 pk-pk%3").arg(c == 0 ? tr("CH1") : tr("CH2"),
                                                           pp ? formatEngineering(*pp, QStringLiteral("V")) : QStringLiteral("—"),
                                                           f ? tr(", %1").arg(formatEngineering(*f, QStringLiteral("Hz"))) : QString());
        }
    if (!notes.isEmpty())
        text += QLatin1Char('\n') + notes.join(QLatin1Char('\n'));
    panel.readout->setText(text);
}

void InstrumentsView::fillMeter(Panel &panel, const spice::SpiceElement &element, bool current)
{
    const QString unit = current ? QStringLiteral("A") : QStringLiteral("V");
    const QString expression = current ? QStringLiteral("i(%1)").arg(element.name) : differential(element, "p", "n");
    QStringList lines;
    if (const auto simulation = m_latest.value(QStringLiteral("op"))) {
        if (const spice::SpicePlot *op = plotOf(simulation, "op")) {
            QString error;
            const auto w = spice::evaluateOnPlot(expression, *op, &simulation->deck, &error);
            // i(V) of the ammeter's 0 V source is the current entering pin p.
            lines << (w ? tr("DC (operating point)  <b>%1</b>").arg(formatEngineering(w->y.front().real(), unit, 5)) : error);
        }
    }
    if (const auto simulation = m_latest.value(QStringLiteral("tran"))) {
        if (const spice::SpicePlot *tran = plotOf(simulation, "tran")) {
            QString error;
            if (const auto w = spice::evaluateOnPlot(expression, *tran, &simulation->deck, &error)) {
                // Like a meter read after settling: the second half of the run.
                const spice::XWindow settled{(w->x.front() + w->x.back()) / 2, w->x.back()};
                const auto mean = spice::mean(*w, settled);
                const auto rms = spice::rms(*w, settled);
                if (mean && rms) {
                    const double ac = std::sqrt(std::max(0.0, *rms * *rms - *mean * *mean));
                    lines << tr("Transient, second half: DC <b>%1</b> · RMS <b>%2</b> · AC RMS <b>%3</b>")
                                 .arg(formatEngineering(*mean, unit, 5), formatEngineering(*rms, unit, 5),
                                      formatEngineering(ac, unit, 5));
                }
            } else {
                lines << error;
            }
        }
    }
    if (lines.isEmpty())
        lines << tr("Run an operating point or a transient analysis.");
    panel.readout->setTextFormat(Qt::RichText);
    panel.readout->setText(lines.join(QStringLiteral("<br>")));
}

void InstrumentsView::fillGenerator(Panel &panel, const spice::SpiceElement &element)
{
    const QString shape = parameterText(element.componentId, QStringLiteral("shape"));
    const double frequency = parameter(element.componentId, QStringLiteral("freq"), 1e3);
    const double amplitude = parameter(element.componentId, QStringLiteral("amplitude"), 1);
    const double offset = parameter(element.componentId, QStringLiteral("offset"), 0);
    panel.readout->setText(tr("%1, %2, %3 peak, offset %4, AC %5")
                               .arg(shape, formatEngineering(frequency, QStringLiteral("Hz")),
                                    formatEngineering(amplitude, QStringLiteral("V")), formatEngineering(offset, QStringLiteral("V")),
                                    formatEngineering(parameter(element.componentId, QStringLiteral("ac"), 1), QStringLiteral("V"))));
    PlotWidget *p = panel.plot;
    p->setXAxis(tr("Time"), QStringLiteral("s"));
    p->setYAxis(tr("Output"), QStringLiteral("V"));
    const auto simulation = m_latest.value(QStringLiteral("tran"));
    const spice::SpicePlot *tran = plotOf(simulation, "tran");
    if (!tran)
        return;
    QString error;
    if (const auto w = spice::evaluateOnPlot(differential(element, "p", "n"), *tran, &simulation->deck, &error)) {
        const int trace = p->addTrace(element.path, QStringLiteral("V"));
        p->setTraceData(trace, w->x, w->real());
        // Two periods from the start.
        p->setXRange(w->x.front(), w->x.front() + std::min(2.0 / frequency, w->x.back() - w->x.front()));
    }
}

void InstrumentsView::fillSpectrum(Panel &panel, const spice::SpiceElement &element)
{
    PlotWidget *p = panel.plot;
    p->setXAxis(tr("Frequency"), QStringLiteral("Hz"));
    p->setYAxis(tr("Amplitude"), QStringLiteral("dBV"));
    const auto simulation = m_latest.value(QStringLiteral("tran"));
    const spice::SpicePlot *tran = plotOf(simulation, "tran");
    if (!tran) {
        panel.readout->setText(tr("Run a transient analysis."));
        return;
    }
    QString error;
    const auto w = spice::evaluateOnPlot(differential(element, "in", "ref"), *tran, &simulation->deck, &error);
    if (!w) {
        panel.readout->setText(error);
        return;
    }
    spice::SpectrumOptions options;
    options.window = spice::windowFromName(parameterText(element.componentId, QStringLiteral("window")))
                         .value_or(spice::WindowType::Hann);
    const auto spectrum = spice::amplitudeSpectrum(*w, options, &error);
    if (!spectrum) {
        panel.readout->setText(error);
        return;
    }
    const double fmax = parameter(element.componentId, QStringLiteral("fmax"), 0);
    std::vector<double> f, db;
    double peak = -INFINITY, peakF = 0;
    for (std::size_t i = 1; i < spectrum->frequency.size(); ++i) {
        if (fmax > 0 && spectrum->frequency[i] > fmax)
            break;
        const double value = 20 * std::log10(std::max(std::abs(spectrum->value[i]), 1e-15));
        f.push_back(spectrum->frequency[i]);
        db.push_back(value);
        if (value > peak) {
            peak = value;
            peakF = spectrum->frequency[i];
        }
    }
    const int trace = p->addTrace(element.path, QStringLiteral("dBV"));
    p->setTraceData(trace, f, db);
    if (std::isfinite(peak))
        p->setYRange(peak - 100, peak + 10);
    panel.readout->setText(tr("Resolution %1 · window %2 · peak %3 dBV at %4")
                               .arg(formatEngineering(spectrum->resolution, QStringLiteral("Hz")), spice::windowName(options.window))
                               .arg(peak, 0, 'f', 1)
                               .arg(formatEngineering(peakF, QStringLiteral("Hz"))));
}

void InstrumentsView::fillBode(Panel &panel, const spice::SpiceElement &element)
{
    PlotWidget *p = panel.plot;
    p->setXAxis(tr("Frequency"), QStringLiteral("Hz"), true);
    p->setYAxis(tr("Gain"), QStringLiteral("dB"));
    p->setYAxis(tr("Phase"), QStringLiteral("°"), false, PlotYAxis::Right);
    const auto simulation = m_latest.value(QStringLiteral("ac"));
    const spice::SpicePlot *ac = plotOf(simulation, "ac");
    if (!ac) {
        panel.readout->setText(tr("Run an AC analysis (the circuit needs a source with an AC magnitude)."));
        return;
    }
    const QString ratio = QStringLiteral("%1/%2").arg(differential(element, "out", "ref"), differential(element, "in", "ref"));
    QString error;
    const auto gain = spice::evaluateOnPlot(QStringLiteral("db(%1)").arg(ratio), *ac, &simulation->deck, &error);
    const auto phase = spice::evaluateOnPlot(QStringLiteral("cph(%1)").arg(ratio), *ac, &simulation->deck, &error);
    if (!gain || !phase) {
        panel.readout->setText(error);
        return;
    }
    const int g = p->addTrace(tr("Gain"), QStringLiteral("dB"));
    p->setTraceData(g, gain->x, gain->real());
    TraceStyle style;
    style.yAxis = PlotYAxis::Right;
    style.lineStyle = Qt::DashLine;
    const int ph = p->addTrace(tr("Phase"), QStringLiteral("°"), style);
    p->setTraceData(ph, phase->x, phase->real());
    // -3 dB bandwidth relative to the first point.
    const std::vector<double> g0 = gain->real();
    QString bandwidth;
    for (std::size_t i = 1; i < g0.size(); ++i)
        if (g0[i] < g0.front() - 3.0) {
            bandwidth = tr(" · −3 dB at %1").arg(formatEngineering(gain->x[i], QStringLiteral("Hz")));
            break;
        }
    panel.readout->setText(tr("Gain at %1: %2 dB%3")
                               .arg(formatEngineering(gain->x.front(), QStringLiteral("Hz")))
                               .arg(g0.front(), 0, 'f', 2)
                               .arg(bandwidth));
}

} // namespace lvd::spiceui

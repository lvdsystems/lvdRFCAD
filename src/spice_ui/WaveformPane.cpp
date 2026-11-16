#include "spice_ui/WaveformPane.h"

#include "plot/PlotAxis.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lvd::spiceui {

using spice::Complex;
using spice::Waveform;

// ---------------------------------------------------------------- definition

QJsonObject WaveformPaneDefinition::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    if (!name.isEmpty())
        o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("traces"), QJsonArray::fromStringList(traces));
    o.insert(QStringLiteral("logX"), logX);
    if (!fftSource.isEmpty()) {
        o.insert(QStringLiteral("fftSource"), fftSource);
        o.insert(QStringLiteral("fftWindow"), fftWindow);
        o.insert(QStringLiteral("fftLinear"), fftLinear);
    }
    if (!complexParts.isEmpty()) {
        QJsonObject parts;
        for (auto it = complexParts.cbegin(); it != complexParts.cend(); ++it)
            parts.insert(it.key(), it.value());
        o.insert(QStringLiteral("complexParts"), parts);
    }
    return o;
}

WaveformPaneDefinition WaveformPaneDefinition::fromJson(const QJsonObject &o)
{
    WaveformPaneDefinition d;
    d.id = o.value(QStringLiteral("id")).toString();
    d.name = o.value(QStringLiteral("name")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("traces")).toArray()) {
        const QString text = v.toString().trimmed();
        if (!text.isEmpty())
            d.traces << text;
    }
    d.logX = o.value(QStringLiteral("logX")).toBool(false);
    d.fftSource = o.value(QStringLiteral("fftSource")).toString();
    d.fftWindow = o.value(QStringLiteral("fftWindow")).toString(QStringLiteral("hann"));
    d.fftLinear = o.value(QStringLiteral("fftLinear")).toBool(false);
    const QJsonObject parts = o.value(QStringLiteral("complexParts")).toObject();
    for (auto it = parts.begin(); it != parts.end(); ++it)
        d.complexParts.insert(it.key(), it.value().toString());
    return d;
}

QString fftWindowKey(spice::WindowType type)
{
    QString key = spice::windowName(type).toLower();
    key.remove(QLatin1Char(' '));
    key.remove(QLatin1Char('-'));
    return key;
}

spice::WindowType fftWindowFromKey(const QString &key)
{
    return spice::windowFromName(key).value_or(spice::WindowType::Hann);
}

QString complexPartKey(ComplexPart part)
{
    switch (part) {
    case ComplexPart::MagnitudeDb: return QStringLiteral("db");
    case ComplexPart::Magnitude: return QStringLiteral("mag");
    case ComplexPart::PhaseDeg: return QStringLiteral("phase");
    case ComplexPart::Real: return QStringLiteral("real");
    case ComplexPart::Imaginary: return QStringLiteral("imag");
    }
    return QStringLiteral("db");
}

ComplexPart complexPartFromKey(const QString &key)
{
    for (ComplexPart part : {ComplexPart::MagnitudeDb, ComplexPart::Magnitude, ComplexPart::PhaseDeg, ComplexPart::Real,
                             ComplexPart::Imaginary})
        if (complexPartKey(part) == key)
            return part;
    return ComplexPart::MagnitudeDb;
}

Waveform plotVectorWaveform(const spice::SpicePlot &plot, const spice::SpiceVector &vector)
{
    Waveform w;
    const spice::SpiceVector *scale = plot.scale();
    const std::size_t n = vector.size();
    if (scale && scale->size() == n && !scale->isComplex) {
        w.x = scale->real;
    } else if (scale && scale->size() == n) {
        w.x.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            w.x[i] = scale->complex[i].real(); // AC frequency stored as complex by some engines
    } else {
        w.x.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            w.x[i] = double(i);
    }
    w.isComplex = vector.isComplex;
    w.y.resize(n);
    if (vector.isComplex) {
        std::copy(vector.complex.begin(), vector.complex.end(), w.y.begin());
    } else {
        for (std::size_t i = 0; i < n; ++i)
            w.y[i] = Complex(vector.real[i], 0.0);
    }
    w.unit = spice::vectorKindUnit(vector.kind);
    return w;
}

namespace {

QString partLabel(ComplexPart part)
{
    switch (part) {
    case ComplexPart::MagnitudeDb: return WaveformPane::tr("Magnitude (dB)");
    case ComplexPart::Magnitude: return WaveformPane::tr("Magnitude");
    case ComplexPart::PhaseDeg: return WaveformPane::tr("Phase (degrees)");
    case ComplexPart::Real: return WaveformPane::tr("Real part");
    case ComplexPart::Imaginary: return WaveformPane::tr("Imaginary part");
    }
    return {};
}

QString axisLabel(const QString &unit)
{
    if (unit == QLatin1String("V"))
        return WaveformPane::tr("Voltage");
    if (unit == QLatin1String("A"))
        return WaveformPane::tr("Current");
    if (unit.startsWith(QLatin1String("dB")))
        return WaveformPane::tr("Magnitude");
    if (unit == QLatin1String("deg"))
        return WaveformPane::tr("Phase");
    if (unit == QLatin1String("W"))
        return WaveformPane::tr("Power");
    if (unit == QLatin1String("s"))
        return WaveformPane::tr("Time");
    if (unit == QLatin1String("Hz"))
        return WaveformPane::tr("Frequency");
    if (unit.contains(QStringLiteral("√Hz")))
        return WaveformPane::tr("Noise density");
    if (unit == QStringLiteral("Ω") || unit == QLatin1String("ohm"))
        return WaveformPane::tr("Impedance");
    if (unit == QLatin1String("S"))
        return WaveformPane::tr("Admittance");
    return WaveformPane::tr("Value");
}

QIcon swatch(const QColor &color)
{
    QPixmap pixmap(12, 12);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(1, 1, 10, 10), 2, 2);
    return QIcon(pixmap);
}

double shownValue(Complex z, ComplexPart part)
{
    switch (part) {
    case ComplexPart::MagnitudeDb: return 20.0 * std::log10(std::max(std::abs(z), 1e-300));
    case ComplexPart::Magnitude: return std::abs(z);
    case ComplexPart::PhaseDeg: return std::arg(z) * 180.0 / 3.14159265358979323846;
    case ComplexPart::Real: return z.real();
    case ComplexPart::Imaginary: return z.imag();
    }
    return z.real();
}

QString cursorName(int which)
{
    return which == 0 ? QStringLiteral("A") : QStringLiteral("B");
}

} // namespace

// ---------------------------------------------------------------- overlay

// Vertical lines with an "A" / "B" tag at the cursors, over the plot.
class CursorOverlay : public QWidget
{
public:
    CursorOverlay(WaveformPane *pane, PlotWidget *plot) : QWidget(plot), m_pane(pane), m_plot(plot)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(plot->rect());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const QRectF area = m_plot->plotArea();
        if (area.isEmpty())
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const PlotTheme &theme = m_plot->theme();
        QFont font = this->font();
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() * 0.85);
        p.setFont(font);
        const QFontMetricsF fm(font);
        for (int which = 0; which < 2; ++which) {
            const auto x = m_pane->cursorX(which);
            if (!x)
                continue;
            const double px = m_plot->dataToPixel(*x, 0).x();
            if (!std::isfinite(px) || px < area.left() - 0.5 || px > area.right() + 0.5)
                continue;
            QColor line = theme.text;
            line.setAlphaF(0.55f);
            p.setPen(QPen(line, 1, Qt::DashLine));
            p.drawLine(QPointF(px, area.top()), QPointF(px, area.bottom()));
            const QString tag = cursorName(which);
            const double w = fm.horizontalAdvance(tag) + 8;
            const double h = fm.height() + 2;
            QRectF box(px - w / 2, area.top() + 1, w, h);
            if (box.right() > area.right())
                box.moveRight(area.right());
            if (box.left() < area.left())
                box.moveLeft(area.left());
            p.setPen(Qt::NoPen);
            p.setBrush(theme.text);
            p.drawRoundedRect(box, 3, 3);
            p.setPen(theme.background);
            p.drawText(box, Qt::AlignCenter, tag);
        }
    }

private:
    WaveformPane *m_pane;
    PlotWidget *m_plot;
};

// ---------------------------------------------------------------- pane

WaveformPane::WaveformPane(const WaveformPaneDefinition &definition, QWidget *parent)
    : QFrame(parent), m_definition(definition)
{
    setObjectName(QStringLiteral("waveformPane"));
    setAutoFillBackground(false);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("paneTitle"));
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);

    m_traceCombo = new QComboBox(this);
    m_traceCombo->setObjectName(QStringLiteral("selectedTrace"));
    m_traceCombo->setToolTip(tr("Trace used by the cursors and the measurements"));
    m_traceCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_traceCombo->setMinimumContentsLength(10);

    m_plot = new PlotWidget(this);
    m_plot->setObjectName(QStringLiteral("plot"));
    m_plot->setMarkerTableVisible(false);
    m_plot->installEventFilter(this);
    m_overlay = new CursorOverlay(this, m_plot);

    m_cursorLabel = new QLabel(this);
    m_cursorLabel->setObjectName(QStringLiteral("cursorReadout"));
    m_cursorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_cursorLabel->hide();

    m_noteLabel = new QLabel(this);
    m_noteLabel->setObjectName(QStringLiteral("paneNotes"));
    m_noteLabel->setWordWrap(true);
    m_noteLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_noteLabel->hide();

    auto *header = new QHBoxLayout;
    header->setContentsMargins(4, 0, 2, 0);
    header->setSpacing(6);
    header->addWidget(m_title);
    header->addStretch();
    header->addWidget(m_traceCombo);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(5, 4, 5, 4);
    layout->setSpacing(2);
    layout->addLayout(header);
    layout->addWidget(m_plot, 1);
    layout->addWidget(m_cursorLabel);
    layout->addWidget(m_noteLabel);

    installEventFilter(this);
    m_title->installEventFilter(this);

    connect(m_traceCombo, &QComboBox::activated, this, [this](int row) {
        Q_EMIT activated();
        setSelectedTrace(m_traceCombo->itemData(row).toInt());
    });
    connect(m_plot, &PlotWidget::viewChanged, this, [this] {
        m_overlay->update();
        if (m_updatingTraces)
            return;
        const auto range = xRange();
        if (range != m_lastXRange) {
            m_lastXRange = range;
            Q_EMIT xRangeChanged(range.first, range.second);
        }
    });
    connect(m_plot, &PlotWidget::markersChanged, this, [this] {
        m_overlay->update();
        if (m_updatingTraces)
            return;
        m_cursorLabel->setText(cursorText());
        m_cursorLabel->setVisible(hasCursor(0) || hasCursor(1));
        Q_EMIT cursorsChanged();
    });
    connect(m_plot, &PlotWidget::hoverReadoutChanged, this, [this](const QString &text) {
        if (!text.isEmpty())
            Q_EMIT statusMessage(text);
    });

    rebuild(false);
}

WaveformPane::~WaveformPane() = default;

QString WaveformPane::displayName() const
{
    if (!m_definition.name.isEmpty())
        return m_definition.name;
    if (isFft())
        return tr("FFT of %1").arg(m_definition.fftSource);
    QString number = m_definition.id;
    if (number.startsWith(QLatin1Char('p')))
        number.remove(0, 1);
    return tr("Pane %1").arg(number);
}

void WaveformPane::setDefinition(const WaveformPaneDefinition &definition)
{
    const bool fftChanged = definition.isFft() != m_definition.isFft();
    m_definition = definition;
    rebuild(!fftChanged);
}

void WaveformPane::setPlot(const spice::SpicePlot *plot, bool keepView)
{
    m_data = plot;
    rebuild(keepView);
}

void WaveformPane::setNameResolver(WaveformNameResolver resolver)
{
    m_nameResolver = std::move(resolver);
    rebuild(true);
}

bool WaveformPane::isTimeDomain() const
{
    return !isFft() && m_data && m_data->type == QLatin1String("tran");
}

// ---------------------------------------------------------------- evaluation

void WaveformPane::evaluate(Entry &entry) const
{
    entry.waveform.reset();
    entry.error.clear();
    if (!m_data)
        return;
    QString error;
    const auto parsed = spice::VectorExpression::parse(entry.expression, &error);
    if (!parsed) {
        entry.error = error;
        return;
    }
    const spice::SpicePlot *shown = m_data;
    const auto resolver = [this, shown](const QString &name) -> std::optional<Waveform> {
        if (const spice::SpiceVector *v = shown->vector(name))
            return plotVectorWaveform(*shown, *v);
        if (m_nameResolver)
            return m_nameResolver(*shown, name);
        return std::nullopt;
    };
    auto w = parsed->evaluate(resolver, &error);
    if (!w) {
        entry.error = error;
        return;
    }
    if (w->x.empty() && w->y.size() == 1) {
        // A constant: a horizontal line over the scale.
        const spice::SpiceVector *scale = shown->scale();
        if (!scale || scale->size() == 0 || scale->isComplex) {
            entry.error = tr("a constant needs a plot with a scale");
            return;
        }
        const auto [lo, hi] = std::minmax_element(scale->real.begin(), scale->real.end());
        const Complex value = w->y.front();
        w->x = {*lo, *hi};
        w->y = {value, value};
    }
    if (w->y.empty()) {
        entry.error = tr("no values");
        return;
    }
    entry.waveform = std::move(*w);
}

void WaveformPane::evaluateSpectrum(Entry &entry) const
{
    entry.waveform.reset();
    entry.error.clear();
    if (!m_data)
        return;
    if (m_data->type != QLatin1String("tran")) {
        entry.error = tr("the FFT needs a transient result");
        return;
    }
    evaluate(entry);
    if (!entry.waveform)
        return;
    const Waveform source = std::move(*entry.waveform);
    entry.waveform.reset();
    if (source.isComplex) {
        entry.error = tr("the FFT needs a real trace");
        return;
    }
    spice::SpectrumOptions options;
    options.window = fftWindowFromKey(m_definition.fftWindow);
    QString error;
    const auto spectrum = spice::amplitudeSpectrum(source, options, &error);
    if (!spectrum) {
        entry.error = error;
        return;
    }
    std::vector<double> y(spectrum->value.size());
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double m = std::abs(spectrum->value[i]);
        y[i] = m_definition.fftLinear ? m : 20.0 * std::log10(std::max(m, 1e-20));
    }
    const QString unit = m_definition.fftLinear ? source.unit : QStringLiteral("dB") + source.unit;
    entry.waveform = spice::makeWaveform(spectrum->frequency, y, unit);
}

QString WaveformPane::shownName(const Entry &entry) const
{
    if (isFft())
        return tr("FFT %1").arg(entry.expression);
    if (entry.waveform && entry.waveform->isComplex && entry.part != ComplexPart::MagnitudeDb) {
        switch (entry.part) {
        case ComplexPart::Magnitude: return tr("|%1|").arg(entry.expression);
        case ComplexPart::PhaseDeg: return tr("%1 phase").arg(entry.expression);
        case ComplexPart::Real: return tr("%1 real").arg(entry.expression);
        case ComplexPart::Imaginary: return tr("%1 imag").arg(entry.expression);
        case ComplexPart::MagnitudeDb: break;
        }
    }
    return entry.expression;
}

namespace {

QString entryUnit(const Waveform &w, ComplexPart part)
{
    if (w.isComplex) {
        if (part == ComplexPart::MagnitudeDb)
            return QStringLiteral("dB");
        if (part == ComplexPart::PhaseDeg)
            return QStringLiteral("deg");
    }
    return w.unit;
}

} // namespace

void WaveformPane::rebuild(bool keepView)
{
    m_updatingTraces = true;
    const std::array<std::optional<double>, 2> savedCursors{cursorX(0), cursorX(1)};
    const auto savedRange = xRange();
    const auto savedData = dataXRange();
    const bool wasAutoscaled = m_plot->isAutoscaled();
    m_plot->clearMarkers();
    m_cursorMarkers = {-1, -1};

    // Match the new expressions with the existing plot traces (data refresh
    // keeps trace ids, colours and visibility).
    const QStringList expressions = isFft() ? QStringList{m_definition.fftSource} : m_definition.traces;
    std::vector<Entry> pool = std::move(m_entries);
    m_entries.clear();
    for (const QString &expression : expressions) {
        Entry entry;
        entry.expression = expression;
        for (Entry &old : pool) {
            if (old.plotId >= 0 && old.expression == expression) {
                entry.plotId = old.plotId;
                entry.plotUnit = old.plotUnit;
                old.plotId = -1;
                break;
            }
        }
        m_entries.push_back(std::move(entry));
    }
    for (const Entry &old : pool)
        if (old.plotId >= 0)
            m_plot->removeTrace(old.plotId);

    for (Entry &entry : m_entries) {
        entry.part = isFft() ? ComplexPart::MagnitudeDb
                             : complexPartFromKey(m_definition.complexParts.value(entry.expression));
        if (isFft())
            evaluateSpectrum(entry);
        else
            evaluate(entry);
        if (!entry.waveform) {
            if (entry.plotId >= 0)
                m_plot->removeTrace(entry.plotId);
            entry.plotId = -1;
            continue;
        }
        const Waveform &w = *entry.waveform;
        if (entry.plotId >= 0 && w.unit != entry.plotUnit) {
            m_plot->removeTrace(entry.plotId);
            entry.plotId = -1;
        }
        if (entry.plotId < 0) {
            entry.plotId = m_plot->addTrace(shownName(entry), w.unit);
            entry.plotUnit = w.unit;
        } else
            m_plot->setTraceName(entry.plotId, shownName(entry));
        if (w.isComplex) {
            m_plot->setTraceComplexPart(entry.plotId, entry.part);
            m_plot->setTraceData(entry.plotId, w.x, w.y);
        } else {
            const std::vector<double> y = w.real();
            m_plot->setTraceData(entry.plotId, w.x, y);
        }
    }

    assignAxes();
    updateAxes();

    // View: y always autoscaled; x kept when zoomed and still inside the data.
    const auto newData = dataXRange();
    m_plot->autoscale();
    if (keepView && !wasAutoscaled && newData && savedData) {
        const double tolerance = 1e-9 * std::max(std::abs(savedData->second - savedData->first), 1e-300);
        const bool wasFull = std::abs(savedRange.first - savedData->first) <= tolerance
                             && std::abs(savedRange.second - savedData->second) <= tolerance;
        const bool inside = savedRange.first >= newData->first - tolerance && savedRange.second <= newData->second + tolerance;
        if (!wasFull && inside && savedRange.second > savedRange.first)
            m_plot->setXRange(savedRange.first, savedRange.second);
    }

    m_selected = std::clamp(m_selected, 0, std::max(0, traceCount() - 1));
    if (keepView)
        restoreCursors(savedCursors);
    m_lastXRange = xRange();
    m_updatingTraces = false;

    updateHeader();
    updateNotes();
    m_cursorLabel->setText(cursorText());
    m_cursorLabel->setVisible(hasCursor(0) || hasCursor(1));
    m_overlay->update();
    Q_EMIT selectionChanged();
    Q_EMIT cursorsChanged();
}

void WaveformPane::assignAxes()
{
    m_notes.clear();
    QString left, right;
    bool leftSet = false, rightSet = false;
    const auto shown = [](const Entry &e) { return entryUnit(*e.waveform, e.part); };
    // Phase prefers the right axis; everything else fills left, then right.
    for (Entry &e : m_entries) {
        if (!e.waveform)
            continue;
        const QString unit = shown(e);
        const bool phase = unit == QLatin1String("deg");
        if (leftSet && unit == left) {
            e.axis = PlotYAxis::Left;
        } else if (rightSet && unit == right) {
            e.axis = PlotYAxis::Right;
        } else if (!leftSet && !phase) {
            left = unit;
            leftSet = true;
            e.axis = PlotYAxis::Left;
        } else if (!rightSet) {
            right = unit;
            rightSet = true;
            e.axis = PlotYAxis::Right;
        } else if (!leftSet) {
            left = unit;
            leftSet = true;
            e.axis = PlotYAxis::Left;
        } else {
            e.axis = PlotYAxis::Left;
            m_notes << tr("%1 is in %2 and is drawn on the left axis (%3): a pane shows two units at most.")
                           .arg(e.expression, unit.isEmpty() ? tr("no unit") : displayUnit(unit),
                                left.isEmpty() ? tr("no unit") : displayUnit(left));
        }
    }
    // Only right-axis traces (a phase alone): use the left axis.
    if (!leftSet && rightSet)
        for (Entry &e : m_entries)
            e.axis = PlotYAxis::Left;
    for (const Entry &e : m_entries) {
        if (e.plotId < 0)
            continue;
        TraceStyle style = m_plot->traceStyle(e.plotId);
        style.yAxis = e.axis;
        style.lineWidth = 1.6;
        m_plot->setTraceStyle(e.plotId, style);
    }
}

void WaveformPane::updateAxes()
{
    // x axis from the scale vector.
    QString xLabel = tr("Point");
    QString xUnit;
    if (isFft()) {
        xLabel = tr("Frequency");
        xUnit = QStringLiteral("Hz");
    } else if (m_data) {
        if (const spice::SpiceVector *scale = m_data->scale()) {
            xUnit = spice::vectorKindUnit(scale->kind);
            switch (scale->kind) {
            case spice::VectorKind::Time: xLabel = tr("Time"); break;
            case spice::VectorKind::Frequency: xLabel = tr("Frequency"); break;
            case spice::VectorKind::Temperature: xLabel = tr("Temperature"); break;
            case spice::VectorKind::Voltage:
            case spice::VectorKind::Current: xLabel = tr("Sweep (%1)").arg(scale->name); break;
            default: xLabel = scale->name; break;
            }
            if (xUnit.isEmpty()) {
                if (scale->name == QLatin1String("time"))
                    xUnit = QStringLiteral("s");
                else if (scale->name == QLatin1String("frequency"))
                    xUnit = QStringLiteral("Hz");
            }
        }
    }
    m_xUnit = xUnit;
    m_plot->setXAxis(xLabel, xUnit, m_definition.logX);

    QString left, right;
    bool leftSet = false, rightSet = false;
    for (const Entry &e : m_entries) {
        if (!e.waveform)
            continue;
        const QString unit = entryUnit(*e.waveform, e.part);
        if (e.axis == PlotYAxis::Left && !leftSet) {
            left = unit;
            leftSet = true;
        } else if (e.axis == PlotYAxis::Right && !rightSet) {
            right = unit;
            rightSet = true;
        }
    }
    m_plot->setYAxis(axisLabel(left), left, false, PlotYAxis::Left);
    m_plot->setYAxis(axisLabel(right), right, false, PlotYAxis::Right);
}

void WaveformPane::updateHeader()
{
    QString title = displayName();
    if (isFft()) {
        title += QStringLiteral("  ·  ") + spice::windowName(fftWindowFromKey(m_definition.fftWindow))
                 + (m_definition.fftLinear ? tr(", linear") : QString());
    }
    m_title->setText(title);
    const QSignalBlocker block(m_traceCombo);
    m_traceCombo->clear();
    for (int i = 0; i < traceCount(); ++i) {
        const Entry &e = m_entries[std::size_t(i)];
        const QColor color = e.plotId >= 0 ? m_plot->traceColor(e.plotId) : QColor(Qt::transparent);
        m_traceCombo->addItem(swatch(color), shownName(e), i);
    }
    m_traceCombo->setCurrentIndex(m_traceCombo->findData(m_selected));
    m_traceCombo->setVisible(traceCount() > 1);
}

void WaveformPane::updateNotes()
{
    QStringList lines;
    for (const Entry &e : m_entries)
        if (!e.error.isEmpty())
            lines << QStringLiteral("%1: %2").arg(e.expression, e.error);
    QStringList notes = m_notes;
    if (m_data && m_entries.empty())
        notes << tr("No traces: double-click a vector or type an expression to add one to this pane.");
    QString html;
    const QString error = errorColor().name();
    for (const QString &line : lines)
        html += QStringLiteral("<div style=\"color:%1\">%2</div>").arg(error, line.toHtmlEscaped());
    for (const QString &line : notes)
        html += QStringLiteral("<div>%1</div>").arg(line.toHtmlEscaped());
    m_noteLabel->setText(html);
    m_noteLabel->setVisible(!html.isEmpty());
    m_noteLabel->setToolTip((lines + notes).join(QLatin1Char('\n')));
}

QString WaveformPane::noteText() const
{
    QStringList lines;
    for (const Entry &e : m_entries)
        if (!e.error.isEmpty())
            lines << QStringLiteral("%1: %2").arg(e.expression, e.error);
    lines << m_notes;
    return lines.join(QLatin1Char('\n'));
}

QColor WaveformPane::errorColor() const
{
    return palette().color(QPalette::Window).lightness() < 128 ? QColor(0xF0, 0x80, 0x80) : QColor(0xB0, 0x20, 0x20);
}

// ---------------------------------------------------------------- traces

QString WaveformPane::traceExpression(int index) const
{
    return index >= 0 && index < traceCount() ? m_entries[std::size_t(index)].expression : QString();
}

QString WaveformPane::traceError(int index) const
{
    return index >= 0 && index < traceCount() ? m_entries[std::size_t(index)].error : QString();
}

const Waveform *WaveformPane::traceWaveform(int index) const
{
    if (index < 0 || index >= traceCount())
        return nullptr;
    const auto &w = m_entries[std::size_t(index)].waveform;
    return w ? &*w : nullptr;
}

int WaveformPane::plotTraceId(int index) const
{
    return index >= 0 && index < traceCount() ? m_entries[std::size_t(index)].plotId : -1;
}

PlotYAxis WaveformPane::traceAxis(int index) const
{
    return index >= 0 && index < traceCount() ? m_entries[std::size_t(index)].axis : PlotYAxis::Left;
}

ComplexPart WaveformPane::traceComplexPart(int index) const
{
    return index >= 0 && index < traceCount() ? m_entries[std::size_t(index)].part : ComplexPart::MagnitudeDb;
}

std::optional<Waveform> WaveformPane::shownWaveform(int index) const
{
    const Waveform *w = traceWaveform(index);
    if (!w)
        return std::nullopt;
    if (!w->isComplex)
        return *w;
    const ComplexPart part = traceComplexPart(index);
    Waveform shown;
    shown.x = w->x;
    shown.y.resize(w->y.size());
    for (std::size_t i = 0; i < w->y.size(); ++i)
        shown.y[i] = Complex(shownValue(w->y[i], part), 0.0);
    shown.unit = entryUnit(*w, part);
    return shown;
}

void WaveformPane::setTraceComplexPart(int index, ComplexPart part)
{
    if (index < 0 || index >= traceCount() || isFft())
        return;
    const QString expression = m_entries[std::size_t(index)].expression;
    if (part == ComplexPart::MagnitudeDb)
        m_definition.complexParts.remove(expression);
    else
        m_definition.complexParts.insert(expression, complexPartKey(part));
    rebuild(true);
    emitEdited();
}

void WaveformPane::removeTraceAt(int index)
{
    if (index < 0 || index >= traceCount() || isFft())
        return;
    const QString expression = m_definition.traces.takeAt(index);
    if (!m_definition.traces.contains(expression))
        m_definition.complexParts.remove(expression);
    if (m_selected > index)
        --m_selected;
    rebuild(true);
    emitEdited();
}

void WaveformPane::setLogX(bool log)
{
    if (m_definition.logX == log)
        return;
    m_definition.logX = log;
    rebuild(false);
    emitEdited();
}

void WaveformPane::setFftWindow(spice::WindowType window)
{
    const QString key = fftWindowKey(window);
    if (m_definition.fftWindow == key)
        return;
    m_definition.fftWindow = key;
    rebuild(true);
    emitEdited();
}

void WaveformPane::setFftLinear(bool linear)
{
    if (m_definition.fftLinear == linear)
        return;
    m_definition.fftLinear = linear;
    rebuild(true);
    emitEdited();
}

void WaveformPane::rename(const QString &name)
{
    if (m_definition.name == name)
        return;
    m_definition.name = name;
    updateHeader();
    emitEdited();
}

void WaveformPane::emitEdited()
{
    Q_EMIT definitionEdited();
}

void WaveformPane::setSelectedTrace(int index)
{
    if (index < 0 || index >= traceCount() || index == m_selected)
        return;
    const std::array<std::optional<double>, 2> xs{cursorX(0), cursorX(1)};
    m_selected = index;
    m_updatingTraces = true;
    m_plot->clearMarkers();
    m_cursorMarkers = {-1, -1};
    restoreCursors(xs);
    m_updatingTraces = false;
    {
        const QSignalBlocker block(m_traceCombo);
        m_traceCombo->setCurrentIndex(m_traceCombo->findData(m_selected));
    }
    m_cursorLabel->setText(cursorText());
    m_overlay->update();
    Q_EMIT selectionChanged();
    Q_EMIT cursorsChanged();
}

void WaveformPane::setCurrent(bool current)
{
    if (m_current == current)
        return;
    m_current = current;
    update();
}

// ---------------------------------------------------------------- cursors

int WaveformPane::markerTrace() const
{
    const int id = plotTraceId(m_selected);
    if (id >= 0)
        return id;
    for (const Entry &e : m_entries)
        if (e.plotId >= 0)
            return e.plotId;
    return -1;
}

void WaveformPane::restoreCursors(const std::array<std::optional<double>, 2> &xs)
{
    const int trace = markerTrace();
    if (trace < 0)
        return;
    const auto range = dataXRange();
    for (int which = 0; which < 2; ++which) {
        if (!xs[std::size_t(which)])
            continue;
        double x = *xs[std::size_t(which)];
        if (range)
            x = std::clamp(x, range->first, range->second);
        m_cursorMarkers[std::size_t(which)] = m_plot->addMarker(trace, x);
    }
}

std::optional<double> WaveformPane::cursorX(int which) const
{
    if (which < 0 || which > 1 || !m_plot)
        return std::nullopt;
    const int id = m_cursorMarkers[std::size_t(which)];
    if (id < 0)
        return std::nullopt;
    if (const auto info = m_plot->marker(id))
        return info->x;
    return std::nullopt;
}

void WaveformPane::setCursorX(int which, std::optional<double> x)
{
    if (which < 0 || which > 1)
        return;
    m_updatingTraces = true;
    const std::size_t w = std::size_t(which);
    if (!x) {
        if (m_cursorMarkers[w] >= 0)
            m_plot->removeMarker(m_cursorMarkers[w]);
        m_cursorMarkers[w] = -1;
    } else if (m_cursorMarkers[w] >= 0 && m_plot->marker(m_cursorMarkers[w])) {
        m_plot->setMarkerX(m_cursorMarkers[w], *x);
    } else {
        // Keep A before B so that the plot numbers them M1 and M2.
        std::array<std::optional<double>, 2> xs{cursorX(0), cursorX(1)};
        xs[w] = *x;
        m_plot->clearMarkers();
        m_cursorMarkers = {-1, -1};
        const int trace = markerTrace();
        if (trace >= 0)
            for (int i = 0; i < 2; ++i)
                if (xs[std::size_t(i)])
                    m_cursorMarkers[std::size_t(i)] = m_plot->addMarker(trace, *xs[std::size_t(i)]);
    }
    m_updatingTraces = false;
    m_cursorLabel->setText(cursorText());
    m_cursorLabel->setVisible(hasCursor(0) || hasCursor(1));
    m_overlay->update();
    Q_EMIT cursorsChanged();
}

void WaveformPane::showCursor(int which, bool on)
{
    if (!on) {
        setCursorX(which, std::nullopt);
        return;
    }
    if (hasCursor(which) || markerTrace() < 0)
        return;
    const auto [lo, hi] = xRange();
    const double t = which == 0 ? 1.0 / 3.0 : 2.0 / 3.0;
    double x = lo + (hi - lo) * t;
    if (m_plot->xScale().isLog() && lo > 0 && hi > 0)
        x = lo * std::pow(hi / lo, t);
    if (const auto range = dataXRange())
        x = std::clamp(x, range->first, range->second);
    setCursorX(which, x);
}

WaveformCursorReadout WaveformPane::cursorReadout() const
{
    WaveformCursorReadout r;
    for (int which = 0; which < 2; ++which) {
        const int id = m_cursorMarkers[std::size_t(which)];
        if (id < 0)
            continue;
        const auto info = m_plot->marker(id);
        if (!info)
            continue;
        r.xUnit = info->xUnit;
        r.yUnit = info->yUnit;
        if (which == 0) {
            r.hasA = true;
            r.xA = info->x;
            r.yA = info->y;
            r.validA = info->valid;
        } else {
            r.hasB = true;
            r.xB = info->x;
            r.yB = info->y;
            r.validB = info->valid;
        }
    }
    if (r.hasA && r.hasB) {
        r.dx = r.xB - r.xA;
        r.dy = r.yB - r.yA;
        if (std::abs(r.dy) <= 1e-12 * std::max(std::abs(r.yA), std::abs(r.yB)))
            r.dy = 0; // rounding noise
        r.inverseDx = r.dx != 0 ? 1.0 / std::abs(r.dx) : INFINITY;
    }
    return r;
}

QString WaveformPane::cursorText() const
{
    const WaveformCursorReadout r = cursorReadout();
    QStringList parts;
    const auto one = [&](const QString &name, bool has, bool valid, double x, double y) {
        if (!has)
            return;
        parts << QStringLiteral("%1: %2, %3").arg(name, formatValue(x, r.xUnit, 5),
                                                  valid ? formatValue(y, r.yUnit, 4) : tr("out of range"));
    };
    one(QStringLiteral("A"), r.hasA, r.validA, r.xA, r.yA);
    one(QStringLiteral("B"), r.hasB, r.validB, r.xB, r.yB);
    if (r.hasDelta()) {
        const QString delta(QChar(0x0394));
        parts << QStringLiteral("%1X = %2").arg(delta, formatValue(r.dx, r.xUnit, 4));
        parts << QStringLiteral("%1Y = %2").arg(delta, formatValue(r.dy, r.yUnit, 4));
        // 1/dX is a frequency on a time axis; elsewhere it means nothing.
        if (std::isfinite(r.inverseDx) && r.xUnit == QLatin1String("s"))
            parts << QStringLiteral("1/%1X = %2").arg(delta, formatValue(r.inverseDx, QStringLiteral("Hz"), 4));
    }
    return parts.join(QStringLiteral("     "));
}

// ---------------------------------------------------------------- view

std::pair<double, double> WaveformPane::xRange() const
{
    const AxisScale s = m_plot->xScale();
    return {s.min(), s.max()};
}

void WaveformPane::setXRange(double min, double max)
{
    m_plot->setXRange(min, max);
    m_lastXRange = xRange();
}

void WaveformPane::autoscale()
{
    m_plot->autoscale();
}

std::optional<std::pair<double, double>> WaveformPane::dataXRange() const
{
    double lo = INFINITY, hi = -INFINITY;
    for (const Entry &e : m_entries) {
        if (!e.waveform || e.waveform->x.empty())
            continue;
        for (double x : {e.waveform->x.front(), e.waveform->x.back()}) {
            if (std::isfinite(x)) {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        }
    }
    if (!(hi >= lo))
        return std::nullopt;
    return std::pair(lo, hi);
}

// ---------------------------------------------------------------- events

bool WaveformPane::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        Q_EMIT activated();
        break;
    case QEvent::ContextMenu:
        if (watched == m_plot) {
            auto *e = static_cast<QContextMenuEvent *>(event);
            Q_EMIT activated();
            showContextMenu(e->globalPos(), QPointF(e->pos()));
            return true;
        }
        break;
    case QEvent::Resize:
        if (watched == m_plot)
            m_overlay->setGeometry(m_plot->rect());
        break;
    default:
        break;
    }
    return QFrame::eventFilter(watched, event);
}

void WaveformPane::paintEvent(QPaintEvent *event)
{
    QFrame::paintEvent(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    QColor color = m_current ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid);
    if (!m_current)
        color.setAlphaF(0.6f);
    const double width = m_current ? 2.0 : 1.0;
    p.setPen(QPen(color, width));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(width / 2 + 0.5, width / 2 + 0.5, -width / 2 - 0.5, -width / 2 - 0.5), 4,
                      4);
}

void WaveformPane::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        updateNotes();
        updateHeader();
    }
    QFrame::changeEvent(event);
}

void WaveformPane::addTraceMenu(QMenu *menu, int index)
{
    const Entry &e = m_entries[std::size_t(index)];
    QAction *select = menu->addAction(tr("Use for Cursors and Measurements"));
    select->setCheckable(true);
    select->setChecked(index == m_selected);
    connect(select, &QAction::triggered, this, [this, index] { setSelectedTrace(index); });
    if (e.waveform && e.waveform->isComplex && !isFft()) {
        QMenu *show = menu->addMenu(tr("Show As"));
        auto *group = new QActionGroup(show);
        for (ComplexPart part : {ComplexPart::MagnitudeDb, ComplexPart::Magnitude, ComplexPart::PhaseDeg,
                                 ComplexPart::Real, ComplexPart::Imaginary}) {
            QAction *a = show->addAction(partLabel(part));
            a->setCheckable(true);
            a->setChecked(e.part == part);
            group->addAction(a);
            connect(a, &QAction::triggered, this, [this, index, part] { setTraceComplexPart(index, part); });
        }
    }
    if (e.plotId >= 0) {
        const int id = e.plotId;
        QAction *visible = menu->addAction(tr("Visible"));
        visible->setCheckable(true);
        visible->setChecked(m_plot->isTraceVisible(id));
        connect(visible, &QAction::toggled, this, [this, id](bool on) { m_plot->setTraceVisible(id, on); });
    }
    if (!isFft() && isTimeDomain() && e.waveform && !e.waveform->isComplex) {
        const QString expression = e.expression;
        connect(menu->addAction(tr("FFT of This Trace")), &QAction::triggered, this,
                [this, expression] { Q_EMIT fftRequested(expression); });
    }
    const QString expression = e.expression;
    connect(menu->addAction(tr("Copy Expression")), &QAction::triggered, this, [expression] {
        if (QClipboard *clipboard = QApplication::clipboard())
            clipboard->setText(expression);
    });
    if (!isFft()) {
        connect(menu->addAction(tr("Remove Trace")), &QAction::triggered, this,
                [this, index] { removeTraceAt(index); });
    }
}

void WaveformPane::showContextMenu(const QPoint &globalPos, const QPointF &plotPos)
{
    QMenu menu(this);
    // Over a legend entry: that trace's menu.
    for (int i = 0; i < traceCount(); ++i) {
        const int id = m_entries[std::size_t(i)].plotId;
        if (id >= 0 && m_plot->legendItemRect(id).contains(plotPos)) {
            menu.addSection(shownName(m_entries[std::size_t(i)]));
            addTraceMenu(&menu, i);
            menu.exec(globalPos);
            return;
        }
    }
    if (traceCount() > 0) {
        QMenu *traces = menu.addMenu(isFft() ? tr("Spectrum") : tr("Traces"));
        for (int i = 0; i < traceCount(); ++i) {
            const Entry &e = m_entries[std::size_t(i)];
            QMenu *sub = traces->addMenu(shownName(e));
            if (e.plotId >= 0)
                sub->setIcon(swatch(m_plot->traceColor(e.plotId)));
            addTraceMenu(sub, i);
        }
    }
    if (markerTrace() >= 0) {
        const double x = m_plot->pixelToData(plotPos).x();
        const bool inside = m_plot->plotArea().contains(plotPos) && std::isfinite(x);
        for (int which = 0; which < 2; ++which) {
            QAction *a = menu.addAction(tr("Cursor %1 Here").arg(cursorName(which)));
            a->setEnabled(inside);
            connect(a, &QAction::triggered, this, [this, which, x] { setCursorX(which, x); });
        }
        if (hasCursor(0) || hasCursor(1)) {
            connect(menu.addAction(tr("Remove Cursors")), &QAction::triggered, this, [this] {
                setCursorX(1, std::nullopt);
                setCursorX(0, std::nullopt);
            });
        }
    }
    menu.addSeparator();
    QAction *log = menu.addAction(tr("Logarithmic X Axis"));
    log->setCheckable(true);
    log->setChecked(m_definition.logX);
    connect(log, &QAction::toggled, this, &WaveformPane::setLogX);
    if (isFft()) {
        QMenu *windows = menu.addMenu(tr("Window"));
        auto *group = new QActionGroup(windows);
        const spice::WindowType current = fftWindowFromKey(m_definition.fftWindow);
        for (spice::WindowType type : spice::allWindows()) {
            QAction *a = windows->addAction(spice::windowName(type));
            a->setCheckable(true);
            a->setChecked(type == current);
            group->addAction(a);
            connect(a, &QAction::triggered, this, [this, type] { setFftWindow(type); });
        }
        QMenu *scale = menu.addMenu(tr("Magnitude"));
        auto *scaleGroup = new QActionGroup(scale);
        QAction *db = scale->addAction(tr("Decibels (dBV)"));
        QAction *linear = scale->addAction(tr("Linear (V)"));
        for (QAction *a : {db, linear}) {
            a->setCheckable(true);
            scaleGroup->addAction(a);
        }
        (m_definition.fftLinear ? linear : db)->setChecked(true);
        connect(db, &QAction::triggered, this, [this] { setFftLinear(false); });
        connect(linear, &QAction::triggered, this, [this] { setFftLinear(true); });
    }
    menu.addSeparator();
    const QList<QAction *> actions = m_plot->plotActions(); // autoscale, grid, admittance, png, svg, csv, copy
    menu.addAction(m_plot->autoscaleAction());
    if (actions.size() >= 7) {
        menu.addAction(actions[1]);
        menu.addSeparator();
        menu.addAction(actions[6]);
        menu.addAction(actions[3]);
        menu.addAction(actions[4]);
        menu.addAction(actions[5]);
    }
    menu.addSeparator();
    connect(menu.addAction(tr("Rename Pane...")), &QAction::triggered, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename Pane"), tr("Name:"), QLineEdit::Normal,
                                                   displayName(), &ok);
        if (ok)
            rename(name.trimmed());
    });
    connect(menu.addAction(tr("Add Pane")), &QAction::triggered, this, [this] { Q_EMIT addPaneRequested(); });
    connect(menu.addAction(tr("Remove Pane")), &QAction::triggered, this, [this] { Q_EMIT removeRequested(); });
    menu.exec(globalPos);
}

} // namespace lvd::spiceui

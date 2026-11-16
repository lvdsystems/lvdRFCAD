#include "plot/PlotWidget.h"

#include "plot/PlotMath.h"
#include "plot/PlotWidgetPrivate.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>
#include <QTextStream>
#include <QWheelEvent>

#ifdef LVD_PLOT_HAVE_SVG
#include <QSvgGenerator>
#endif

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lvd {

namespace {

constexpr double kRadToDeg = 180.0 / std::numbers::pi;

double shownPart(std::complex<double> z, ComplexPart part)
{
    switch (part) {
    case ComplexPart::MagnitudeDb: return magnitudeDb(z);
    case ComplexPart::Magnitude: return std::abs(z);
    case ComplexPart::PhaseDeg: return std::arg(z) * kRadToDeg;
    case ComplexPart::Real: return z.real();
    case ComplexPart::Imaginary: return z.imag();
    }
    return z.real();
}

// Index i such that x lies within [x[i], x[i+1]] and the fraction t along it.
std::optional<std::pair<std::size_t, double>> locate(const std::vector<double> &xs, bool ascending, double x)
{
    const std::size_t n = xs.size();
    if (n == 0 || !std::isfinite(x))
        return std::nullopt;
    if (n == 1)
        return xs[0] == x ? std::optional(std::pair<std::size_t, double>(0, 0.0)) : std::nullopt;
    const auto fraction = [&](std::size_t i) {
        const double span = xs[i + 1] - xs[i];
        return span != 0 ? (x - xs[i]) / span : 0.0;
    };
    if (ascending) {
        if (x < xs.front() || x > xs.back())
            return std::nullopt;
        auto it = std::upper_bound(xs.begin(), xs.end(), x);
        std::size_t i = it == xs.end() ? n - 2 : static_cast<std::size_t>(it - xs.begin()) - 1;
        i = std::min(i, n - 2);
        return std::pair(i, fraction(i));
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const double a = std::min(xs[i], xs[i + 1]);
        const double b = std::max(xs[i], xs[i + 1]);
        if (x >= a && x <= b)
            return std::pair(i, fraction(i));
    }
    return std::nullopt;
}

QString trimmedNumber(double v, int decimals)
{
    QString text = QString::number(v, 'f', decimals);
    if (text.contains(QLatin1Char('.'))) {
        while (text.endsWith(QLatin1Char('0')))
            text.chop(1);
        if (text.endsWith(QLatin1Char('.')))
            text.chop(1);
    }
    return text == QLatin1String("-0") ? QStringLiteral("0") : text;
}

// "48.2 - j5.1 ohm": both parts rounded to four digits of the larger one.
QString formatImpedance(std::complex<double> z)
{
    if (!std::isfinite(z.real()) || !std::isfinite(z.imag()))
        return QString(QChar(0x221E));
    const double m = std::max(std::abs(z.real()), std::abs(z.imag()));
    const int decimals = m > 0 ? std::clamp(3 - static_cast<int>(std::floor(std::log10(m))), 0, 6) : 0;
    const QString imaginary = trimmedNumber(std::abs(z.imag()), decimals);
    const bool negative = z.imag() < 0 && imaginary != QLatin1String("0");
    return QStringLiteral("%1 %2 j%3 %4")
        .arg(trimmedNumber(z.real(), decimals), negative ? QString(QChar(0x2212)) : QStringLiteral("+"), imaginary,
             QString(QChar(0x03A9)));
}

// "0.808 angle -82.8 deg"
QString formatGamma(std::complex<double> g)
{
    const double magnitude = std::abs(g);
    const double angle = magnitude < 5e-4 ? 0.0 : std::arg(g) * kRadToDeg;
    return QString::number(magnitude, 'f', 3) + QChar(0x2220) + trimmedNumber(angle, 1) + QChar(0x00B0);
}

QString csvField(const QString &text)
{
    if (text.contains(QLatin1Char(',')) || text.contains(QLatin1Char('"')) || text.contains(QLatin1Char('\n'))) {
        QString quoted = text;
        quoted.replace(QLatin1String("\""), QLatin1String("\"\""));
        return QLatin1Char('"') + quoted + QLatin1Char('"');
    }
    return text;
}

QString csvNumber(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 12) : QString();
}

} // namespace

PlotWidget::PlotWidget(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_x.label = tr("Frequency");
    m_x.unit = QStringLiteral("Hz");
    m_yLeft.label = tr("Magnitude");
    m_yLeft.unit = QStringLiteral("dB");
    updateTheme();
    createActions();
    refreshAutoRanges();
}

PlotWidget::~PlotWidget() = default;

void PlotWidget::createActions()
{
    m_autoscaleAction = new QAction(tr("&Autoscale"), this);
    m_autoscaleAction->setToolTip(tr("Reset the view to show all data (double-click)"));
    connect(m_autoscaleAction, &QAction::triggered, this, &PlotWidget::autoscale);

    m_gridAction = new QAction(tr("Show &Grid"), this);
    m_gridAction->setCheckable(true);
    m_gridAction->setChecked(m_gridVisible);
    connect(m_gridAction, &QAction::toggled, this, &PlotWidget::setGridVisible);

    m_admittanceAction = new QAction(tr("Show Ad&mittance Grid"), this);
    m_admittanceAction->setCheckable(true);
    connect(m_admittanceAction, &QAction::toggled, this, &PlotWidget::setAdmittanceGridVisible);

    m_exportPngAction = new QAction(tr("Export &PNG..."), this);
    connect(m_exportPngAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Export PNG"), {}, tr("PNG images (*.png)"));
        if (!path.isEmpty())
            exportImage(path, size());
    });
    m_exportSvgAction = new QAction(tr("Export &SVG..."), this);
    m_exportSvgAction->setEnabled(svgSupported());
    connect(m_exportSvgAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Export SVG"), {}, tr("SVG drawings (*.svg)"));
        if (!path.isEmpty())
            exportSvg(path);
    });
    m_exportCsvAction = new QAction(tr("Export &CSV..."), this);
    connect(m_exportCsvAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Export CSV"), {}, tr("CSV files (*.csv)"));
        if (!path.isEmpty())
            exportCsv(path);
    });
    m_copyAction = new QAction(tr("C&opy Image"), this);
    connect(m_copyAction, &QAction::triggered, this, &PlotWidget::copyImageToClipboard);
}

QList<QAction *> PlotWidget::plotActions() const
{
    return {m_autoscaleAction, m_gridAction, m_admittanceAction, m_exportPngAction,
            m_exportSvgAction, m_exportCsvAction, m_copyAction};
}

QSize PlotWidget::sizeHint() const
{
    return {640, 420};
}

QSize PlotWidget::minimumSizeHint() const
{
    return {200, 150};
}

void PlotWidget::invalidate()
{
    m_layout.reset();
    update();
}

void PlotWidget::updateTheme()
{
    m_theme = PlotTheme::fromPalette(palette());
}

// ---------------------------------------------------------------- general

void PlotWidget::setKind(PlotKind kind)
{
    if (m_kind == kind)
        return;
    m_kind = kind;
    m_viewCentre = 0.0;
    m_viewHalfSize = 0;
    m_drag = Drag::None;
    m_admittanceAction->setEnabled(kind == PlotKind::Smith);
    refreshAutoRanges();
    invalidate();
    Q_EMIT viewChanged();
}

void PlotWidget::setTitle(const QString &title)
{
    m_title = title;
    invalidate();
}

void PlotWidget::setGridVisible(bool visible)
{
    m_gridVisible = visible;
    if (m_gridAction->isChecked() != visible)
        m_gridAction->setChecked(visible);
    update();
}

void PlotWidget::setLegendVisible(bool visible)
{
    m_legendVisible = visible;
    invalidate();
}

void PlotWidget::setMarkerTableVisible(bool visible)
{
    m_markerTableVisible = visible;
    invalidate();
}

void PlotWidget::setCrosshairEnabled(bool enabled)
{
    m_crosshairEnabled = enabled;
    update();
}

// ---------------------------------------------------------------- axes

void PlotWidget::setXAxis(const QString &label, const QString &unit, bool log)
{
    m_x.label = label;
    m_x.unit = unit;
    if (m_x.scale.isLog() != log) {
        m_x.scale.setLog(log);
        m_x.autoRange = true;
    }
    refreshAutoRanges();
    invalidate();
}

void PlotWidget::setYAxis(const QString &label, const QString &unit, bool log, PlotYAxis side)
{
    Axis &axis = side == PlotYAxis::Left ? m_yLeft : m_yRight;
    axis.label = label;
    axis.unit = unit;
    if (axis.scale.isLog() != log) {
        axis.scale.setLog(log);
        axis.autoRange = true;
    }
    refreshAutoRanges();
    invalidate();
}

void PlotWidget::setXRange(double min, double max)
{
    if (!(max > min))
        return;
    m_x.scale.setRange(min, max);
    m_x.autoRange = false;
    invalidate();
    Q_EMIT viewChanged();
}

void PlotWidget::setYRange(double min, double max, PlotYAxis side)
{
    if (!(max > min))
        return;
    Axis &axis = side == PlotYAxis::Left ? m_yLeft : m_yRight;
    axis.scale.setRange(min, max);
    axis.autoRange = false;
    invalidate();
    Q_EMIT viewChanged();
}

AxisScale PlotWidget::yScale(PlotYAxis side) const
{
    return side == PlotYAxis::Left ? m_yLeft.scale : m_yRight.scale;
}

bool PlotWidget::hasRightAxis() const
{
    return std::any_of(m_traces.begin(), m_traces.end(),
                       [](const auto &t) { return t->style.yAxis == PlotYAxis::Right; });
}

void PlotWidget::refreshAutoRanges()
{
    const auto unite = [this](bool isX, PlotYAxis side, bool log, double &lo, double &hi) {
        lo = INFINITY;
        hi = -INFINITY;
        for (const auto &t : m_traces) {
            if (!t->visible || t->size() == 0)
                continue;
            if (!isX && t->style.yAxis != side)
                continue;
            const double tMin = isX ? (log ? t->xMinPositive : t->xMin) : (log ? t->yMinPositive : t->yMin);
            const double tMax = isX ? t->xMax : t->yMax;
            if (std::isfinite(tMin))
                lo = std::min(lo, tMin);
            if (std::isfinite(tMax))
                hi = std::max(hi, tMax);
        }
        return std::isfinite(lo) && std::isfinite(hi) && hi >= lo;
    };
    const auto apply = [&](Axis &axis, bool isX, PlotYAxis side) {
        if (!axis.autoRange)
            return;
        const bool log = axis.scale.isLog();
        double lo = 0, hi = 0;
        if (!unite(isX, side, log, lo, hi)) {
            if (log)
                axis.scale.setRange(1, 10);
            else
                axis.scale.setRange(0, 1);
            return;
        }
        // Decibel axes: near-perfect values (an ideal match at -200 dB) would squash
        // everything else, so autoscale looks at most 100 dB below the largest value.
        if (!isX && !log && axis.unit.startsWith(QLatin1String("dB")))
            lo = std::max(lo, hi - 100.0);
        if (log) {
            if (hi <= lo) {
                lo /= 2;
                hi *= 2;
            }
            if (!isX) {
                const double margin = (std::log10(hi) - std::log10(lo)) * 0.05;
                lo /= std::pow(10.0, margin);
                hi *= std::pow(10.0, margin);
            }
        } else {
            if (hi <= lo) {
                const double d = lo == 0 ? 1.0 : std::abs(lo) * 0.1;
                lo -= d;
                hi += d;
            } else if (!isX) {
                const double margin = (hi - lo) * 0.05;
                lo -= margin;
                hi += margin;
            }
        }
        axis.scale.setRange(lo, hi);
    };
    apply(m_x, true, PlotYAxis::Left);
    apply(m_yLeft, false, PlotYAxis::Left);
    apply(m_yRight, false, PlotYAxis::Right);

    double polarMax = -INFINITY;
    for (const auto &t : m_traces) {
        if (t->visible && std::isfinite(t->polarMax))
            polarMax = std::max(polarMax, t->polarMax);
    }
    if (m_polarScale == PolarScale::Decibel) {
        m_polarAutoMax = std::isfinite(polarMax) ? std::ceil(polarMax / 5.0 - 1e-9) * 5.0 : 0.0;
        if (m_polarAutoMax <= m_polarFloorDb)
            m_polarAutoMax = m_polarFloorDb + 10;
    } else if (std::isfinite(polarMax) && polarMax > 0) {
        const AxisTicks ticks = linearTicks(0, polarMax, 5);
        m_polarAutoMax = std::ceil(polarMax / ticks.step - 1e-9) * ticks.step;
    } else {
        m_polarAutoMax = 1;
    }
}

void PlotWidget::autoscale()
{
    m_x.autoRange = m_yLeft.autoRange = m_yRight.autoRange = true;
    m_viewCentre = 0.0;
    m_viewHalfSize = 0;
    m_polarMax = NAN;
    refreshAutoRanges();
    invalidate();
    Q_EMIT viewChanged();
}

bool PlotWidget::isAutoscaled() const
{
    return m_x.autoRange && m_yLeft.autoRange && m_yRight.autoRange && m_viewHalfSize == 0;
}

// ---------------------------------------------------------------- traces

PlotWidget::Trace *PlotWidget::findTrace(int traceId)
{
    for (auto &t : m_traces)
        if (t->id == traceId)
            return t.get();
    return nullptr;
}

const PlotWidget::Trace *PlotWidget::findTrace(int traceId) const
{
    for (const auto &t : m_traces)
        if (t->id == traceId)
            return t.get();
    return nullptr;
}

int PlotWidget::nextColorSlot() const
{
    // The first palette slot not used by an automatic colour; colour follows
    // the trace, so removing a trace never repaints the others.
    for (int slot = 0;; ++slot) {
        const bool used = std::any_of(m_traces.begin(), m_traces.end(), [slot](const auto &t) {
            return !t->style.color.isValid() && t->colorSlot == slot;
        });
        if (!used)
            return slot;
    }
}

int PlotWidget::addTrace(const QString &name, const QString &unit, const TraceStyle &style)
{
    auto trace = std::make_unique<Trace>();
    trace->id = m_nextTraceId++;
    trace->name = name;
    trace->unit = unit;
    trace->style = style;
    trace->colorSlot = nextColorSlot();
    // Beyond the eight palette hues, vary the dash instead of inventing colours.
    if (!style.color.isValid() && trace->colorSlot >= PlotTheme::kSeriesCount && style.lineStyle == Qt::SolidLine)
        trace->style.lineStyle = trace->colorSlot / PlotTheme::kSeriesCount == 1 ? Qt::DashLine : Qt::DotLine;
    const int id = trace->id;
    m_traces.push_back(std::move(trace));
    invalidate();
    return id;
}

void PlotWidget::traceDataChanged(Trace &t)
{
    const std::size_t n = t.size();
    if (t.isComplex) {
        t.y.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            t.y[i] = shownPart(t.z[i], t.part);
    }
    t.xAscending = true;
    t.xMin = t.xMax = t.xMinPositive = NAN;
    t.yMin = t.yMax = t.yMinPositive = NAN;
    double xMin = INFINITY, xMax = -INFINITY, xPos = INFINITY;
    double yMin = INFINITY, yMax = -INFINITY, yPos = INFINITY;
    double polarMax = -INFINITY;
    double previous = -INFINITY;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = t.x[i];
        const double y = t.y[i];
        if (std::isfinite(x)) {
            if (x < previous)
                t.xAscending = false;
            previous = x;
            xMin = std::min(xMin, x);
            xMax = std::max(xMax, x);
            if (x > 0)
                xPos = std::min(xPos, x);
        } else {
            t.xAscending = false;
        }
        if (std::isfinite(y)) {
            yMin = std::min(yMin, y);
            yMax = std::max(yMax, y);
            if (y > 0)
                yPos = std::min(yPos, y);
        }
        const double pv = polarValue(t, i);
        if (std::isfinite(pv))
            polarMax = std::max(polarMax, pv);
    }
    if (std::isfinite(xMin)) {
        t.xMin = xMin;
        t.xMax = xMax;
    }
    if (std::isfinite(xPos))
        t.xMinPositive = xPos;
    if (std::isfinite(yMin)) {
        t.yMin = yMin;
        t.yMax = yMax;
    }
    if (std::isfinite(yPos))
        t.yMinPositive = yPos;
    t.polarMax = std::isfinite(polarMax) ? polarMax : NAN;
    refreshAutoRanges();
    invalidate();
    if (std::any_of(m_markers.begin(), m_markers.end(), [&](const Marker &m) { return m.traceId == t.id; }))
        Q_EMIT markersChanged();
}

void PlotWidget::setTraceData(int traceId, std::span<const double> x, std::span<const double> y)
{
    Trace *t = findTrace(traceId);
    if (!t)
        return;
    const std::size_t n = std::min(x.size(), y.size());
    t->isComplex = false;
    t->x.assign(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n));
    t->y.assign(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(n));
    t->z.clear();
    traceDataChanged(*t);
}

void PlotWidget::setTraceData(int traceId, std::span<const double> x, std::span<const std::complex<double>> values)
{
    Trace *t = findTrace(traceId);
    if (!t)
        return;
    const std::size_t n = std::min(x.size(), values.size());
    t->isComplex = true;
    t->x.assign(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n));
    t->z.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(n));
    traceDataChanged(*t);
}

void PlotWidget::setTraceComplexPart(int traceId, ComplexPart part)
{
    Trace *t = findTrace(traceId);
    if (!t || t->part == part)
        return;
    t->part = part;
    traceDataChanged(*t);
}

void PlotWidget::setTraceName(int traceId, const QString &name)
{
    if (Trace *t = findTrace(traceId)) {
        t->name = name;
        invalidate();
    }
}

void PlotWidget::setTraceStyle(int traceId, const TraceStyle &style)
{
    Trace *t = findTrace(traceId);
    if (!t)
        return;
    t->style = style;
    refreshAutoRanges();
    invalidate();
}

TraceStyle PlotWidget::traceStyle(int traceId) const
{
    const Trace *t = findTrace(traceId);
    return t ? t->style : TraceStyle{};
}

QColor PlotWidget::traceColor(int traceId) const
{
    const Trace *t = findTrace(traceId);
    if (!t)
        return {};
    return t->style.color.isValid() ? t->style.color : m_theme.seriesColor(t->colorSlot);
}

void PlotWidget::setTraceVisible(int traceId, bool visible)
{
    Trace *t = findTrace(traceId);
    if (!t || t->visible == visible)
        return;
    t->visible = visible;
    refreshAutoRanges();
    invalidate();
    Q_EMIT traceVisibilityChanged(traceId, visible);
}

bool PlotWidget::isTraceVisible(int traceId) const
{
    const Trace *t = findTrace(traceId);
    return t && t->visible;
}

QString PlotWidget::traceName(int traceId) const
{
    const Trace *t = findTrace(traceId);
    return t ? t->name : QString();
}

std::vector<int> PlotWidget::traceIds() const
{
    std::vector<int> ids;
    for (const auto &t : m_traces)
        ids.push_back(t->id);
    return ids;
}

void PlotWidget::removeTrace(int traceId)
{
    const auto it = std::find_if(m_traces.begin(), m_traces.end(), [&](const auto &t) { return t->id == traceId; });
    if (it == m_traces.end())
        return;
    m_traces.erase(it);
    const auto before = m_markers.size();
    std::erase_if(m_markers, [&](const Marker &m) { return m.traceId == traceId; });
    if (!std::any_of(m_markers.begin(), m_markers.end(), [&](const Marker &m) { return m.id == m_referenceMarker; }))
        m_referenceMarker = -1;
    refreshAutoRanges();
    invalidate();
    if (before != m_markers.size())
        Q_EMIT markersChanged();
}

void PlotWidget::clear()
{
    const bool hadMarkers = !m_markers.empty();
    m_traces.clear();
    m_markers.clear();
    m_circles.clear();
    m_referenceMarker = -1;
    m_nextMarkerNumber = 1;
    setHover({});
    refreshAutoRanges();
    invalidate();
    if (hadMarkers)
        Q_EMIT markersChanged();
}

QString PlotWidget::shownUnit(const Trace &trace)
{
    if (trace.isComplex) {
        if (trace.part == ComplexPart::MagnitudeDb)
            return QStringLiteral("dB");
        if (trace.part == ComplexPart::PhaseDeg)
            return QStringLiteral("deg");
    }
    return trace.unit;
}

// ---------------------------------------------------------------- Smith, polar

void PlotWidget::setSmithZ0(double z0)
{
    if (z0 > 0) {
        m_z0 = z0;
        update();
        Q_EMIT markersChanged();
    }
}

void PlotWidget::setAdmittanceGridVisible(bool visible)
{
    m_admittanceGrid = visible;
    if (m_admittanceAction->isChecked() != visible)
        m_admittanceAction->setChecked(visible);
    update();
}

int PlotWidget::addCircle(const PlotCircle &circle)
{
    const int id = m_nextCircleId++;
    m_circles.emplace_back(id, circle);
    update();
    return id;
}

void PlotWidget::removeCircle(int circleId)
{
    std::erase_if(m_circles, [&](const auto &c) { return c.first == circleId; });
    update();
}

void PlotWidget::clearCircles()
{
    m_circles.clear();
    update();
}

void PlotWidget::setPolarScale(PolarScale scale, double floorDb)
{
    m_polarScale = scale;
    if (std::isfinite(floorDb))
        m_polarFloorDb = floorDb;
    for (auto &t : m_traces)
        traceDataChanged(*t); // polar maxima depend on the scale
    refreshAutoRanges();
    invalidate();
}

void PlotWidget::setPolarOrientation(PolarOrientation orientation)
{
    m_polarOrientation = orientation;
    update();
}

void PlotWidget::setPolarMaximum(double maximum)
{
    m_polarMax = maximum > polarMinimum() ? maximum : NAN;
    invalidate();
}

double PlotWidget::polarMinimum() const
{
    return m_polarScale == PolarScale::Decibel ? m_polarFloorDb : 0.0;
}

double PlotWidget::polarMaximum() const
{
    return std::isfinite(m_polarMax) ? m_polarMax : m_polarAutoMax;
}

double PlotWidget::polarValueOf(std::complex<double> z) const
{
    return m_polarScale == PolarScale::Decibel ? magnitudeDb(z) : std::abs(z);
}

double PlotWidget::polarValue(const Trace &trace, std::size_t i) const
{
    return trace.isComplex ? polarValueOf(trace.z[i]) : trace.y[i];
}

double PlotWidget::polarAngle(const Trace &trace, std::size_t i) const
{
    return trace.isComplex ? std::arg(trace.z[i]) * kRadToDeg : trace.x[i];
}

double PlotWidget::polarRadius(double value) const
{
    const double lo = polarMinimum();
    const double hi = polarMaximum();
    if (std::isnan(value))
        return NAN;
    return std::max(0.0, (value - lo) / (hi - lo));
}

std::complex<double> PlotWidget::polarPoint(double angleDeg, double value) const
{
    const double r = polarRadius(value);
    double angle = angleDeg;
    if (m_polarOrientation == PolarOrientation::Compass)
        angle = 90.0 - angleDeg;
    return std::polar(r, angle / kRadToDeg);
}

// ---------------------------------------------------------------- values

std::optional<PlotWidget::SampledValue> PlotWidget::sample(const Trace &trace, double x) const
{
    const auto at = locate(trace.x, trace.xAscending, x);
    if (!at)
        return std::nullopt;
    const auto [i, t] = *at;
    SampledValue v;
    if (trace.size() == 1) {
        v.y = trace.y[0];
        if (trace.isComplex)
            v.z = trace.z[0];
        return v;
    }
    v.y = trace.y[i] + (trace.y[i + 1] - trace.y[i]) * t;
    if (trace.isComplex)
        v.z = trace.z[i] + (trace.z[i + 1] - trace.z[i]) * t;
    return v;
}

std::optional<std::complex<double>> PlotWidget::planePoint(const Trace &trace, double x) const
{
    const auto v = sample(trace, x);
    if (!v)
        return std::nullopt;
    if (m_kind == PlotKind::Smith)
        return trace.isComplex ? std::optional(v->z) : std::nullopt;
    if (trace.isComplex)
        return polarPoint(std::arg(v->z) * kRadToDeg, polarValueOf(v->z));
    return polarPoint(x, v->y);
}

QString PlotWidget::describeGamma(std::complex<double> gamma) const
{
    const double vswr = vswrFromGamma(gamma);
    return tr("%1 = %2   Z = %3   VSWR = %4")
        .arg(QString(QChar(0x0393)), formatGamma(gamma), formatImpedance(impedanceFromGamma(gamma, m_z0)),
             std::isfinite(vswr) ? trimmedNumber(vswr, vswr < 10 ? 3 : 1) : QString(QChar(0x221E)));
}

QString PlotWidget::describe(const Trace &trace, double x) const
{
    const auto v = sample(trace, x);
    if (!v)
        return {};
    if (m_kind == PlotKind::Smith && trace.isComplex)
        return tr("%1   %2").arg(formatValue(x, m_x.unit, 6), describeGamma(v->z));
    if (m_kind == PlotKind::Polar) {
        if (trace.isComplex) {
            const QString unit = m_polarScale == PolarScale::Decibel ? QStringLiteral("dB") : trace.unit;
            return tr("%1   %2 at %3").arg(formatValue(x, m_x.unit, 6), formatValue(polarValueOf(v->z), unit, 4),
                                          formatValue(std::arg(v->z) * kRadToDeg, QStringLiteral("deg"), 4));
        }
        return tr("%1   %2").arg(formatValue(x, QStringLiteral("deg"), 4), formatValue(v->y, trace.unit, 4));
    }
    return tr("%1   %2").arg(formatValue(x, m_x.unit, 6), formatValue(v->y, shownUnit(trace), 4));
}

PlotMarkerInfo PlotWidget::markerInfo(const Marker &marker) const
{
    PlotMarkerInfo info;
    info.id = marker.id;
    info.traceId = marker.traceId;
    info.name = QStringLiteral("M%1").arg(marker.number);
    info.x = marker.x;
    info.reference = marker.id == m_referenceMarker;
    const Trace *t = findTrace(marker.traceId);
    if (!t)
        return info;
    info.xUnit = (m_kind == PlotKind::Polar && !t->isComplex) ? QStringLiteral("deg") : m_x.unit;
    info.yUnit = shownUnit(*t);
    info.isComplex = t->isComplex;
    if (const auto v = sample(*t, marker.x)) {
        info.valid = true;
        info.y = v->y;
        info.value = v->z;
    }
    return info;
}

QString PlotWidget::markerLine(const Marker &marker) const
{
    const PlotMarkerInfo info = markerInfo(marker);
    const Trace *t = findTrace(marker.traceId);
    if (!t)
        return {};
    QString line = info.name + QLatin1Char('\t') + t->name + QLatin1Char('\t');
    if (!info.valid)
        return line + tr("out of range");
    const QString delta = QString(QChar(0x0394));
    if (m_kind == PlotKind::Smith && t->isComplex) {
        const double vswr = vswrFromGamma(info.value);
        line += formatValue(info.x, info.xUnit, 6) + QLatin1Char('\t') + formatGamma(info.value) + QLatin1Char('\t')
                + formatImpedance(impedanceFromGamma(info.value, m_z0)) + QLatin1Char('\t')
                + tr("VSWR %1").arg(std::isfinite(vswr) ? trimmedNumber(vswr, vswr < 10 ? 2 : 1)
                                                        : QString(QChar(0x221E)));
        return line;
    }
    if (m_kind == PlotKind::Polar) {
        if (t->isComplex) {
            const QString unit = m_polarScale == PolarScale::Decibel ? QStringLiteral("dB") : t->unit;
            return line + formatValue(info.x, info.xUnit, 6) + QLatin1Char('\t')
                   + formatValue(polarValueOf(info.value), unit, 4) + QLatin1Char('\t')
                   + formatValue(std::arg(info.value) * kRadToDeg, QStringLiteral("deg"), 4);
        }
        return line + formatValue(info.x, info.xUnit, 4) + QLatin1Char('\t') + formatValue(info.y, info.yUnit, 4);
    }
    line += formatValue(info.x, info.xUnit, 6) + QLatin1Char('\t') + formatValue(info.y, info.yUnit, 4);
    if (m_referenceMarker >= 0 && marker.id != m_referenceMarker) {
        const auto ref = std::find_if(m_markers.begin(), m_markers.end(),
                                      [&](const Marker &m) { return m.id == m_referenceMarker; });
        if (ref != m_markers.end()) {
            const PlotMarkerInfo r = markerInfo(*ref);
            if (r.valid) {
                line += QLatin1Char('\t') + delta + QLatin1Char(' ') + formatValue(info.x - r.x, info.xUnit, 4);
                const QString yUnit = info.yUnit == r.yUnit ? info.yUnit : QString();
                line += QLatin1Char('\t') + delta + QLatin1Char(' ') + formatValue(info.y - r.y, yUnit, 4);
            }
        }
    }
    return line;
}

// ---------------------------------------------------------------- markers

int PlotWidget::addMarker(int traceId, double x)
{
    if (!findTrace(traceId))
        return -1;
    Marker m;
    m.id = m_nextMarkerId++;
    m.traceId = traceId;
    m.number = m_nextMarkerNumber++;
    m.x = x;
    m_markers.push_back(m);
    invalidate();
    Q_EMIT markersChanged();
    return m.id;
}

void PlotWidget::setMarkerX(int markerId, double x)
{
    for (Marker &m : m_markers) {
        if (m.id == markerId && m.x != x) {
            m.x = x;
            update();
            Q_EMIT markersChanged();
        }
    }
}

void PlotWidget::removeMarker(int markerId)
{
    const auto before = m_markers.size();
    std::erase_if(m_markers, [&](const Marker &m) { return m.id == markerId; });
    if (m_referenceMarker == markerId)
        m_referenceMarker = -1;
    if (m_markers.empty())
        m_nextMarkerNumber = 1;
    if (before != m_markers.size()) {
        invalidate();
        Q_EMIT markersChanged();
    }
}

void PlotWidget::clearMarkers()
{
    if (m_markers.empty())
        return;
    m_markers.clear();
    m_referenceMarker = -1;
    m_nextMarkerNumber = 1;
    invalidate();
    Q_EMIT markersChanged();
}

std::vector<PlotMarkerInfo> PlotWidget::markers() const
{
    std::vector<PlotMarkerInfo> result;
    for (const Marker &m : m_markers)
        result.push_back(markerInfo(m));
    return result;
}

std::optional<PlotMarkerInfo> PlotWidget::marker(int markerId) const
{
    for (const Marker &m : m_markers)
        if (m.id == markerId)
            return markerInfo(m);
    return std::nullopt;
}

void PlotWidget::setReferenceMarker(int markerId)
{
    m_referenceMarker = markerId;
    update();
    Q_EMIT markersChanged();
}

std::optional<double> PlotWidget::findPeak(int traceId, bool maximum, double xMin, double xMax) const
{
    const Trace *t = findTrace(traceId);
    if (!t)
        return std::nullopt;
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < t->size(); ++i) {
        const double x = t->x[i];
        const double y = m_kind == PlotKind::Polar ? polarValue(*t, i) : t->y[i];
        if (!std::isfinite(y) || !(x >= xMin && x <= xMax))
            continue;
        const double current = best ? (m_kind == PlotKind::Polar ? polarValue(*t, *best) : t->y[*best]) : 0.0;
        if (!best || (maximum ? y > current : y < current))
            best = i;
    }
    if (!best)
        return std::nullopt;
    return t->x[*best];
}

int PlotWidget::addPeakMarker(int traceId, bool maximum)
{
    const auto x = findPeak(traceId, maximum);
    return x ? addMarker(traceId, *x) : -1;
}

QStringList PlotWidget::markerTable() const
{
    QStringList lines;
    for (const Marker &m : m_markers) {
        QString line = markerLine(m);
        if (m.id == m_referenceMarker)
            line += QLatin1Char('\t') + tr("ref");
        lines << line;
    }
    return lines;
}

// ---------------------------------------------------------------- mapping

const PlotWidget::Layout &PlotWidget::widgetLayout() const
{
    if (!m_layout)
        m_layout = std::make_unique<Layout>(computeLayout(QRectF(rect()), const_cast<PlotWidget *>(this), true));
    return *m_layout;
}

QRectF PlotWidget::plotArea() const
{
    return widgetLayout().plotRect;
}

QPointF PlotWidget::dataToPixel(double x, double y, PlotYAxis side) const
{
    const Layout &l = widgetLayout();
    return {l.x.toPixel(x), (side == PlotYAxis::Left ? l.yLeft : l.yRight).toPixel(y)};
}

QPointF PlotWidget::pixelToData(const QPointF &pixel, PlotYAxis side) const
{
    const Layout &l = widgetLayout();
    return {l.x.fromPixel(pixel.x()), (side == PlotYAxis::Left ? l.yLeft : l.yRight).fromPixel(pixel.y())};
}

QPointF PlotWidget::gammaToPixel(std::complex<double> gamma) const
{
    return widgetLayout().gammaToPixel(gamma);
}

std::complex<double> PlotWidget::pixelToGamma(const QPointF &pixel) const
{
    return widgetLayout().pixelToGamma(pixel);
}

QRectF PlotWidget::legendItemRect(int traceId) const
{
    for (const auto &[id, r] : widgetLayout().legendItems)
        if (id == traceId)
            return r;
    return {};
}

// ---------------------------------------------------------------- interaction

int PlotWidget::markerAt(const QPointF &position) const
{
    const Layout &l = widgetLayout();
    for (auto it = m_markers.rbegin(); it != m_markers.rend(); ++it) {
        const auto p = markerPixel(l, *it);
        if (!p)
            continue;
        // The triangle sits above its tip.
        const QPointF handle(p->x(), p->y() - 7);
        if (QLineF(handle, position).length() <= 10 || QLineF(*p, position).length() <= 6)
            return it->id;
    }
    return -1;
}

int PlotWidget::legendItemAt(const QPointF &position) const
{
    for (const auto &[id, r] : widgetLayout().legendItems)
        if (r.contains(position))
            return id;
    return -1;
}

std::optional<double> PlotWidget::nearestX(const Trace &trace, const QPointF &position, double *distance) const
{
    if (trace.size() == 0)
        return std::nullopt;
    const Layout &l = widgetLayout();
    if (m_kind == PlotKind::Rectangular) {
        double x = l.x.fromPixel(position.x());
        if (!std::isfinite(x))
            return std::nullopt;
        x = std::clamp(x, trace.xMin, trace.xMax);
        if (distance) {
            const auto v = sample(trace, x);
            const AxisScale &ys = trace.style.yAxis == PlotYAxis::Left ? l.yLeft : l.yRight;
            *distance = v ? std::abs(ys.toPixel(v->y) - position.y()) : INFINITY;
        }
        return x;
    }
    // Smith and polar: closest point on the drawn polyline.
    const auto pointAt = [&](std::size_t i) -> QPointF {
        std::complex<double> g;
        if (m_kind == PlotKind::Smith) {
            if (!trace.isComplex)
                return {NAN, NAN};
            g = trace.z[i];
        } else {
            g = polarPoint(polarAngle(trace, i), polarValue(trace, i));
        }
        return l.gammaToPixel(g);
    };
    double best = INFINITY;
    double bestX = NAN;
    QPointF previous = pointAt(0);
    if (trace.size() == 1) {
        best = QLineF(previous, position).length();
        bestX = trace.x[0];
    }
    for (std::size_t i = 0; i + 1 < trace.size(); ++i) {
        const QPointF next = pointAt(i + 1);
        const QPointF d = next - previous;
        const double len2 = QPointF::dotProduct(d, d);
        double t = len2 > 0 ? QPointF::dotProduct(position - previous, d) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const double dist = QLineF(previous + d * t, position).length();
        if (dist < best) {
            best = dist;
            bestX = trace.x[i] + (trace.x[i + 1] - trace.x[i]) * t;
        }
        previous = next;
    }
    if (!std::isfinite(best))
        return std::nullopt;
    if (distance)
        *distance = best;
    return bestX;
}

void PlotWidget::setHover(const PlotReadout &readout)
{
    const bool changed = readout.text != m_hover.text;
    m_hover = readout;
    if (changed)
        Q_EMIT hoverReadoutChanged(readout.text);
}

void PlotWidget::updateHover(const QPointF &position)
{
    const Layout &l = widgetLayout();
    if (!l.plotRect.contains(position)) {
        setHover({});
        return;
    }
    PlotReadout best;
    double bestDistance = INFINITY;
    for (const auto &t : m_traces) {
        if (!t->visible || t->size() == 0 || (m_kind == PlotKind::Smith && !t->isComplex))
            continue;
        double distance = INFINITY;
        const auto x = nearestX(*t, position, &distance);
        if (!x || distance >= bestDistance)
            continue;
        if (m_kind == PlotKind::Rectangular) {
            // Only traces present under the cursor.
            const double cursorX = l.x.fromPixel(position.x());
            if (!sample(*t, cursorX))
                continue;
        } else if (distance > 24) {
            continue;
        }
        const auto v = sample(*t, *x);
        if (!v)
            continue;
        bestDistance = distance;
        best.valid = true;
        best.traceId = t->id;
        best.x = *x;
        best.y = v->y;
        best.value = v->z;
        best.text = t->name + QStringLiteral(":   ") + describe(*t, *x);
    }
    if (!best.valid) {
        best.valid = true;
        if (m_kind == PlotKind::Rectangular) {
            const QPointF d(l.x.fromPixel(position.x()), l.yLeft.fromPixel(position.y()));
            best.x = d.x();
            best.y = d.y();
            best.text = tr("%1   %2").arg(formatValue(d.x(), m_x.unit, 5), formatValue(d.y(), m_yLeft.unit, 4));
        } else if (m_kind == PlotKind::Smith) {
            best.value = l.pixelToGamma(position);
            best.text = describeGamma(best.value);
        } else {
            const std::complex<double> g = l.pixelToGamma(position);
            double angle = std::arg(g) * kRadToDeg;
            if (m_polarOrientation == PolarOrientation::Compass)
                angle = 90.0 - angle;
            angle = std::fmod(angle + 360.0, 360.0);
            const double value = polarMinimum() + std::abs(g) * (polarMaximum() - polarMinimum());
            best.x = angle;
            best.y = value;
            best.text = tr("%1   %2").arg(formatValue(angle, QStringLiteral("deg"), 4),
                                          formatValue(value, m_polarScale == PolarScale::Decibel ? QStringLiteral("dB")
                                                                                                 : QString(), 4));
        }
    }
    setHover(best);
}

void PlotWidget::zoomAxes(const QPointF &position, double factor, bool zoomX, bool zoomY)
{
    const Layout &l = widgetLayout();
    if (m_kind == PlotKind::Rectangular) {
        if (zoomX) {
            m_x.scale.zoom(factor, l.x.fromPixel(position.x()));
            m_x.autoRange = false;
        }
        if (zoomY) {
            m_yLeft.scale.zoom(factor, l.yLeft.fromPixel(position.y()));
            m_yRight.scale.zoom(factor, l.yRight.fromPixel(position.y()));
            m_yLeft.autoRange = m_yRight.autoRange = false;
        }
    } else {
        const std::complex<double> g = l.pixelToGamma(position);
        const double half = m_viewHalfSize > 0 ? m_viewHalfSize : 1.0;
        const double newHalf = std::clamp(half * factor, 1e-3, 4.0);
        const double f = newHalf / half;
        m_viewCentre = g + (l.viewCentre - g) * f;
        m_viewHalfSize = newHalf;
    }
    invalidate();
    Q_EMIT viewChanged();
}

void PlotWidget::zoomAt(const QPointF &position, double factor)
{
    zoomAxes(position, factor, true, true);
}

void PlotWidget::zoomToRect(const QRectF &r)
{
    const Layout &l = widgetLayout();
    if (m_kind == PlotKind::Rectangular) {
        const double x0 = l.x.fromPixel(r.left());
        const double x1 = l.x.fromPixel(r.right());
        const double yl0 = l.yLeft.fromPixel(r.bottom());
        const double yl1 = l.yLeft.fromPixel(r.top());
        const double yr0 = l.yRight.fromPixel(r.bottom());
        const double yr1 = l.yRight.fromPixel(r.top());
        if (x1 > x0) {
            m_x.scale.setRange(x0, x1);
            m_x.autoRange = false;
        }
        if (yl1 > yl0) {
            m_yLeft.scale.setRange(yl0, yl1);
            m_yLeft.autoRange = false;
        }
        if (yr1 > yr0) {
            m_yRight.scale.setRange(yr0, yr1);
            m_yRight.autoRange = false;
        }
    } else {
        m_viewCentre = l.pixelToGamma(r.center());
        m_viewHalfSize = std::clamp(std::max(r.width(), r.height()) / 2 / l.scale, 1e-3, 4.0);
    }
    invalidate();
    Q_EMIT viewChanged();
}

void PlotWidget::mousePressEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    m_pressPos = m_lastPos = pos;
    if (event->button() == Qt::LeftButton) {
        const int legendTrace = legendItemAt(pos);
        if (legendTrace >= 0) {
            setTraceVisible(legendTrace, !isTraceVisible(legendTrace));
            return;
        }
        const int marker = markerAt(pos);
        if (marker >= 0) {
            m_drag = Drag::Marker;
            m_dragMarker = marker;
            setCursor(Qt::SizeHorCursor);
            return;
        }
        if (widgetLayout().plotRect.contains(pos)) {
            m_drag = Drag::RubberBand;
            return;
        }
    } else if (event->button() == Qt::MiddleButton && widgetLayout().plotRect.contains(pos)) {
        m_drag = Drag::Pan;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    QWidget::mousePressEvent(event);
}

void PlotWidget::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    m_hoverPos = pos;
    m_hovering = true;
    switch (m_drag) {
    case Drag::Marker:
        for (const Marker &m : m_markers) {
            if (m.id != m_dragMarker)
                continue;
            if (const Trace *t = findTrace(m.traceId)) {
                if (const auto x = nearestX(*t, pos))
                    setMarkerX(m.id, *x);
            }
            break;
        }
        break;
    case Drag::Pan: {
        const QPointF d = pos - m_lastPos;
        if (m_kind == PlotKind::Rectangular) {
            m_x.scale.pan(d.x());
            m_yLeft.scale.pan(d.y());
            m_yRight.scale.pan(d.y());
            m_x.autoRange = m_yLeft.autoRange = m_yRight.autoRange = false;
        } else {
            const Layout &l = widgetLayout();
            m_viewCentre -= std::complex<double>(d.x() / l.scale, -d.y() / l.scale);
            if (m_viewHalfSize == 0)
                m_viewHalfSize = 1.0;
        }
        invalidate();
        Q_EMIT viewChanged();
        break;
    }
    case Drag::RubberBand:
        update();
        break;
    case Drag::None:
        break;
    }
    m_lastPos = pos;
    if (m_drag != Drag::Pan)
        updateHover(pos);
    if (m_drag == Drag::None) {
        const bool overMarker = markerAt(pos) >= 0;
        const bool overLegend = legendItemAt(pos) >= 0;
        setCursor(overMarker ? Qt::SizeHorCursor : overLegend ? Qt::PointingHandCursor : Qt::CrossCursor);
    }
    update();
}

void PlotWidget::mouseReleaseEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    if (m_drag == Drag::RubberBand && event->button() == Qt::LeftButton) {
        const QRectF band = QRectF(m_pressPos, pos).normalized().intersected(widgetLayout().plotRect);
        if (band.width() > 5 && band.height() > 5)
            zoomToRect(band);
    }
    if (m_drag != Drag::None) {
        m_drag = Drag::None;
        m_dragMarker = -1;
        setCursor(Qt::CrossCursor);
        update();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PlotWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && legendItemAt(event->position()) < 0) {
        m_drag = Drag::None;
        autoscale();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void PlotWidget::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0) {
        event->ignore();
        return;
    }
    const QPointF pos = event->position();
    const QRectF plot = widgetLayout().plotRect;
    const double factor = std::pow(1.0 / 1.25, steps);
    bool zoomX = true;
    bool zoomY = true;
    if (m_kind == PlotKind::Rectangular) {
        // Over an axis or with a modifier, zoom one direction only.
        if (event->modifiers() & Qt::ShiftModifier || (pos.y() > plot.bottom() && pos.x() >= plot.left()))
            zoomY = false;
        else if (event->modifiers() & Qt::ControlModifier || pos.x() < plot.left() || pos.x() > plot.right())
            zoomX = false;
    }
    QPointF anchor(std::clamp(pos.x(), plot.left(), plot.right()), std::clamp(pos.y(), plot.top(), plot.bottom()));
    zoomAxes(anchor, factor, zoomX, zoomY);
    event->accept();
}

void PlotWidget::leaveEvent(QEvent *event)
{
    m_hovering = false;
    setHover({});
    update();
    QWidget::leaveEvent(event);
}

void PlotWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(m_autoscaleAction);
    menu.addSeparator();
    const QPointF pos = event->pos();
    // Markers: add on the trace under the cursor, peak search, remove.
    int traceId = m_hover.valid ? m_hover.traceId : -1;
    if (traceId < 0) {
        for (const auto &t : m_traces) {
            if (t->visible && t->size() > 0 && !(m_kind == PlotKind::Smith && !t->isComplex)) {
                traceId = t->id;
                break;
            }
        }
    }
    const int markerUnder = markerAt(pos);
    if (traceId >= 0) {
        const Trace *t = findTrace(traceId);
        const QString name = t ? t->name : QString();
        const double hoverX = m_hover.traceId == traceId ? m_hover.x : NAN;
        QAction *add = menu.addAction(tr("Add Marker on %1").arg(name));
        add->setEnabled(std::isfinite(hoverX));
        connect(add, &QAction::triggered, this, [this, traceId, hoverX] { addMarker(traceId, hoverX); });
        connect(menu.addAction(tr("Marker at Maximum of %1").arg(name)), &QAction::triggered, this,
                [this, traceId] { addPeakMarker(traceId, true); });
        connect(menu.addAction(tr("Marker at Minimum of %1").arg(name)), &QAction::triggered, this,
                [this, traceId] { addPeakMarker(traceId, false); });
    }
    if (markerUnder >= 0) {
        connect(menu.addAction(tr("Use as Reference Marker")), &QAction::triggered, this,
                [this, markerUnder] { setReferenceMarker(markerUnder); });
        connect(menu.addAction(tr("Remove Marker")), &QAction::triggered, this,
                [this, markerUnder] { removeMarker(markerUnder); });
    }
    if (!m_markers.empty())
        connect(menu.addAction(tr("Remove All Markers")), &QAction::triggered, this, &PlotWidget::clearMarkers);
    menu.addSeparator();
    menu.addAction(m_gridAction);
    if (m_kind == PlotKind::Smith)
        menu.addAction(m_admittanceAction);
    menu.addSeparator();
    menu.addAction(m_copyAction);
    menu.addAction(m_exportPngAction);
    menu.addAction(m_exportSvgAction);
    menu.addAction(m_exportCsvAction);
    menu.exec(event->globalPos());
}

void PlotWidget::resizeEvent(QResizeEvent *event)
{
    m_layout.reset();
    QWidget::resizeEvent(event);
}

void PlotWidget::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) {
        updateTheme();
        invalidate();
    }
    QWidget::changeEvent(event);
}

void PlotWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    paintPlot(painter, QRectF(rect()), true);
}

// ---------------------------------------------------------------- export

QImage PlotWidget::renderImage(const QSize &size, qreal devicePixelRatio) const
{
    const QSize logical = size.isValid() && !size.isEmpty() ? size : this->size();
    QImage image(logical * devicePixelRatio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(m_theme.surround);
    QPainter painter(&image);
    paintPlot(painter, QRectF(QPointF(0, 0), QSizeF(logical)), false);
    return image;
}

void PlotWidget::render(QPainter *painter, const QRectF &target) const
{
    paintPlot(*painter, target, false);
}

bool PlotWidget::exportImage(const QString &path, const QSize &size) const
{
    return renderImage(size).save(path);
}

bool PlotWidget::svgSupported()
{
#ifdef LVD_PLOT_HAVE_SVG
    return true;
#else
    return false;
#endif
}

bool PlotWidget::exportSvg(const QString &path, const QSize &size) const
{
#ifdef LVD_PLOT_HAVE_SVG
    const QSize logical = size.isValid() && !size.isEmpty() ? size : this->size();
    QSvgGenerator generator;
    generator.setFileName(path);
    generator.setSize(logical);
    generator.setViewBox(QRect(QPoint(0, 0), logical));
    generator.setResolution(96);
    generator.setTitle(m_title.isEmpty() ? tr("Plot") : m_title);
    generator.setDescription(tr("Exported from lvd RF cad"));
    QPainter painter;
    if (!painter.begin(&generator))
        return false;
    painter.fillRect(QRectF(QPointF(0, 0), QSizeF(logical)), m_theme.surround);
    paintPlot(painter, QRectF(QPointF(0, 0), QSizeF(logical)), false);
    return painter.end();
#else
    Q_UNUSED(path);
    Q_UNUSED(size);
    return false;
#endif
}

QString PlotWidget::csvText() const
{
    // One shared x column when all traces use the same x values; otherwise
    // an x column per trace. Complex traces give real and imaginary parts.
    QString text;
    QTextStream out(&text);
    if (m_traces.empty())
        return text;
    bool sharedX = true;
    for (const auto &t : m_traces)
        sharedX = sharedX && t->x == m_traces.front()->x;
    const QString xHeader = m_x.unit.isEmpty() ? m_x.label : QStringLiteral("%1 (%2)").arg(m_x.label, m_x.unit);
    QStringList header;
    if (sharedX)
        header << csvField(xHeader);
    for (const auto &t : m_traces) {
        const QString xName = t->isComplex || m_kind != PlotKind::Polar ? xHeader : tr("Angle (deg)");
        if (!sharedX)
            header << csvField(t->name + QStringLiteral(" ") + xName);
        const QString unit = t->unit.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(t->unit);
        if (t->isComplex)
            header << csvField(t->name + tr(" re") + unit) << csvField(t->name + tr(" im") + unit);
        else
            header << csvField(t->name + unit);
    }
    out << header.join(QLatin1Char(',')) << '\n';
    std::size_t rows = 0;
    for (const auto &t : m_traces)
        rows = std::max(rows, t->size());
    for (std::size_t r = 0; r < rows; ++r) {
        QStringList row;
        if (sharedX)
            row << csvNumber(m_traces.front()->x[r]);
        for (const auto &t : m_traces) {
            const bool has = r < t->size();
            if (!sharedX)
                row << (has ? csvNumber(t->x[r]) : QString());
            if (t->isComplex)
                row << (has ? csvNumber(t->z[r].real()) : QString()) << (has ? csvNumber(t->z[r].imag()) : QString());
            else
                row << (has ? csvNumber(t->y[r]) : QString());
        }
        out << row.join(QLatin1Char(',')) << '\n';
    }
    out.flush();
    return text;
}

bool PlotWidget::exportCsv(const QString &path) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(csvText().toUtf8());
    return file.commit();
}

void PlotWidget::copyImageToClipboard() const
{
    if (QClipboard *clipboard = QApplication::clipboard())
        clipboard->setImage(renderImage(size(), 2.0).convertToFormat(QImage::Format_ARGB32));
}

} // namespace lvd

#include "plot/ColorMapPlot.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QFontMetricsF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QTextStream>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace lvd {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kPixelLimit = 1e5;
constexpr double kFixedMarkerDiameter = 8.0;

double clampPixel(double p)
{
    return std::isfinite(p) ? std::clamp(p, -kPixelLimit, kPixelLimit) : p;
}

// Crisp one-pixel lines: snap to pixel centres.
double snap(double p)
{
    return std::floor(p) + 0.5;
}

QString axisTitle(const QString &label, const QString &unit)
{
    if (unit.isEmpty() || unitTakesPrefix(unit))
        return label;
    if (label.isEmpty())
        return displayUnit(unit);
    return QStringLiteral("%1 (%2)").arg(label, displayUnit(unit));
}

double widestLabel(const QFontMetricsF &fm, const AxisTicks &ticks, const QString &unit)
{
    double w = 0;
    for (double v : ticks.major)
        w = std::max(w, fm.horizontalAdvance(formatTick(v, ticks.step, unit)));
    return w;
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

QString csvHeader(const QString &label, const QString &unit, const QString &fallback)
{
    const QString name = label.isEmpty() ? fallback : label;
    return unit.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, unit);
}

// Lookup table of a colour map, for building raster images quickly.
std::array<QRgb, 256> colorTable(ColorMap map)
{
    std::array<QRgb, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i)
        table[i] = colorMapColor(map, static_cast<double>(i) / 255.0).rgba();
    return table;
}

// A small label with a halo in the plot background colour, readable over the
// raster and on both themes.
void drawHaloText(QPainter &painter, const QFont &font, const QPointF &baseline, const QString &text,
                  const QColor &color, const QColor &halo)
{
    QPainterPath path;
    path.addText(baseline, font, text);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.strokePath(path, QPen(halo, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.fillPath(path, color);
    painter.restore();
}

} // namespace

struct ColorMapPlot::Layout {
    QRectF target;
    QFont font;
    QFont titleFont;
    QFont smallFont;
    QRectF titleRect;
    QRectF plotRect;
    QRectF barRect;
    bool showBar = false;
    double pad = 6;
    double tick = 4;
    double leftBlock = 0;
    double xMin = 0, xMax = 1, yMin = 0, yMax = 1;
    double kx = 1, ky = 1; // pixels per data unit
    double barMin = 0, barMax = 1;
    AxisTicks xTicks;
    AxisTicks yTicks;
    AxisTicks barTicks;

    QPointF toPixel(const QPointF &d) const
    {
        return {plotRect.left() + (d.x() - xMin) * kx, plotRect.bottom() - (d.y() - yMin) * ky};
    }
    QPointF toData(const QPointF &p) const
    {
        return {xMin + (p.x() - plotRect.left()) / kx, yMin + (plotRect.bottom() - p.y()) / ky};
    }
};

QRectF ColorMapPlot::Raster::extent() const
{
    return QRectF(x0 - dx / 2, y0 - dy / 2, (x1 - x0) + dx, (y1 - y0) + dy);
}

// ---------------------------------------------------------------- setup

ColorMapPlot::ColorMapPlot(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    updateTheme();
    createActions();
}

ColorMapPlot::~ColorMapPlot() = default;

void ColorMapPlot::createActions()
{
    m_autoscaleAction = new QAction(tr("&Autoscale"), this);
    m_autoscaleAction->setToolTip(tr("Reset the view to show all data (double-click)"));
    connect(m_autoscaleAction, &QAction::triggered, this, &ColorMapPlot::autoscale);

    m_exportPngAction = new QAction(tr("Export &PNG..."), this);
    connect(m_exportPngAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Export PNG"), {}, tr("PNG images (*.png)"));
        if (!path.isEmpty())
            exportImage(path, size());
    });
    m_exportCsvAction = new QAction(tr("Export &CSV..."), this);
    connect(m_exportCsvAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Export CSV"), {}, tr("CSV files (*.csv)"));
        if (!path.isEmpty())
            exportCsv(path);
    });
    m_copyAction = new QAction(tr("C&opy Image"), this);
    connect(m_copyAction, &QAction::triggered, this, &ColorMapPlot::copyImageToClipboard);

    m_colorMapMenu = new QMenu(tr("Colour &Map"), this);
    m_colorMapGroup = new QActionGroup(this);
    m_colorMapGroup->setExclusive(true);
    for (ColorMap map : {ColorMap::Viridis, ColorMap::Jet, ColorMap::Phase, ColorMap::Gray}) {
        QString name = colorMapName(map);
        name[0] = name[0].toUpper();
        QAction *action = m_colorMapMenu->addAction(name);
        action->setCheckable(true);
        action->setChecked(map == m_colorMap);
        action->setData(static_cast<int>(map));
        m_colorMapGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, map] { setColorMap(map); });
    }
}

QList<QAction *> ColorMapPlot::plotActions() const
{
    return {m_autoscaleAction, m_colorMapMenu->menuAction(), m_copyAction, m_exportPngAction, m_exportCsvAction};
}

QSize ColorMapPlot::sizeHint() const
{
    return {640, 480};
}

QSize ColorMapPlot::minimumSizeHint() const
{
    return {240, 180};
}

void ColorMapPlot::updateTheme()
{
    m_theme = PlotTheme::fromPalette(palette());
}

void ColorMapPlot::invalidate()
{
    m_layout.reset();
    update();
}

void ColorMapPlot::dataChanged()
{
    m_rasterDirty = true;
    if (m_colorAuto)
        refreshColorRange();
    if (m_autoView)
        m_view = dataBounds();
    invalidate();
}

// ---------------------------------------------------------------- settings

void ColorMapPlot::setTitle(const QString &title)
{
    m_title = title;
    invalidate();
}

void ColorMapPlot::setXAxis(const QString &label, const QString &unit)
{
    m_xLabel = label;
    m_xUnit = unit;
    invalidate();
}

void ColorMapPlot::setYAxis(const QString &label, const QString &unit)
{
    m_yLabel = label;
    m_yUnit = unit;
    invalidate();
}

void ColorMapPlot::setAspectLocked(bool locked)
{
    m_aspectLocked = locked;
    invalidate();
}

void ColorMapPlot::setGridVisible(bool visible)
{
    m_gridVisible = visible;
    update();
}

void ColorMapPlot::setSmoothImage(bool smooth)
{
    m_smooth = smooth;
    update();
}

void ColorMapPlot::setColorMap(ColorMap map)
{
    m_colorMap = map;
    m_rasterDirty = true;
    for (QAction *action : m_colorMapGroup->actions())
        action->setChecked(action->data().toInt() == static_cast<int>(map));
    update();
}

void ColorMapPlot::setColorBar(const QString &label, const QString &unit)
{
    m_barLabel = label;
    m_barUnit = unit;
    invalidate();
}

void ColorMapPlot::setColorBarVisible(bool visible)
{
    m_colorBarVisible = visible;
    invalidate();
}

void ColorMapPlot::setColorRange(double min, double max)
{
    if (!std::isfinite(min) || !std::isfinite(max))
        return;
    if (max < min)
        std::swap(min, max);
    m_colorAuto = false;
    m_colorMin = min;
    m_colorMax = max;
    m_rasterDirty = true;
    invalidate();
}

void ColorMapPlot::setColorAutoRange()
{
    m_colorAuto = true;
    refreshColorRange();
    m_rasterDirty = true;
    invalidate();
}

void ColorMapPlot::refreshColorRange()
{
    double lo = kInf;
    double hi = -kInf;
    const auto scan = [&](const std::vector<double> &values) {
        for (double v : values) {
            if (std::isfinite(v)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
    };
    scan(m_image.values);
    scan(m_values);
    if (lo > hi) {
        lo = 0;
        hi = 1;
    }
    m_colorMin = lo;
    m_colorMax = hi;
}

std::pair<double, double> ColorMapPlot::colorRange() const
{
    // A flat map still needs a span to map values and place bar ticks.
    if (m_colorMax > m_colorMin)
        return {m_colorMin, m_colorMax};
    const double half = std::max(0.5, std::abs(m_colorMin) * 0.05);
    return {m_colorMin - half, m_colorMin + half};
}

// ---------------------------------------------------------------- data

void ColorMapPlot::setImage(int nx, int ny, double x0, double x1, double y0, double y1, std::vector<double> values)
{
    if (nx <= 0 || ny <= 0 || !std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) || !std::isfinite(y1)) {
        clearImage();
        return;
    }
    const auto total = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
    values.resize(total, std::numeric_limits<double>::quiet_NaN());
    // Store ascending axes so drawing and look-up need one orientation only.
    if (x1 < x0) {
        std::swap(x0, x1);
        for (int iy = 0; iy < ny; ++iy) {
            const auto row = values.begin() + static_cast<std::ptrdiff_t>(iy) * nx;
            std::reverse(row, row + nx);
        }
    }
    if (y1 < y0) {
        std::swap(y0, y1);
        for (int iy = 0; iy < ny / 2; ++iy)
            std::swap_ranges(values.begin() + static_cast<std::ptrdiff_t>(iy) * nx,
                             values.begin() + static_cast<std::ptrdiff_t>(iy + 1) * nx,
                             values.begin() + static_cast<std::ptrdiff_t>(ny - 1 - iy) * nx);
    }
    Raster r;
    r.nx = nx;
    r.ny = ny;
    // A single column or row: the span is its width (1 when there is none).
    const auto cell = [](int n, double &a, double &b) {
        if (n > 1)
            return (b - a) / (n - 1);
        const double width = b > a ? b - a : 1.0;
        a = b = (a + b) / 2;
        return width;
    };
    r.dx = cell(nx, x0, x1);
    r.dy = cell(ny, y0, y1);
    if (!(r.dx > 0))
        r.dx = 1;
    if (!(r.dy > 0))
        r.dy = 1;
    r.x0 = x0;
    r.x1 = x1;
    r.y0 = y0;
    r.y1 = y1;
    r.values = std::move(values);
    m_image = std::move(r);
    dataChanged();
}

void ColorMapPlot::clearImage()
{
    m_image = Raster{};
    m_rasterCache = QImage();
    dataChanged();
}

void ColorMapPlot::setScatter(std::vector<QPointF> positions, std::vector<double> values, double sizeData)
{
    values.resize(positions.size(), std::numeric_limits<double>::quiet_NaN());
    m_positions = std::move(positions);
    m_values = std::move(values);
    m_scatterSize = std::isfinite(sizeData) && sizeData > 0 ? sizeData : 0.0;
    dataChanged();
}

void ColorMapPlot::setScatterValues(std::vector<double> values)
{
    values.resize(m_positions.size(), std::numeric_limits<double>::quiet_NaN());
    m_values = std::move(values);
    // Positions are unchanged: keep the view while the values update live.
    if (m_colorAuto)
        refreshColorRange();
    invalidate();
}

void ColorMapPlot::setScatterCrossed(std::vector<int> indices)
{
    m_crossed = std::move(indices);
    update();
}

void ColorMapPlot::clearScatter()
{
    m_positions.clear();
    m_values.clear();
    m_crossed.clear();
    m_scatterSize = 0;
    dataChanged();
}

// ---------------------------------------------------------------- overlays

int ColorMapPlot::addCircle(QPointF centre, double radius, const ColorMapOverlayStyle &style, const QString &label)
{
    Overlay o;
    o.id = m_nextOverlayId++;
    o.kind = Overlay::Kind::Circle;
    o.points = {centre};
    o.radius = std::abs(radius);
    o.style = style;
    o.label = label;
    m_overlays.push_back(std::move(o));
    if (m_autoView)
        m_view = dataBounds();
    invalidate();
    return m_overlays.back().id;
}

int ColorMapPlot::addPolyline(std::vector<QPointF> points, const ColorMapOverlayStyle &style, const QString &label)
{
    Overlay o;
    o.id = m_nextOverlayId++;
    o.kind = Overlay::Kind::Polyline;
    o.points = std::move(points);
    o.style = style;
    o.label = label;
    m_overlays.push_back(std::move(o));
    if (m_autoView)
        m_view = dataBounds();
    invalidate();
    return m_overlays.back().id;
}

int ColorMapPlot::addMarker(QPointF at, const QString &label, const ColorMapOverlayStyle &style)
{
    Overlay o;
    o.id = m_nextOverlayId++;
    o.kind = Overlay::Kind::Marker;
    o.points = {at};
    o.style = style;
    o.label = label;
    m_overlays.push_back(std::move(o));
    if (m_autoView)
        m_view = dataBounds();
    invalidate();
    return m_overlays.back().id;
}

void ColorMapPlot::removeOverlay(int id)
{
    std::erase_if(m_overlays, [id](const Overlay &o) { return o.id == id; });
    if (m_autoView)
        m_view = dataBounds();
    invalidate();
}

void ColorMapPlot::clearOverlays()
{
    m_overlays.clear();
    if (m_autoView)
        m_view = dataBounds();
    invalidate();
}

std::vector<int> ColorMapPlot::overlayIds() const
{
    std::vector<int> ids;
    for (const Overlay &o : m_overlays)
        ids.push_back(o.id);
    return ids;
}

// ---------------------------------------------------------------- view

QRectF ColorMapPlot::dataBounds() const
{
    double x0 = kInf, x1 = -kInf, y0 = kInf, y1 = -kInf;
    const auto add = [&](double a0, double a1, double b0, double b1) {
        if (!std::isfinite(a0) || !std::isfinite(a1) || !std::isfinite(b0) || !std::isfinite(b1))
            return;
        x0 = std::min(x0, a0);
        x1 = std::max(x1, a1);
        y0 = std::min(y0, b0);
        y1 = std::max(y1, b1);
    };
    if (hasImage()) {
        const QRectF e = m_image.extent();
        add(e.left(), e.right(), e.top(), e.bottom());
    }
    const auto addPoints = [&](const std::vector<QPointF> &points, double size, double fraction) {
        double p0 = kInf, p1 = -kInf, q0 = kInf, q1 = -kInf;
        for (const QPointF &p : points) {
            if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
                continue;
            p0 = std::min(p0, p.x());
            p1 = std::max(p1, p.x());
            q0 = std::min(q0, p.y());
            q1 = std::max(q1, p.y());
        }
        if (p0 > p1)
            return;
        // Room for the markers themselves, which have a size beyond their centres.
        const double span = std::max(p1 - p0, q1 - q0);
        double margin = size / 2 + fraction * span;
        if (!(margin > 0))
            margin = std::max({0.5, std::abs(p0) * 0.1, std::abs(q0) * 0.1});
        add(p0 - margin, p1 + margin, q0 - margin, q1 + margin);
    };
    if (!m_positions.empty())
        addPoints(m_positions, m_scatterSize, m_scatterSize > 0 ? 0.03 : 0.06);
    // Overlays (grating-lobe circles can be far away) only frame an empty plot.
    if (x0 > x1) {
        for (const Overlay &o : m_overlays) {
            if (o.kind == Overlay::Kind::Circle) {
                const QPointF c = o.points.front();
                add(c.x() - o.radius, c.x() + o.radius, c.y() - o.radius, c.y() + o.radius);
            } else {
                addPoints(o.points, 0, 0.05);
            }
        }
    }
    if (x0 > x1)
        return QRectF(-1, -1, 2, 2);
    if (!(x1 > x0)) {
        const double half = std::max(0.5, std::abs(x0) * 0.1);
        x0 -= half;
        x1 += half;
    }
    if (!(y1 > y0)) {
        const double half = std::max(0.5, std::abs(y0) * 0.1);
        y0 -= half;
        y1 += half;
    }
    return QRectF(x0, y0, x1 - x0, y1 - y0);
}

void ColorMapPlot::setView(const QRectF &range, bool automatic)
{
    if (!std::isfinite(range.left()) || !std::isfinite(range.top()) || !(range.width() > 0) || !(range.height() > 0)
        || !std::isfinite(range.width()) || !std::isfinite(range.height()))
        return;
    m_view = range;
    m_autoView = automatic;
    invalidate();
    Q_EMIT viewChanged();
}

void ColorMapPlot::setRange(double x0, double x1, double y0, double y1)
{
    setView(QRectF(std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0), std::abs(y1 - y0)), false);
}

void ColorMapPlot::autoscale()
{
    setView(dataBounds(), true);
}

QRectF ColorMapPlot::viewRange() const
{
    return m_view;
}

void ColorMapPlot::zoomAt(const QPointF &position, double factor)
{
    if (!(factor > 0) || !std::isfinite(factor))
        return;
    const Layout &l = widgetLayout();
    const QPointF c = l.toData(position);
    const double x0 = c.x() + (m_view.left() - c.x()) * factor;
    const double x1 = c.x() + (m_view.right() - c.x()) * factor;
    const double y0 = c.y() + (m_view.top() - c.y()) * factor;
    const double y1 = c.y() + (m_view.bottom() - c.y()) * factor;
    const double scale = std::max({1.0, std::abs(c.x()), std::abs(c.y())});
    if (!(x1 - x0 > 1e-12 * scale) || !(y1 - y0 > 1e-12 * scale) || !(x1 - x0 < 1e12 * scale))
        return;
    setView(QRectF(x0, y0, x1 - x0, y1 - y0), false);
}

// ---------------------------------------------------------------- layout

ColorMapPlot::Layout ColorMapPlot::computeLayout(const QRectF &target, QPaintDevice *device) const
{
    Layout l;
    l.target = target;
    l.font = font();
    l.titleFont = l.font;
    l.titleFont.setBold(true);
    l.titleFont.setPointSizeF(l.font.pointSizeF() * 1.2);
    l.smallFont = l.font;
    l.smallFont.setPointSizeF(l.font.pointSizeF() * 0.9);
    const QFontMetricsF fm(l.font, device);
    const QFontMetricsF tfm(l.titleFont, device);
    const double pad = std::round(fm.height() * 0.6);
    const double tick = std::round(fm.height() * 0.3);
    l.pad = pad;
    l.tick = tick;

    double top = target.top() + pad * 0.6;
    if (!m_title.isEmpty()) {
        l.titleRect = QRectF(target.left() + pad, top, target.width() - 2 * pad, tfm.height());
        top += tfm.height() + pad * 0.3;
    }
    const double availTop = top + fm.height() * 0.5;
    const double availBottom = target.bottom() - pad * 0.6 - fm.height() * 2 - pad * 0.6;
    const double availH = std::max(10.0, availBottom - availTop);

    l.xMin = m_view.left();
    l.xMax = m_view.right();
    l.yMin = m_view.top();
    l.yMax = m_view.bottom();
    const double spanX = l.xMax - l.xMin;
    const double spanY = l.yMax - l.yMin;

    // Colour bar block on the right: gap, bar, ticks, labels, rotated title.
    const auto [barMin, barMax] = colorRange();
    l.barMin = barMin;
    l.barMax = barMax;
    l.showBar = m_colorBarVisible && (hasImage() || !m_positions.empty());
    const double barWidth = std::max(12.0, std::round(fm.height() * 0.9));
    const double barGap = std::round(pad * 1.5);
    const bool barTitled = !axisTitle(m_barLabel, m_barUnit).isEmpty();
    double rightBlock = pad + fm.averageCharWidth() * 2;
    if (l.showBar) {
        l.barTicks = linearTicks(barMin, barMax, std::max(2, static_cast<int>(availH / (fm.height() * 2.2))));
        rightBlock = barGap + barWidth + tick + 3 + widestLabel(fm, l.barTicks, m_barUnit) + pad * 0.6
                     + (barTitled ? fm.height() : 0.0) + pad;
    }

    // The left block depends on the y tick labels, which depend on the plot
    // height (aspect lock): two passes settle it.
    const bool yTitled = !axisTitle(m_yLabel, m_yUnit).isEmpty();
    double plotW = 10;
    double plotH = availH;
    l.yTicks = linearTicks(l.yMin, l.yMax, std::max(2, static_cast<int>(availH / (fm.height() * 2.8))));
    for (int pass = 0; pass < 2; ++pass) {
        l.leftBlock = pad + (yTitled ? fm.height() + pad * 0.5 : 0.0) + widestLabel(fm, l.yTicks, m_yUnit) + tick + 3;
        const double availW = std::max(10.0, target.width() - l.leftBlock - rightBlock);
        plotW = availW;
        plotH = availH;
        if (m_aspectLocked) {
            const double s = std::min(availW / spanX, availH / spanY);
            plotW = std::clamp(s * spanX, 10.0, availW);
            plotH = std::clamp(s * spanY, 10.0, availH);
        }
        l.yTicks = linearTicks(l.yMin, l.yMax, std::max(2, static_cast<int>(plotH / (fm.height() * 2.8))));
    }
    // With the aspect locked, centre the plot with its axes and colour bar.
    const double total = l.leftBlock + plotW + rightBlock;
    const double left = target.left() + std::max(0.0, std::floor((target.width() - total) / 2)) + l.leftBlock;
    const double plotTop = availTop + std::floor((availH - plotH) / 2);
    l.plotRect = QRectF(left, plotTop, plotW, plotH);
    l.kx = plotW / spanX;
    l.ky = plotH / spanY;

    int xCount = std::max(2, static_cast<int>(plotW / (fm.averageCharWidth() * 12)));
    for (int attempt = 0; attempt < 8; ++attempt) {
        l.xTicks = linearTicks(l.xMin, l.xMax, xCount);
        const double need = static_cast<double>(l.xTicks.major.size())
                            * (widestLabel(fm, l.xTicks, m_xUnit) + fm.averageCharWidth() * 3);
        if (need <= plotW || xCount <= 2)
            break;
        xCount = std::max(2, static_cast<int>(xCount * 0.7));
    }

    if (l.showBar) {
        // A short plot (aspect lock) keeps a readable bar.
        const double barH = std::min(availH, std::max(plotH, std::min(availH, 160.0)));
        const double barTop = std::clamp(l.plotRect.center().y() - barH / 2, availTop, availBottom - barH);
        l.barRect = QRectF(l.plotRect.right() + barGap, barTop, barWidth, barH);
        l.barTicks = linearTicks(barMin, barMax, std::max(2, static_cast<int>(barH / (fm.height() * 2.2))));
    }
    return l;
}

const ColorMapPlot::Layout &ColorMapPlot::widgetLayout() const
{
    if (!m_layout)
        m_layout = std::make_unique<Layout>(computeLayout(QRectF(rect()), const_cast<ColorMapPlot *>(this)));
    return *m_layout;
}

QRectF ColorMapPlot::plotArea() const
{
    return widgetLayout().plotRect;
}

QRectF ColorMapPlot::colorBarArea() const
{
    return widgetLayout().barRect;
}

QPointF ColorMapPlot::dataToPixel(QPointF point) const
{
    return widgetLayout().toPixel(point);
}

QPointF ColorMapPlot::pixelToData(QPointF pixel) const
{
    return widgetLayout().toData(pixel);
}

// ---------------------------------------------------------------- painting

const QImage &ColorMapPlot::rasterImage() const
{
    if (!m_rasterDirty && !m_rasterCache.isNull())
        return m_rasterCache;
    m_rasterDirty = false;
    if (!hasImage()) {
        m_rasterCache = QImage();
        return m_rasterCache;
    }
    const auto table = colorTable(m_colorMap);
    const auto [lo, hi] = colorRange();
    const double scale = 255.0 / (hi - lo);
    const int nx = m_image.nx;
    const int ny = m_image.ny;
    m_rasterCache = QImage(nx, ny, QImage::Format_ARGB32_Premultiplied);
    const double *values = m_image.values.data();
    for (int iy = 0; iy < ny; ++iy) {
        // Image rows run top-down, data rows bottom-up.
        auto *line = reinterpret_cast<QRgb *>(m_rasterCache.scanLine(ny - 1 - iy));
        const double *row = values + static_cast<std::ptrdiff_t>(iy) * nx;
        for (int ix = 0; ix < nx; ++ix) {
            const double v = row[ix];
            if (std::isnan(v)) {
                line[ix] = 0; // transparent
                continue;
            }
            const double t = std::clamp((v - lo) * scale + 0.5, 0.0, 255.0);
            line[ix] = table[static_cast<std::size_t>(t)];
        }
    }
    return m_rasterCache;
}

void ColorMapPlot::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    paintPlot(painter, QRectF(rect()), true);
}

void ColorMapPlot::paintPlot(QPainter &painter, const QRectF &target, bool interactive) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    Layout local;
    if (!interactive)
        local = computeLayout(target, painter.device());
    const Layout &l = interactive ? widgetLayout() : local;

    painter.fillRect(target, m_theme.surround);
    if (!m_title.isEmpty()) {
        painter.setFont(l.titleFont);
        painter.setPen(m_theme.text);
        painter.drawText(l.titleRect, Qt::AlignCenter, m_title);
    }
    painter.fillRect(l.plotRect, m_theme.background);

    // Grid under the data: it shows where the raster is transparent.
    if (m_gridVisible) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QRectF plot = l.plotRect;
        const auto grid = [&](const AxisTicks &xs, const AxisTicks &ys, bool major) {
            painter.setPen(QPen(major ? m_theme.grid : m_theme.minorGrid, 0));
            for (double v : major ? xs.major : xs.minor) {
                const double px = l.toPixel({v, 0}).x();
                if (px > plot.left() + 0.5 && px < plot.right() - 0.5)
                    painter.drawLine(QPointF(snap(px), plot.top()), QPointF(snap(px), plot.bottom()));
            }
            for (double v : major ? ys.major : ys.minor) {
                const double py = l.toPixel({0, v}).y();
                if (py > plot.top() + 0.5 && py < plot.bottom() - 0.5)
                    painter.drawLine(QPointF(plot.left(), snap(py)), QPointF(plot.right(), snap(py)));
            }
        };
        grid(l.xTicks, l.yTicks, false);
        grid(l.xTicks, l.yTicks, true);
        painter.restore();
    }

    painter.save();
    painter.setClipRect(l.plotRect);
    paintImage(painter, l);
    paintScatter(painter, l);
    paintOverlays(painter, l);
    painter.restore();

    paintAxes(painter, l);
    if (l.showBar)
        paintColorBar(painter, l);
    painter.restore();
}

void ColorMapPlot::paintImage(QPainter &painter, const Layout &l) const
{
    if (!hasImage())
        return;
    const QImage &image = rasterImage();
    if (image.isNull())
        return;
    // Draw only the visible cells, whole cells so they land on their edges;
    // the clip trims the partial ones. Keeps deep zooms cheap.
    const QRectF e = m_image.extent();
    const double vx0 = std::max(l.xMin, e.left());
    const double vx1 = std::min(l.xMax, e.right());
    const double vy0 = std::max(l.yMin, e.top());
    const double vy1 = std::min(l.yMax, e.bottom());
    if (!(vx1 > vx0) || !(vy1 > vy0))
        return;
    const int nx = m_image.nx;
    const int ny = m_image.ny;
    const int ix0 = std::clamp(static_cast<int>(std::floor((vx0 - e.left()) / m_image.dx)), 0, nx - 1);
    const int ix1 = std::clamp(static_cast<int>(std::ceil((vx1 - e.left()) / m_image.dx)), ix0 + 1, nx);
    const int iy0 = std::clamp(static_cast<int>(std::floor((vy0 - e.top()) / m_image.dy)), 0, ny - 1);
    const int iy1 = std::clamp(static_cast<int>(std::ceil((vy1 - e.top()) / m_image.dy)), iy0 + 1, ny);
    const QPointF topLeft = l.toPixel({e.left() + ix0 * m_image.dx, e.top() + iy1 * m_image.dy});
    const QPointF bottomRight = l.toPixel({e.left() + ix1 * m_image.dx, e.top() + iy0 * m_image.dy});
    const QRectF targetRect(topLeft, bottomRight);
    const QRectF source(ix0, ny - iy1, ix1 - ix0, iy1 - iy0);
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.drawImage(targetRect, image, source);
    painter.restore();
}

double ColorMapPlot::scatterDiameter(const Layout &l) const
{
    // Data-sized markers follow the zoom; never vanish below 3 px.
    return m_scatterSize > 0 ? std::max(3.0, m_scatterSize * std::min(l.kx, l.ky)) : kFixedMarkerDiameter;
}

void ColorMapPlot::paintScatter(QPainter &painter, const Layout &l) const
{
    if (m_positions.empty())
        return;
    const auto [lo, hi] = colorRange();
    const double rx = m_scatterSize > 0 ? std::max(1.5, m_scatterSize * l.kx / 2) : kFixedMarkerDiameter / 2;
    const double ry = m_scatterSize > 0 ? std::max(1.5, m_scatterSize * l.ky / 2) : kFixedMarkerDiameter / 2;
    const bool outlined = std::min(rx, ry) >= 2.5;
    QColor edge = m_theme.text;
    edge.setAlphaF(0.45f);
    const QRectF visible = l.plotRect.adjusted(-rx, -ry, rx, ry);
    for (std::size_t i = 0; i < m_positions.size(); ++i) {
        const QPointF p = l.toPixel(m_positions[i]);
        if (!visible.contains(p))
            continue;
        const double v = m_values[i];
        if (std::isnan(v)) {
            painter.setPen(QPen(m_theme.mutedText, 1.2));
            painter.setBrush(Qt::NoBrush);
        } else {
            painter.setPen(outlined ? QPen(edge, 0.8) : QPen(Qt::NoPen));
            painter.setBrush(colorMapColor(m_colorMap, (v - lo) / (hi - lo)));
        }
        painter.drawEllipse(p, rx, ry);
    }
    painter.setBrush(Qt::NoBrush);

    // Crossed-out markers (failed elements): a cross with a halo so it reads
    // over any fill colour.
    const double half = std::max(4.0, std::min(rx, ry) * 0.75);
    QColor halo = m_theme.background;
    halo.setAlphaF(0.85f);
    for (int index : m_crossed) {
        if (index < 0 || index >= static_cast<int>(m_positions.size()))
            continue;
        const QPointF p = l.toPixel(m_positions[static_cast<std::size_t>(index)]);
        if (!visible.contains(p))
            continue;
        const QLineF a(p.x() - half, p.y() - half, p.x() + half, p.y() + half);
        const QLineF b(p.x() - half, p.y() + half, p.x() + half, p.y() - half);
        painter.setPen(QPen(halo, 4.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(a);
        painter.drawLine(b);
        painter.setPen(QPen(m_theme.text, 1.8, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(a);
        painter.drawLine(b);
    }
}

void ColorMapPlot::paintOverlays(QPainter &painter, const Layout &l) const
{
    if (m_overlays.empty())
        return;
    const QFontMetricsF sfm(l.smallFont, painter.device());
    const QRectF inner = l.plotRect.adjusted(2, 2, -2, -2);
    const auto clamped = [](const QPointF &p) { return QPointF(clampPixel(p.x()), clampPixel(p.y())); };
    // A halo that contrasts with the stroke itself, so a white contour or a
    // red circle stays readable over any colour map and on both themes.
    const auto haloFor = [](const QColor &color, double alpha) {
        QColor halo = color.lightness() > 150 ? QColor(0, 0, 0) : QColor(255, 255, 255);
        halo.setAlphaF(static_cast<float>(alpha));
        return halo;
    };
    const auto colorOf = [&](const Overlay &o) { return o.style.color.isValid() ? o.style.color : m_theme.text; };
    constexpr double kMarkerHalf = 6;

    // Shapes first; labels go on top of all of them.
    std::vector<QRectF> markerBoxes;
    for (const Overlay &o : m_overlays) {
        const QColor color = colorOf(o);
        const double width = std::max(0.5, o.style.width);
        // Dashed lines get no halo: a solid halo would read as a solid line.
        const QPen haloPen = o.style.style == Qt::SolidLine
                                 ? QPen(haloFor(color, 0.45), width + 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                                 : QPen(Qt::NoPen);
        const QPen pen(color, width, o.style.style, Qt::RoundCap, Qt::RoundJoin);
        painter.setBrush(Qt::NoBrush);
        switch (o.kind) {
        case Overlay::Kind::Circle: {
            const QPointF c = clamped(l.toPixel(o.points.front()));
            const double rx = std::min(o.radius * l.kx, kPixelLimit);
            const double ry = std::min(o.radius * l.ky, kPixelLimit);
            for (const QPen &p : {haloPen, pen}) {
                painter.setPen(p);
                painter.drawEllipse(c, rx, ry);
            }
            break;
        }
        case Overlay::Kind::Polyline: {
            // A NaN point breaks the line (several contour pieces in one overlay).
            std::vector<QPolygonF> pieces(1);
            for (const QPointF &d : o.points) {
                if (std::isfinite(d.x()) && std::isfinite(d.y()))
                    pieces.back().append(clamped(l.toPixel(d)));
                else if (!pieces.back().isEmpty())
                    pieces.emplace_back();
            }
            for (const QPen &p : {haloPen, pen}) {
                painter.setPen(p);
                for (const QPolygonF &piece : pieces)
                    painter.drawPolyline(piece);
            }
            break;
        }
        case Overlay::Kind::Marker: {
            const QPointF c = clamped(l.toPixel(o.points.front()));
            const QLineF h(c.x() - kMarkerHalf, c.y(), c.x() + kMarkerHalf, c.y());
            const QLineF v(c.x(), c.y() - kMarkerHalf, c.x(), c.y() + kMarkerHalf);
            const double markerWidth = std::max(1.5, width);
            for (const QPen &p : {QPen(haloFor(color, 0.8), markerWidth + 2.5, Qt::SolidLine, Qt::RoundCap),
                                  QPen(color, markerWidth, Qt::SolidLine, Qt::RoundCap)}) {
                painter.setPen(p);
                painter.drawLine(h);
                painter.drawLine(v);
            }
            markerBoxes.emplace_back(c.x() - kMarkerHalf, c.y() - kMarkerHalf, 2 * kMarkerHalf, 2 * kMarkerHalf);
            break;
        }
        }
    }

    // Labels: each takes the first candidate spot that lies in the plot and
    // keeps clear of the marker crosses and the labels already placed; failing
    // that, one that only overlaps markers; else it is left out.
    using Side = std::pair<int, int>; // +1/-1: right/left, above/below
    using Spot = std::pair<QPointF, Side>;
    std::vector<QRectF> labelBoxes;
    const auto labelBox = [&](const QPointF &anchor, const QString &text, const Side &side) {
        const double w = sfm.horizontalAdvance(text);
        const double h = sfm.height();
        const double gap = 4;
        const double x = side.first >= 0 ? anchor.x() + gap : anchor.x() - gap - w;
        const double y = side.second >= 0 ? anchor.y() - gap - h : anchor.y() + gap;
        return QRectF(x, y, w, h);
    };
    const auto hits = [](const std::vector<QRectF> &boxes, const QRectF &box) {
        return std::any_of(boxes.begin(), boxes.end(), [&](const QRectF &b) { return b.intersects(box); });
    };
    const auto place = [&](const Overlay &o, const std::vector<Spot> &spots) {
        std::optional<QRectF> chosen;
        std::optional<QRectF> fallback;
        for (const auto &[anchor, side] : spots) {
            const QRectF box = labelBox(anchor, o.label, side);
            if (!inner.contains(box) || hits(labelBoxes, box))
                continue;
            if (!hits(markerBoxes, box)) {
                chosen = box;
                break;
            }
            if (!fallback)
                fallback = box;
        }
        if (!chosen)
            chosen = fallback;
        if (!chosen)
            return;
        labelBoxes.push_back(chosen->adjusted(-2, -1, 2, 1));
        const QColor color = colorOf(o);
        drawHaloText(painter, l.smallFont, QPointF(chosen->left(), chosen->top() + sfm.ascent()), o.label, color,
                     haloFor(color, 0.85));
    };
    const std::array<Side, 4> sides = {Side{1, 1}, Side{-1, 1}, Side{1, -1}, Side{-1, -1}};
    // Markers first: their labels matter most and belong right next to them.
    for (const Overlay::Kind kind : {Overlay::Kind::Marker, Overlay::Kind::Circle, Overlay::Kind::Polyline}) {
        for (const Overlay &o : m_overlays) {
            if (o.kind != kind || o.label.isEmpty() || o.points.empty())
                continue;
            std::vector<Spot> spots;
            switch (kind) {
            case Overlay::Kind::Marker: {
                const QPointF c = clamped(l.toPixel(o.points.front()));
                for (const Side &side : sides)
                    spots.emplace_back(c, side);
                break;
            }
            case Overlay::Kind::Circle: {
                // Around the circle, starting upper right, the label outside.
                const QPointF c = clamped(l.toPixel(o.points.front()));
                const double rx = std::min(o.radius * l.kx, kPixelLimit);
                const double ry = std::min(o.radius * l.ky, kPixelLimit);
                for (double deg : {45.0, 135.0, -45.0, -135.0, 90.0, -90.0, 0.0, 180.0, 60.0, 120.0, -60.0, -120.0,
                                   30.0, 150.0, -30.0, -150.0}) {
                    const double a = deg * std::numbers::pi / 180.0;
                    const QPointF anchor(c.x() + rx * std::cos(a), c.y() - ry * std::sin(a));
                    spots.emplace_back(anchor, Side{std::cos(a) >= -1e-9 ? 1 : -1, std::sin(a) >= -1e-9 ? 1 : -1});
                }
                break;
            }
            case Overlay::Kind::Polyline: {
                const std::size_t n = o.points.size();
                for (double f : {0.25, 0.75, 0.5, 0.0, 1.0}) {
                    const QPointF d = o.points[std::min(n - 1, static_cast<std::size_t>(f * static_cast<double>(n - 1)))];
                    if (!std::isfinite(d.x()) || !std::isfinite(d.y()))
                        continue;
                    for (const Side &side : sides)
                        spots.emplace_back(l.toPixel(d), side);
                }
                break;
            }
            }
            place(o, spots);
        }
    }
}

void ColorMapPlot::paintAxes(QPainter &painter, const Layout &l) const
{
    const QRectF plot = l.plotRect;
    const QFontMetricsF fm(l.font, painter.device());
    const double tick = l.tick;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(m_theme.axis, 0));
    painter.drawRect(QRectF(snap(plot.left()), snap(plot.top()), std::floor(plot.width()), std::floor(plot.height())));
    painter.setFont(l.font);
    for (double v : l.xTicks.major) {
        const double px = l.toPixel({v, 0}).x();
        if (px < plot.left() - 0.5 || px > plot.right() + 0.5)
            continue;
        painter.setPen(QPen(m_theme.axis, 0));
        painter.drawLine(QPointF(snap(px), plot.bottom()), QPointF(snap(px), plot.bottom() + tick));
        const QString text = formatTick(v, l.xTicks.step, m_xUnit);
        const double w = fm.horizontalAdvance(text);
        painter.setPen(m_theme.mutedText);
        painter.drawText(QRectF(px - w / 2 - 2, plot.bottom() + tick + 1, w + 4, fm.height()), Qt::AlignCenter, text);
    }
    for (double v : l.yTicks.major) {
        const double py = l.toPixel({0, v}).y();
        if (py < plot.top() - 0.5 || py > plot.bottom() + 0.5)
            continue;
        painter.setPen(QPen(m_theme.axis, 0));
        painter.drawLine(QPointF(plot.left(), snap(py)), QPointF(plot.left() - tick, snap(py)));
        const QString text = formatTick(v, l.yTicks.step, m_yUnit);
        const double w = fm.horizontalAdvance(text) + 4;
        painter.setPen(m_theme.mutedText);
        painter.drawText(QRectF(plot.left() - tick - 3 - w, py - fm.height() / 2, w, fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter, text);
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(m_theme.text);
    const QString xTitle = axisTitle(m_xLabel, m_xUnit);
    if (!xTitle.isEmpty()) {
        const double w = std::max(plot.width(), fm.horizontalAdvance(xTitle) + 8);
        painter.drawText(QRectF(plot.center().x() - w / 2, plot.bottom() + tick + fm.height() + 2, w, fm.height()),
                         Qt::AlignCenter, xTitle);
    }
    const QString yTitle = axisTitle(m_yLabel, m_yUnit);
    if (!yTitle.isEmpty()) {
        const double h = std::max(plot.height(), fm.horizontalAdvance(yTitle) + 8);
        painter.translate(plot.left() - l.leftBlock + l.pad, plot.center().y());
        painter.rotate(-90);
        painter.drawText(QRectF(-h / 2, 0, h, fm.height()), Qt::AlignCenter, yTitle);
    }
    painter.restore();
}

void ColorMapPlot::paintColorBar(QPainter &painter, const Layout &l) const
{
    const QRectF bar = l.barRect;
    if (bar.isEmpty())
        return;
    const QFontMetricsF fm(l.font, painter.device());
    const auto table = colorTable(m_colorMap);
    QImage gradient(1, 256, QImage::Format_ARGB32_Premultiplied);
    for (int i = 0; i < 256; ++i)
        gradient.setPixel(0, 255 - i, table[static_cast<std::size_t>(i)]);
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(bar, gradient);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(m_theme.axis, 0));
    painter.drawRect(QRectF(snap(bar.left()), snap(bar.top()), std::floor(bar.width()), std::floor(bar.height())));

    const double tick = l.tick;
    double labelW = 0;
    const double span = l.barMax - l.barMin;
    painter.setFont(l.font);
    for (double v : l.barTicks.major) {
        const double py = bar.bottom() - (v - l.barMin) / span * bar.height();
        if (py < bar.top() - 0.5 || py > bar.bottom() + 0.5)
            continue;
        painter.setPen(QPen(m_theme.axis, 0));
        painter.drawLine(QPointF(bar.right(), snap(py)), QPointF(bar.right() + tick, snap(py)));
        const QString text = formatTick(v, l.barTicks.step, m_barUnit);
        const double w = fm.horizontalAdvance(text) + 4;
        labelW = std::max(labelW, w);
        painter.setPen(m_theme.mutedText);
        painter.drawText(QRectF(bar.right() + tick + 3, py - fm.height() / 2, w, fm.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }
    const QString title = axisTitle(m_barLabel, m_barUnit);
    if (!title.isEmpty()) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(m_theme.text);
        const double x = bar.right() + tick + 3 + std::max(labelW, widestLabel(fm, l.barTicks, m_barUnit) + 4)
                         + l.pad * 0.6 + fm.height();
        const double h = std::max(bar.height(), fm.horizontalAdvance(title) + 8);
        painter.translate(x, bar.center().y());
        painter.rotate(90);
        painter.drawText(QRectF(-h / 2, 0, h, fm.height()), Qt::AlignCenter, title);
    }
    painter.restore();
}

// ---------------------------------------------------------------- read-out

std::optional<double> ColorMapPlot::rasterValueAt(QPointF point) const
{
    if (!hasImage())
        return std::nullopt;
    const QRectF e = m_image.extent();
    if (!(point.x() >= e.left() && point.x() <= e.right() && point.y() >= e.top() && point.y() <= e.bottom()))
        return std::nullopt;
    const int ix = std::clamp(static_cast<int>(std::floor((point.x() - e.left()) / m_image.dx)), 0, m_image.nx - 1);
    const int iy = std::clamp(static_cast<int>(std::floor((point.y() - e.top()) / m_image.dy)), 0, m_image.ny - 1);
    const double v = m_image.values[static_cast<std::size_t>(iy) * static_cast<std::size_t>(m_image.nx)
                                    + static_cast<std::size_t>(ix)];
    if (std::isnan(v))
        return std::nullopt;
    return v;
}

int ColorMapPlot::scatterIndexAt(const Layout &l, const QPointF &pixel) const
{
    if (m_positions.empty())
        return -1;
    // Generous hit area for small markers.
    const double rx = std::max(5.0, m_scatterSize > 0 ? m_scatterSize * l.kx / 2 : kFixedMarkerDiameter / 2 + 2);
    const double ry = std::max(5.0, m_scatterSize > 0 ? m_scatterSize * l.ky / 2 : kFixedMarkerDiameter / 2 + 2);
    int best = -1;
    double bestDistance = 1.0;
    for (std::size_t i = 0; i < m_positions.size(); ++i) {
        const QPointF p = l.toPixel(m_positions[i]);
        const double dx = (p.x() - pixel.x()) / rx;
        const double dy = (p.y() - pixel.y()) / ry;
        const double d = dx * dx + dy * dy;
        if (d <= bestDistance) {
            bestDistance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int ColorMapPlot::scatterIndexAt(const QPointF &pixel) const
{
    return scatterIndexAt(widgetLayout(), pixel);
}

std::optional<double> ColorMapPlot::valueAt(QPointF point) const
{
    const Layout &l = widgetLayout();
    const int index = scatterIndexAt(l, l.toPixel(point));
    if (index >= 0 && std::isfinite(m_values[static_cast<std::size_t>(index)]))
        return m_values[static_cast<std::size_t>(index)];
    return rasterValueAt(point);
}

QString ColorMapPlot::describe(const QPointF &pixel) const
{
    const Layout &l = widgetLayout();
    if (!l.plotRect.contains(pixel))
        return {};
    const int index = scatterIndexAt(l, pixel);
    const QPointF d = index >= 0 ? m_positions[static_cast<std::size_t>(index)] : l.toData(pixel);
    const auto coordinate = [](const QString &label, double v, const QString &unit) {
        const QString value = formatValue(v, unit, 3);
        return label.isEmpty() ? value : label + QLatin1Char(' ') + value;
    };
    QString text = coordinate(m_xLabel, d.x(), m_xUnit) + QStringLiteral(", ") + coordinate(m_yLabel, d.y(), m_yUnit);
    std::optional<double> value;
    if (index >= 0) {
        text += QStringLiteral(" [#%1]").arg(index);
        const double v = m_values[static_cast<std::size_t>(index)];
        if (std::isfinite(v))
            value = v;
    } else {
        value = rasterValueAt(d);
    }
    if (value)
        text += QStringLiteral(": ") + formatValue(*value, m_barUnit, 3);
    return text;
}

void ColorMapPlot::setHoverText(const QString &text)
{
    if (text == m_hoverText)
        return;
    m_hoverText = text;
    Q_EMIT hoverReadoutChanged(text);
}

void ColorMapPlot::updateHover(const QPointF &pixel)
{
    setHoverText(describe(pixel));
}

// ---------------------------------------------------------------- events

void ColorMapPlot::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_moved = false;
        m_pressPos = event->position();
        m_lastPos = m_pressPos;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ColorMapPlot::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    if (m_dragging) {
        if (!m_moved && QLineF(pos, m_pressPos).length() > 3)
            m_moved = true;
        if (m_moved) {
            const Layout &l = widgetLayout();
            const QPointF delta = pos - m_lastPos;
            m_lastPos = pos;
            setView(m_view.translated(-delta.x() / l.kx, delta.y() / l.ky), false);
            setCursor(Qt::ClosedHandCursor);
        }
    }
    updateHover(pos);
    QWidget::mouseMoveEvent(event);
}

void ColorMapPlot::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        unsetCursor();
        const QPointF pos = event->position();
        if (!m_moved && plotArea().contains(pos))
            Q_EMIT pointClicked(pixelToData(pos), scatterIndexAt(pos));
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ColorMapPlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        autoscale();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void ColorMapPlot::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps != 0 && plotArea().contains(event->position())) {
        zoomAt(event->position(), std::pow(0.8, steps));
        updateHover(event->position());
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void ColorMapPlot::leaveEvent(QEvent *event)
{
    setHoverText({});
    QWidget::leaveEvent(event);
}

void ColorMapPlot::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(m_autoscaleAction);
    menu.addSeparator();
    menu.addMenu(m_colorMapMenu);
    menu.addSeparator();
    menu.addAction(m_copyAction);
    menu.addAction(m_exportPngAction);
    menu.addAction(m_exportCsvAction);
    menu.exec(event->globalPos());
}

void ColorMapPlot::resizeEvent(QResizeEvent *event)
{
    m_layout.reset();
    QWidget::resizeEvent(event);
}

void ColorMapPlot::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) {
        updateTheme();
        invalidate();
    }
    QWidget::changeEvent(event);
}

// ---------------------------------------------------------------- export

QImage ColorMapPlot::renderImage(const QSize &size, qreal devicePixelRatio) const
{
    const QSize logical = size.isValid() && !size.isEmpty() ? size : this->size();
    if (logical.isEmpty() || !(devicePixelRatio > 0))
        return {};
    QImage image(logical * devicePixelRatio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(m_theme.surround);
    QPainter painter(&image);
    paintPlot(painter, QRectF(QPointF(0, 0), QSizeF(logical)), false);
    return image;
}

void ColorMapPlot::render(QPainter *painter, const QRectF &target) const
{
    paintPlot(*painter, target, false);
}

bool ColorMapPlot::exportImage(const QString &path, const QSize &size) const
{
    const QImage image = renderImage(size);
    return !image.isNull() && image.save(path);
}

QString ColorMapPlot::csvText() const
{
    QString text;
    QTextStream out(&text);
    const QString header = csvField(csvHeader(m_xLabel, m_xUnit, QStringLiteral("x"))) + QLatin1Char(',')
                           + csvField(csvHeader(m_yLabel, m_yUnit, QStringLiteral("y"))) + QLatin1Char(',')
                           + csvField(csvHeader(m_barLabel, m_barUnit, tr("Value")));
    if (hasImage()) {
        out << header << '\n';
        for (int iy = 0; iy < m_image.ny; ++iy) {
            const double y = m_image.y0 + iy * m_image.dy;
            for (int ix = 0; ix < m_image.nx; ++ix) {
                const double x = m_image.x0 + ix * m_image.dx;
                const double v = m_image.values[static_cast<std::size_t>(iy) * static_cast<std::size_t>(m_image.nx)
                                                + static_cast<std::size_t>(ix)];
                out << csvNumber(x) << ',' << csvNumber(y) << ',' << csvNumber(v) << '\n';
            }
        }
    } else if (!m_positions.empty()) {
        out << header << '\n';
        for (std::size_t i = 0; i < m_positions.size(); ++i)
            out << csvNumber(m_positions[i].x()) << ',' << csvNumber(m_positions[i].y()) << ','
                << csvNumber(m_values[i]) << '\n';
    }
    out.flush();
    return text;
}

bool ColorMapPlot::exportCsv(const QString &path) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(csvText().toUtf8());
    return file.commit();
}

void ColorMapPlot::copyImageToClipboard() const
{
    if (QClipboard *clipboard = QApplication::clipboard())
        clipboard->setImage(renderImage(size(), 2.0).convertToFormat(QImage::Format_ARGB32));
}

} // namespace lvd

// Layout and painting of PlotWidget, shared by the widget and the exports.

#include "plot/PlotMath.h"
#include "plot/PlotWidget.h"
#include "plot/PlotWidgetPrivate.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lvd {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kPixelLimit = 1e5;

double clampPixel(double p)
{
    return std::isfinite(p) ? std::clamp(p, -kPixelLimit, kPixelLimit) : p;
}

QString axisTitle(const QString &label, const QString &unit)
{
    if (unit.isEmpty() || unitTakesPrefix(unit))
        return label;
    if (label.isEmpty())
        return displayUnit(unit);
    return QStringLiteral("%1 (%2)").arg(label, displayUnit(unit));
}

AxisTicks ticksFor(const AxisScale &scale, int count)
{
    return scale.isLog() ? logTicks(scale.min(), scale.max(), count) : linearTicks(scale.min(), scale.max(), count);
}

double widestLabel(const QFontMetricsF &fm, const AxisTicks &ticks, const QString &unit)
{
    double w = 0;
    for (double v : ticks.major)
        w = std::max(w, fm.horizontalAdvance(formatTick(v, ticks.step, unit)));
    return w;
}

// Crisp one-pixel lines: snap to pixel centres.
double snap(double p)
{
    return std::floor(p) + 0.5;
}

QString reactanceLabel(double x)
{
    const QString magnitude = formatTick(std::abs(x), 0, {});
    if (x < 0)
        return QString(QChar(0x2212)) + QStringLiteral("j") + magnitude;
    return QStringLiteral("j") + magnitude;
}

// Long polylines with wide antialiased pens are slow to rasterise as one
// path; short overlapping pieces draw the same line much faster.
void drawPolyline(QPainter &painter, const QPolygonF &polygon)
{
    constexpr qsizetype kChunk = 16;
    const qsizetype n = polygon.size();
    if (n <= kChunk) {
        painter.drawPolyline(polygon);
        return;
    }
    for (qsizetype start = 0; start < n - 1; start += kChunk) {
        const qsizetype count = std::min(kChunk + 1, n - start);
        painter.drawPolyline(polygon.constData() + start, static_cast<int>(count));
    }
}

// Fills the area covered by a dense trace, in short overlapping pieces (the
// rasteriser is much faster on small polygons; the overlap avoids seams).
void fillEnvelope(QPainter &painter, const std::vector<EnvelopeColumn> &columns, double half)
{
    constexpr std::size_t kChunk = 8;
    const std::size_t n = columns.size();
    QPolygonF polygon;
    for (std::size_t start = 0; start < n; start += kChunk) {
        const std::size_t stop = std::min(n, start + kChunk + 1);
        const double left = columns[start].x - (start == 0 ? half : 0.0);
        const double right = columns[stop - 1].x + (stop == n ? half : 0.0);
        polygon.clear();
        polygon.append(QPointF(left, columns[start].min - half));
        for (std::size_t i = start; i < stop; ++i)
            polygon.append(QPointF(columns[i].x, columns[i].min - half));
        polygon.append(QPointF(right, columns[stop - 1].min - half));
        polygon.append(QPointF(right, columns[stop - 1].max + half));
        for (std::size_t i = stop; i-- > start;)
            polygon.append(QPointF(columns[i].x, columns[i].max + half));
        polygon.append(QPointF(left, columns[start].max + half));
        painter.drawPolygon(polygon);
    }
}

} // namespace

PlotWidget::Layout PlotWidget::computeLayout(const QRectF &target, QPaintDevice *device, bool interactive) const
{
    Layout l;
    l.target = target;
    l.interactive = interactive;
    l.font = font();
    l.titleFont = l.font;
    l.titleFont.setBold(true);
    l.titleFont.setPointSizeF(l.font.pointSizeF() * 1.2);
    l.smallFont = l.font;
    l.smallFont.setPointSizeF(l.font.pointSizeF() * 0.9);
    const QFontMetricsF fm(l.font, device);
    const QFontMetricsF tfm(l.titleFont, device);
    const double pad = std::round(fm.height() * 0.6);

    double top = target.top() + pad * 0.6;
    if (!m_title.isEmpty()) {
        l.titleRect = QRectF(target.left() + pad, top, target.width() - 2 * pad, tfm.height());
        top += tfm.height() + pad * 0.3;
    }
    if (m_legendVisible && !m_traces.empty()) {
        const double sample = fm.height() * 1.6;
        const double rowHeight = fm.height() + 2;
        const double left = target.left() + pad;
        const double right = target.right() - pad;
        double x = left;
        double y = top;
        for (const auto &t : m_traces) {
            const double w = sample + 6 + fm.horizontalAdvance(t->name);
            if (x > left && x + w > right) {
                x = left;
                y += rowHeight;
            }
            l.legendItems.emplace_back(t->id, QRectF(x, y, w, rowHeight));
            x += w + fm.height() * 1.2;
        }
        // Centre each row.
        double rowStart = 0;
        for (std::size_t i = 0; i < l.legendItems.size();) {
            const double rowY = l.legendItems[i].second.top();
            std::size_t j = i;
            double rowRight = 0;
            while (j < l.legendItems.size() && l.legendItems[j].second.top() == rowY)
                rowRight = l.legendItems[j++].second.right();
            rowStart = l.legendItems[i].second.left();
            const double shift = std::floor(((right - left) - (rowRight - rowStart)) / 2);
            for (std::size_t k = i; k < j; ++k)
                l.legendItems[k].second.translate(shift, 0);
            i = j;
        }
        l.legendRect = QRectF(left, top, right - left, y + rowHeight - top);
        top = y + rowHeight + pad * 0.4;
    }

    if (m_kind == PlotKind::Rectangular) {
        l.x = m_x.scale;
        l.yLeft = m_yLeft.scale;
        l.yRight = m_yRight.scale;
        l.rightAxis = hasRightAxis();
        const double plotTop = top + fm.height() * 0.5;
        const double plotBottom = target.bottom() - pad * 0.6 - fm.height() * 2 - pad * 0.6;
        const double plotHeight = std::max(10.0, plotBottom - plotTop);
        const int yCount = std::max(2, static_cast<int>(plotHeight / (fm.height() * 2.8)));
        l.yLeftTicks = ticksFor(l.yLeft, yCount);
        const double leftMargin = pad + fm.height() + pad * 0.5 + widestLabel(fm, l.yLeftTicks, m_yLeft.unit) + pad * 0.5;
        double rightMargin = pad + fm.averageCharWidth() * 3;
        if (l.rightAxis) {
            l.yRightTicks = ticksFor(l.yRight, yCount);
            rightMargin = pad + fm.height() + pad * 0.5 + widestLabel(fm, l.yRightTicks, m_yRight.unit) + pad * 0.5;
        }
        l.plotRect = QRectF(target.left() + leftMargin, plotTop,
                            std::max(10.0, target.width() - leftMargin - rightMargin), plotHeight);
        int xCount = std::max(2, static_cast<int>(l.plotRect.width() / (fm.averageCharWidth() * 14)));
        for (int attempt = 0; attempt < 8; ++attempt) {
            l.xTicks = ticksFor(l.x, xCount);
            const double need = static_cast<double>(l.xTicks.major.size())
                                * (widestLabel(fm, l.xTicks, m_x.unit) + fm.averageCharWidth() * 3);
            if (need <= l.plotRect.width() || xCount <= 2)
                break;
            xCount = std::max(2, static_cast<int>(xCount * 0.7));
        }
        l.x.setPixels(l.plotRect.left(), l.plotRect.right());
        l.yLeft.setPixels(l.plotRect.bottom(), l.plotRect.top());
        l.yRight.setPixels(l.plotRect.bottom(), l.plotRect.top());
    } else {
        // Keep the marker table off the chart: reserve a band above it.
        if (m_markerTableVisible && !m_markers.empty()) {
            const QFontMetricsF sfm(l.smallFont, device);
            const double height = (sfm.height() + 1) * static_cast<double>(m_markers.size()) + 8;
            l.tableRect = QRectF(target.left() + pad, top, target.width() - 2 * pad, height);
            top += height + pad * 0.3;
        }
        l.plotRect = QRectF(target.left() + pad, top, target.width() - 2 * pad, target.bottom() - pad * 0.6 - top);
        const double margin = fm.height() * 1.7;
        const double radius = std::max(10.0, std::min(l.plotRect.width(), l.plotRect.height()) / 2 - margin);
        const double half = m_viewHalfSize > 0 ? m_viewHalfSize : 1.0;
        l.scale = radius / half;
        l.centre = l.plotRect.center();
        l.viewCentre = m_viewCentre;
    }
    return l;
}

void PlotWidget::paintPlot(QPainter &painter, const QRectF &target, bool interactive) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    Layout local;
    if (!interactive)
        local = computeLayout(target, painter.device(), false);
    const Layout &l = interactive ? widgetLayout() : local;

    painter.fillRect(target, m_theme.surround);
    if (!m_title.isEmpty()) {
        painter.setFont(l.titleFont);
        painter.setPen(m_theme.text);
        painter.drawText(l.titleRect, Qt::AlignCenter, m_title);
    }
    switch (m_kind) {
    case PlotKind::Rectangular:
        paintRectangular(painter, l);
        break;
    case PlotKind::Smith:
        paintSmithGrid(painter, l);
        paintComplexTraces(painter, l);
        break;
    case PlotKind::Polar:
        paintPolarGrid(painter, l);
        paintComplexTraces(painter, l);
        break;
    }
    paintMarkers(painter, l);
    paintLegend(painter, l);
    if (m_markerTableVisible)
        paintMarkerTable(painter, l);
    if (interactive)
        paintHover(painter, l);
    painter.restore();
}

void PlotWidget::paintRectangular(QPainter &painter, const Layout &l) const
{
    const QRectF plot = l.plotRect;
    painter.fillRect(plot, m_theme.background);
    const QFontMetricsF fm(l.font, painter.device());

    // Grid.
    painter.setRenderHint(QPainter::Antialiasing, false);
    if (m_gridVisible) {
        const auto vertical = [&](const std::vector<double> &values, const QColor &color) {
            painter.setPen(QPen(color, 0));
            for (double v : values) {
                const double px = l.x.toPixel(v);
                if (px >= plot.left() && px <= plot.right())
                    painter.drawLine(QPointF(snap(px), plot.top()), QPointF(snap(px), plot.bottom()));
            }
        };
        const auto horizontal = [&](const AxisScale &s, const std::vector<double> &values, const QColor &color) {
            painter.setPen(QPen(color, 0));
            for (double v : values) {
                const double py = s.toPixel(v);
                if (py >= plot.top() && py <= plot.bottom())
                    painter.drawLine(QPointF(plot.left(), snap(py)), QPointF(plot.right(), snap(py)));
            }
        };
        vertical(l.xTicks.minor, m_theme.minorGrid);
        horizontal(l.yLeft, l.yLeftTicks.minor, m_theme.minorGrid);
        vertical(l.xTicks.major, m_theme.grid);
        horizontal(l.yLeft, l.yLeftTicks.major, m_theme.grid);
    }

    // Frame and ticks.
    painter.setPen(QPen(m_theme.axis, 0));
    painter.drawRect(QRectF(snap(plot.left()), snap(plot.top()), std::floor(plot.width()), std::floor(plot.height())));
    const double tick = fm.height() * 0.3;
    painter.setFont(l.font);
    for (double v : l.xTicks.major) {
        const double px = l.x.toPixel(v);
        if (px < plot.left() - 0.5 || px > plot.right() + 0.5)
            continue;
        painter.setPen(QPen(m_theme.axis, 0));
        painter.drawLine(QPointF(snap(px), plot.bottom()), QPointF(snap(px), plot.bottom() + tick));
        const QString text = formatTick(v, l.xTicks.step, m_x.unit);
        const double w = fm.horizontalAdvance(text);
        painter.setPen(m_theme.mutedText);
        painter.drawText(QRectF(px - w / 2 - 2, plot.bottom() + tick + 1, w + 4, fm.height()), Qt::AlignCenter, text);
    }
    const auto yLabels = [&](const AxisScale &s, const AxisTicks &ticks, const QString &unit, bool left) {
        const double x0 = left ? plot.left() : plot.right();
        for (double v : ticks.major) {
            const double py = s.toPixel(v);
            if (py < plot.top() - 0.5 || py > plot.bottom() + 0.5)
                continue;
            painter.setPen(QPen(m_theme.axis, 0));
            painter.drawLine(QPointF(x0, snap(py)), QPointF(left ? x0 - tick : x0 + tick, snap(py)));
            const QString text = formatTick(v, ticks.step, unit);
            painter.setPen(m_theme.mutedText);
            const double w = fm.horizontalAdvance(text) + 4;
            const QRectF r = left ? QRectF(x0 - tick - 3 - w, py - fm.height() / 2, w, fm.height())
                                  : QRectF(x0 + tick + 3, py - fm.height() / 2, w, fm.height());
            painter.drawText(r, (left ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, text);
        }
    };
    yLabels(l.yLeft, l.yLeftTicks, m_yLeft.unit, true);
    if (l.rightAxis)
        yLabels(l.yRight, l.yRightTicks, m_yRight.unit, false);

    // Axis titles.
    painter.setPen(m_theme.text);
    const QString xTitle = axisTitle(m_x.label, m_x.unit);
    painter.drawText(QRectF(plot.left(), plot.bottom() + tick + fm.height() + 2, plot.width(), fm.height()),
                     Qt::AlignCenter, xTitle);
    const auto verticalTitle = [&](const QString &text, double x, bool left) {
        painter.save();
        painter.translate(x, plot.center().y());
        painter.rotate(left ? -90 : 90);
        painter.drawText(QRectF(-plot.height() / 2, 0, plot.height(), fm.height()), Qt::AlignCenter, text);
        painter.restore();
    };
    const double pad = std::round(fm.height() * 0.6);
    verticalTitle(axisTitle(m_yLeft.label, m_yLeft.unit), l.target.left() + pad, true);
    if (l.rightAxis)
        verticalTitle(axisTitle(m_yRight.label, m_yRight.unit), l.target.right() - pad, false);

    // Traces.
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.save();
    painter.setClipRect(plot.adjusted(-1, -1, 1, 1));
    const double columns = plot.width();
    std::vector<QPolygonF> segments;
    std::vector<std::vector<EnvelopeColumn>> envelope;
    for (const auto &t : m_traces) {
        if (!t->visible || t->size() == 0)
            continue;
        const AxisScale &ys = t->style.yAxis == PlotYAxis::Left ? l.yLeft : l.yRight;
        const std::size_t n = t->size();
        std::size_t begin = 0;
        std::size_t end = n;
        if (t->xAscending && n > 2) {
            const double lo = std::min(l.x.min(), l.x.max());
            const double hi = std::max(l.x.min(), l.x.max());
            const auto first = std::lower_bound(t->x.begin(), t->x.end(), lo);
            const auto last = std::upper_bound(t->x.begin(), t->x.end(), hi);
            begin = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, (first - t->x.begin()) - 1));
            end = std::min(n, static_cast<std::size_t>(last - t->x.begin()) + 1);
        }
        const AxisScale::Mapper xm = l.x.mapper();
        const AxisScale::Mapper ym = ys.mapper();
        const auto mapX = [xm](double v) { return clampPixel(xm(v)); };
        const auto mapY = [ym](double v) { return clampPixel(ym(v)); };
        segments.clear();
        const QColor color = traceColor(t->id);
        const bool decimated = t->xAscending && static_cast<double>(end - begin) > 2 * columns;
        if (decimated && t->style.lineStyle == Qt::SolidLine) {
            // Dense solid trace: fill the per-column extent as one polygon,
            // much faster than stroking thousands of steep segments.
            envelope.clear();
            decimateEnvelope(t->x.data(), t->y.data(), begin, end, mapX, mapY, envelope);
            const double half = t->style.lineWidth / 2;
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            for (const auto &all : envelope)
                fillEnvelope(painter, all, half);
            painter.setBrush(Qt::NoBrush);
            continue;
        }
        if (decimated) {
            decimateMinMax(t->x.data(), t->y.data(), begin, end, mapX, mapY, segments);
        } else {
            QPolygonF current;
            current.reserve(static_cast<qsizetype>(end - begin));
            for (std::size_t i = begin; i < end; ++i) {
                const double px = mapX(t->x[i]);
                const double py = mapY(t->y[i]);
                if (!std::isfinite(px) || !std::isfinite(py)) {
                    if (!current.isEmpty())
                        segments.push_back(std::move(current));
                    current = QPolygonF();
                    continue;
                }
                current.append(QPointF(px, py));
            }
            if (!current.isEmpty())
                segments.push_back(std::move(current));
        }
        if (t->style.lineStyle != Qt::NoPen) {
            painter.setPen(QPen(color, t->style.lineWidth, t->style.lineStyle, decimated ? Qt::FlatCap : Qt::RoundCap,
                                decimated ? Qt::BevelJoin : Qt::RoundJoin));
            for (const QPolygonF &segment : segments)
                drawPolyline(painter, segment);
        }
        const bool fewPoints = static_cast<double>(end - begin) <= columns / 3;
        if (t->style.showPoints && fewPoints) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            const double r = std::max(2.0, t->style.lineWidth * 1.4);
            for (const QPolygonF &segment : segments)
                for (const QPointF &p : segment)
                    painter.drawEllipse(p, r, r);
            painter.setBrush(Qt::NoBrush);
        }
    }
    painter.restore();
}

void PlotWidget::paintSmithGrid(QPainter &painter, const Layout &l) const
{
    const QFontMetricsF sfm(l.smallFont, painter.device());
    const QPointF c = l.gammaToPixel(0.0);
    const double R = l.scale;
    QPainterPath unit;
    unit.addEllipse(c, R, R);

    painter.save();
    painter.setClipRect(l.plotRect);
    painter.fillPath(unit, m_theme.background);

    // Denser grid when the chart is large on screen.
    std::vector<double> values = {0.2, 0.5, 1, 2, 5};
    if (R > 380)
        values = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.8, 1, 1.5, 2, 3, 4, 5, 10, 20};
    else if (R > 220)
        values = {0.2, 0.4, 0.6, 1, 1.5, 2, 3, 5, 10};
    else if (R < 90)
        values = {0.5, 1, 2};

    const auto drawGrid = [&](double sign, const QColor &color, double width) {
        painter.setPen(QPen(color, width));
        painter.setBrush(Qt::NoBrush);
        for (double r : values) {
            const QPointF centre = l.gammaToPixel(sign * r / (1 + r));
            painter.drawEllipse(centre, R / (1 + r), R / (1 + r));
        }
        for (double x : values) {
            for (double s : {1.0, -1.0}) {
                const QPointF centre = l.gammaToPixel(sign * std::complex<double>(1.0, s / x));
                painter.drawEllipse(centre, R / x, R / x);
            }
        }
        painter.drawLine(l.gammaToPixel(-1.0), l.gammaToPixel(1.0));
    };
    if (m_gridVisible) {
        painter.save();
        painter.setClipPath(unit, Qt::IntersectClip);
        if (m_admittanceGrid)
            drawGrid(-1.0, m_theme.admittanceGrid, 1.0);
        drawGrid(1.0, m_theme.smithGrid, 1.0);
        painter.restore();
    }
    painter.setPen(QPen(m_theme.axis, 1.5));
    painter.drawEllipse(c, R, R);

    // Labels: resistance along the real axis, reactance around the rim.
    painter.setFont(l.smallFont);
    painter.setPen(m_theme.mutedText);
    if (m_gridVisible) {
        for (double r : values) {
            const QPointF p = l.gammaToPixel((r - 1) / (r + 1));
            const QString text = formatTick(r, 0, {});
            const double w = sfm.horizontalAdvance(text);
            painter.drawText(QRectF(p.x() - w - 3, p.y() - sfm.height() - 1, w + 2, sfm.height()),
                             Qt::AlignRight | Qt::AlignBottom, text);
        }
        for (double x : values) {
            for (double s : {1.0, -1.0}) {
                const std::complex<double> jx(0.0, s * x);
                const std::complex<double> g = (jx - 1.0) / (jx + 1.0);
                const std::complex<double> u = g / std::abs(g);
                const QPointF rim = l.gammaToPixel(g);
                const QPointF p(rim.x() + u.real() * sfm.height() * 0.85, rim.y() - u.imag() * sfm.height() * 0.85);
                const QString text = reactanceLabel(s * x);
                const double w = sfm.horizontalAdvance(text);
                painter.drawText(QRectF(p.x() - w / 2 - 1, p.y() - sfm.height() / 2, w + 2, sfm.height()),
                                 Qt::AlignCenter, text);
            }
        }
        const auto endLabel = [&](std::complex<double> g, const QString &text, double dx) {
            const QPointF p = l.gammaToPixel(g);
            const double w = sfm.horizontalAdvance(text);
            painter.drawText(QRectF(p.x() + dx - w / 2 - 1, p.y() - sfm.height() / 2, w + 2, sfm.height()),
                             Qt::AlignCenter, text);
        };
        endLabel(-1.0, QStringLiteral("0"), -sfm.height() * 0.7);
        endLabel(1.0, QString(QChar(0x221E)), sfm.height() * 0.7);
    }

    // Caller circles (stability, gain, noise).
    for (const auto &[id, circle] : m_circles) {
        Q_UNUSED(id);
        const QColor color = circle.color.isValid() ? circle.color : m_theme.auxiliary;
        painter.save();
        if (circle.clipToUnitCircle)
            painter.setClipPath(unit, Qt::IntersectClip);
        painter.setPen(QPen(color, circle.lineWidth, circle.lineStyle));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(l.gammaToPixel(circle.centre), circle.radius * R, circle.radius * R);
        painter.restore();
        if (circle.name.isEmpty())
            continue;
        // Name at the top of the circle, or else at its point nearest to the
        // chart centre (circles mostly outside the chart).
        const double distance = std::abs(circle.centre);
        std::complex<double> at = circle.centre + std::complex<double>(0, circle.radius);
        if (circle.clipToUnitCircle && std::abs(at) > 0.97)
            at = distance > 1e-9 ? circle.centre - circle.radius * circle.centre / distance : circle.centre + circle.radius;
        if (circle.clipToUnitCircle && std::abs(at) > 0.97)
            continue;
        const QPointF p = l.gammaToPixel(at);
        const double w = sfm.horizontalAdvance(circle.name);
        const QRectF box(p.x() - w / 2 - 3, p.y() - sfm.height() / 2, w + 6, sfm.height());
        painter.setPen(Qt::NoPen);
        QColor fill = m_theme.background;
        fill.setAlpha(210);
        painter.setBrush(fill);
        painter.drawRoundedRect(box, 2, 2);
        painter.setPen(m_theme.text);
        painter.drawText(box, Qt::AlignCenter, circle.name);
        painter.setBrush(Qt::NoBrush);
    }
    painter.restore();
}

void PlotWidget::paintPolarGrid(QPainter &painter, const Layout &l) const
{
    const QFontMetricsF sfm(l.smallFont, painter.device());
    const QPointF c = l.gammaToPixel(0.0);
    const double R = l.scale;
    painter.save();
    painter.setClipRect(l.plotRect);
    QPainterPath outer;
    outer.addEllipse(c, R, R);
    painter.fillPath(outer, m_theme.background);

    const double lo = polarMinimum();
    const double hi = polarMaximum();
    const AxisTicks ticks = linearTicks(lo, hi, std::max(3, std::min(6, static_cast<int>(R / 45))));
    const bool db = m_polarScale == PolarScale::Decibel;
    const auto displayAngle = [&](double deg) {
        return m_polarOrientation == PolarOrientation::Compass ? 90.0 - deg : deg;
    };
    const auto at = [&](double radius, double deg) {
        return l.gammaToPixel(std::polar(radius, displayAngle(deg) * kDegToRad));
    };
    if (m_gridVisible) {
        painter.setPen(QPen(m_theme.grid, 1.0));
        for (double v : ticks.major) {
            const double r = polarRadius(v);
            if (r > 1e-6 && r < 1 - 1e-6)
                painter.drawEllipse(c, r * R, r * R);
        }
        for (int deg = 0; deg < 360; deg += 30)
            painter.drawLine(c, at(1.0, deg));
    }
    painter.setPen(QPen(m_theme.axis, 1.5));
    painter.drawEllipse(c, R, R);

    painter.setFont(l.smallFont);
    painter.setPen(m_theme.mutedText);
    for (int deg = 0; deg < 360; deg += 30) {
        const QPointF p = at(1.0 + sfm.height() * 0.95 / R, deg);
        const QString text = formatValue(deg, QStringLiteral("deg"), 3);
        const double w = sfm.horizontalAdvance(text);
        painter.drawText(QRectF(p.x() - w / 2 - 1, p.y() - sfm.height() / 2, w + 2, sfm.height()), Qt::AlignCenter, text);
    }
    // Radial values along a direction between two spokes.
    const double labelAngle = m_polarOrientation == PolarOrientation::Compass ? 165.0 : 75.0;
    for (double v : ticks.major) {
        const double r = polarRadius(v);
        if (r > 1 + 1e-6 || r < 1e-6)
            continue;
        QString text = formatTick(v, ticks.step, {});
        if (db)
            text += QStringLiteral(" dB");
        const QPointF p = at(r, labelAngle);
        const double w = sfm.horizontalAdvance(text);
        const QRectF box(p.x() - w / 2 - 2, p.y() - sfm.height() / 2, w + 4, sfm.height());
        QColor fill = m_theme.background;
        fill.setAlpha(200);
        painter.fillRect(box, fill);
        painter.drawText(box, Qt::AlignCenter, text);
    }
    painter.restore();
}

void PlotWidget::paintComplexTraces(QPainter &painter, const Layout &l) const
{
    painter.save();
    painter.setClipRect(l.plotRect);
    std::vector<QPolygonF> segments;
    for (const auto &t : m_traces) {
        if (!t->visible || t->size() == 0)
            continue;
        if (m_kind == PlotKind::Smith && !t->isComplex)
            continue;
        segments.clear();
        QPolygonF current;
        QPointF last;
        const std::size_t n = t->size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::complex<double> g = m_kind == PlotKind::Smith ? t->z[i]
                                                                     : polarPoint(polarAngle(*t, i), polarValue(*t, i));
            const QPointF p = l.gammaToPixel(g);
            if (!std::isfinite(p.x()) || !std::isfinite(p.y())) {
                if (!current.isEmpty())
                    segments.push_back(std::move(current));
                current = QPolygonF();
                continue;
            }
            const QPointF q(clampPixel(p.x()), clampPixel(p.y()));
            // Skip points closer than a fraction of a pixel to the previous one.
            if (!current.isEmpty() && i + 1 < n && std::abs(q.x() - last.x()) + std::abs(q.y() - last.y()) < 0.75)
                continue;
            current.append(q);
            last = q;
        }
        if (!current.isEmpty())
            segments.push_back(std::move(current));
        const QColor color = traceColor(t->id);
        if (t->style.lineStyle != Qt::NoPen) {
            painter.setPen(QPen(color, t->style.lineWidth, t->style.lineStyle, Qt::RoundCap, Qt::RoundJoin));
            for (const QPolygonF &segment : segments)
                drawPolyline(painter, segment);
        }
        if (t->style.showPoints && n <= 2000) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            const double r = std::max(2.0, t->style.lineWidth * 1.4);
            for (const QPolygonF &segment : segments)
                for (const QPointF &p : segment)
                    painter.drawEllipse(p, r, r);
            painter.setBrush(Qt::NoBrush);
        }
    }
    painter.restore();
}

std::optional<QPointF> PlotWidget::markerPixel(const Layout &l, const Marker &marker) const
{
    const Trace *t = findTrace(marker.traceId);
    if (!t || !t->visible)
        return std::nullopt;
    if (m_kind == PlotKind::Rectangular) {
        const auto v = sample(*t, marker.x);
        if (!v)
            return std::nullopt;
        const AxisScale &ys = t->style.yAxis == PlotYAxis::Left ? l.yLeft : l.yRight;
        const QPointF p(l.x.toPixel(marker.x), ys.toPixel(v->y));
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
            return std::nullopt;
        return p;
    }
    const auto g = planePoint(*t, marker.x);
    if (!g)
        return std::nullopt;
    return l.gammaToPixel(*g);
}

void PlotWidget::paintMarkers(QPainter &painter, const Layout &l) const
{
    if (m_markers.empty())
        return;
    QFont font = l.smallFont;
    font.setBold(true);
    const QFontMetricsF fm(font, painter.device());
    painter.save();
    painter.setClipRect(l.plotRect.adjusted(-2, -2, 2, 2));
    painter.setFont(font);
    for (const Marker &m : m_markers) {
        const auto p = markerPixel(l, m);
        if (!p || !l.plotRect.adjusted(-1, -1, 1, 1).contains(*p))
            continue;
        const QColor color = traceColor(m.traceId);
        const bool flip = p->y() - 14 - fm.height() < l.plotRect.top();
        const double dir = flip ? 1.0 : -1.0;
        QPolygonF triangle;
        triangle << *p << QPointF(p->x() - 5.5, p->y() + dir * 11) << QPointF(p->x() + 5.5, p->y() + dir * 11);
        const bool reference = m.id == m_referenceMarker;
        painter.setPen(QPen(reference ? color : m_theme.background, 1.5));
        painter.setBrush(reference ? m_theme.background : color);
        painter.drawPolygon(triangle);
        painter.setBrush(Qt::NoBrush);
        const QString text = QStringLiteral("M%1").arg(m.number) + (reference ? tr(" (ref)") : QString());
        const double w = fm.horizontalAdvance(text);
        const double y = flip ? p->y() + 12 : p->y() - 12 - fm.height();
        painter.setPen(m_theme.text);
        painter.drawText(QRectF(p->x() - w / 2 - 2, y, w + 4, fm.height()), Qt::AlignCenter, text);
    }
    painter.restore();
}

void PlotWidget::paintLegend(QPainter &painter, const Layout &l) const
{
    if (l.legendItems.empty())
        return;
    const QFontMetricsF fm(l.font, painter.device());
    painter.save();
    painter.setFont(l.font);
    for (const auto &[id, r] : l.legendItems) {
        const Trace *t = findTrace(id);
        if (!t)
            continue;
        QColor color = traceColor(id);
        if (!t->visible)
            color.setAlpha(70);
        const double sample = fm.height() * 1.6;
        const double y = r.center().y();
        if (t->style.lineStyle != Qt::NoPen) {
            painter.setPen(QPen(color, std::max(2.0, t->style.lineWidth), t->style.lineStyle, Qt::FlatCap));
            painter.drawLine(QPointF(r.left(), y), QPointF(r.left() + sample, y));
        }
        if (t->style.showPoints || t->style.lineStyle == Qt::NoPen) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(QPointF(r.left() + sample / 2, y), 3, 3);
            painter.setBrush(Qt::NoBrush);
        }
        painter.setPen(t->visible ? m_theme.text : m_theme.mutedText);
        QFont f = l.font;
        f.setStrikeOut(!t->visible);
        painter.setFont(f);
        painter.drawText(QRectF(r.left() + sample + 6, r.top(), r.width() - sample - 6, r.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, t->name);
    }
    painter.restore();
}

void PlotWidget::paintMarkerTable(QPainter &painter, const Layout &l) const
{
    if (m_markers.empty())
        return;
    const QStringList lines = markerTable();
    const QFontMetricsF fm(l.smallFont, painter.device());
    std::vector<QStringList> rows;
    std::vector<double> widths;
    for (const QString &line : lines) {
        rows.push_back(line.split(QLatin1Char('\t')));
        const QStringList &cells = rows.back();
        if (widths.size() < static_cast<std::size_t>(cells.size()))
            widths.resize(static_cast<std::size_t>(cells.size()), 0);
        for (qsizetype i = 0; i < cells.size(); ++i)
            widths[static_cast<std::size_t>(i)] = std::max(widths[static_cast<std::size_t>(i)], fm.horizontalAdvance(cells[i]));
    }
    const double gap = fm.averageCharWidth() * 2;
    const double swatch = fm.height() * 0.6;
    double width = swatch + gap * 0.5;
    for (double w : widths)
        width += w + gap;
    width += 4;
    const double rowHeight = fm.height() + 1;
    const double height = rowHeight * static_cast<double>(rows.size()) + 8;
    const QRectF box = l.tableRect.isValid() ? QRectF(l.tableRect.right() - width, l.tableRect.top(), width, height)
                                             : QRectF(l.plotRect.right() - width - 8, l.plotRect.top() + 8, width, height);

    painter.save();
    painter.setPen(QPen(m_theme.grid, 1));
    painter.setBrush(m_theme.tableBackground);
    painter.drawRoundedRect(box, 3, 3);
    painter.setFont(l.smallFont);
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const double y = box.top() + 4 + rowHeight * static_cast<double>(r);
        const QColor color = traceColor(m_markers[r].traceId);
        painter.fillRect(QRectF(box.left() + 6, y + (rowHeight - swatch) / 2, swatch, swatch), color);
        double x = box.left() + 6 + swatch + gap * 0.5;
        painter.setPen(m_theme.text);
        for (qsizetype i = 0; i < rows[r].size(); ++i) {
            const double w = widths[static_cast<std::size_t>(i)];
            // Numbers right-aligned, names left-aligned.
            const Qt::Alignment align = i < 2 ? Qt::AlignLeft : Qt::AlignRight;
            painter.drawText(QRectF(x, y, w, rowHeight), align | Qt::AlignVCenter, rows[r][i]);
            x += w + gap;
        }
    }
    painter.restore();
}

void PlotWidget::paintHover(QPainter &painter, const Layout &l) const
{
    if (m_drag == Drag::RubberBand) {
        QColor fill = m_theme.rubberBand;
        fill.setAlpha(40);
        painter.setPen(QPen(m_theme.rubberBand, 1));
        painter.setBrush(fill);
        painter.drawRect(QRectF(m_pressPos, m_hoverPos).normalized().intersected(l.plotRect));
        painter.setBrush(Qt::NoBrush);
        return;
    }
    if (!m_hovering || !m_crosshairEnabled || !m_hover.valid || m_drag != Drag::None
        || !l.plotRect.contains(m_hoverPos))
        return;
    painter.save();
    painter.setClipRect(l.plotRect);
    std::optional<QPointF> point;
    if (const Trace *t = m_hover.traceId >= 0 ? findTrace(m_hover.traceId) : nullptr) {
        if (m_kind == PlotKind::Rectangular) {
            const AxisScale &ys = t->style.yAxis == PlotYAxis::Left ? l.yLeft : l.yRight;
            point = QPointF(l.x.toPixel(m_hover.x), ys.toPixel(m_hover.y));
        } else if (const auto g = planePoint(*t, m_hover.x)) {
            point = l.gammaToPixel(*g);
        }
    }
    painter.setPen(QPen(m_theme.crosshair, 1, Qt::DashLine));
    if (m_kind == PlotKind::Rectangular) {
        painter.drawLine(QPointF(m_hoverPos.x(), l.plotRect.top()), QPointF(m_hoverPos.x(), l.plotRect.bottom()));
        if (point)
            painter.drawLine(QPointF(l.plotRect.left(), point->y()), QPointF(l.plotRect.right(), point->y()));
    }
    if (point) {
        painter.setPen(QPen(traceColor(m_hover.traceId), 2));
        painter.setBrush(m_theme.background);
        painter.drawEllipse(*point, 4.5, 4.5);
        painter.setBrush(Qt::NoBrush);
    }
    // Read-out box beside the cursor.
    const QFontMetricsF fm(l.smallFont, painter.device());
    const QString text = m_hover.text;
    const double w = fm.horizontalAdvance(text) + 12;
    const double h = fm.height() + 6;
    QPointF topLeft = m_hoverPos + QPointF(14, 16);
    if (topLeft.x() + w > l.plotRect.right())
        topLeft.setX(m_hoverPos.x() - 14 - w);
    if (topLeft.y() + h > l.plotRect.bottom())
        topLeft.setY(m_hoverPos.y() - 16 - h);
    topLeft.setX(std::max(topLeft.x(), l.plotRect.left() + 2));
    const QRectF box(topLeft, QSizeF(w, h));
    painter.setPen(QPen(m_theme.grid, 1));
    painter.setBrush(m_theme.tableBackground);
    painter.drawRoundedRect(box, 3, 3);
    painter.setFont(l.smallFont);
    painter.setPen(m_theme.text);
    painter.drawText(box, Qt::AlignCenter, text);
    painter.restore();
}

} // namespace lvd

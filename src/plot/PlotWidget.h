#pragma once

#include "plot/PlotAxis.h"
#include "plot/PlotTheme.h"

#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cmath>
#include <complex>
#include <memory>
#include <optional>
#include <span>
#include <vector>

class QAction;
class QPainter;

namespace lvd {

enum class PlotKind { Rectangular, Smith, Polar };
enum class PlotYAxis { Left, Right };

// What a rectangular plot shows of a trace holding complex values.
enum class ComplexPart { MagnitudeDb, Magnitude, PhaseDeg, Real, Imaginary };

// Radial scale of a polar plot.
enum class PolarScale { Linear, Decibel };
// Mathematical: 0 deg to the right, counter-clockwise. Compass: 0 deg at the
// top, clockwise (antenna patterns).
enum class PolarOrientation { Mathematical, Compass };

struct TraceStyle {
    QColor color;                          // invalid: next palette colour
    Qt::PenStyle lineStyle = Qt::SolidLine; // Qt::NoPen draws points only
    double lineWidth = 2.0;
    bool showPoints = false;               // a dot at every data point
    PlotYAxis yAxis = PlotYAxis::Left;     // rectangular plots
};

// A circle in the Gamma plane drawn by the caller on a Smith chart
// (stability, constant gain or noise circles).
struct PlotCircle {
    QString name;
    std::complex<double> centre;
    double radius = 0;
    QColor color; // invalid: the theme's auxiliary colour
    Qt::PenStyle lineStyle = Qt::DashLine;
    double lineWidth = 1.5;
    bool clipToUnitCircle = true;
};

struct PlotMarkerInfo {
    int id = -1;
    int traceId = -1;
    QString name;              // "M1", "M2", ...
    double x = 0;              // frequency or x value (polar: angle for real traces)
    double y = 0;              // value shown on a rectangular plot, in yUnit
    QString xUnit;
    QString yUnit;
    bool isComplex = false;
    std::complex<double> value; // complex traces: interpolated value (Gamma for S11)
    bool reference = false;
    bool valid = false;        // false when x is outside the trace data
};

// The value under the mouse, for a status bar.
struct PlotReadout {
    bool valid = false;
    int traceId = -1; // -1: a position, not a trace value
    double x = 0;
    double y = 0;
    std::complex<double> value;
    QString text;
};

// Plot widget for RF results: rectangular (linear or log axes, two y axes),
// Smith chart and polar plots, with markers, legend, zoom and export.
// Drawing is done with QPainter; data are plain vectors.
class PlotWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PlotWidget(QWidget *parent = nullptr);
    ~PlotWidget() override;

    // ---- General
    void setKind(PlotKind kind);
    PlotKind kind() const { return m_kind; }
    void setTitle(const QString &title);
    QString title() const { return m_title; }
    void setGridVisible(bool visible);
    bool isGridVisible() const { return m_gridVisible; }
    void setLegendVisible(bool visible);
    bool isLegendVisible() const { return m_legendVisible; }
    void setMarkerTableVisible(bool visible);
    bool isMarkerTableVisible() const { return m_markerTableVisible; }
    void setCrosshairEnabled(bool enabled);
    bool isCrosshairEnabled() const { return m_crosshairEnabled; }

    // ---- Rectangular axes. The unit selects the tick formatting: "Hz", "s",
    // "V", ... take SI prefixes on the ticks; others ("dB") go in the title.
    void setXAxis(const QString &label, const QString &unit, bool log = false);
    void setYAxis(const QString &label, const QString &unit, bool log = false, PlotYAxis side = PlotYAxis::Left);
    void setXRange(double min, double max); // turns x autoscale off
    void setYRange(double min, double max, PlotYAxis side = PlotYAxis::Left);
    AxisScale xScale() const { return m_x.scale; }
    AxisScale yScale(PlotYAxis side = PlotYAxis::Left) const;
    bool hasRightAxis() const;

    // ---- Traces
    int addTrace(const QString &name, const QString &unit = {}, const TraceStyle &style = {});
    // Real data (rectangular; polar: x is the angle in degrees and y the radius
    // value in the polar scale's unit). Copies; cheap enough for live tuning.
    void setTraceData(int traceId, std::span<const double> x, std::span<const double> y);
    // Complex data (Smith, polar, and rectangular through ComplexPart);
    // x is kept for read-out (frequency).
    void setTraceData(int traceId, std::span<const double> x, std::span<const std::complex<double>> values);
    void setTraceComplexPart(int traceId, ComplexPart part);
    void setTraceName(int traceId, const QString &name);
    void setTraceStyle(int traceId, const TraceStyle &style);
    TraceStyle traceStyle(int traceId) const;
    QColor traceColor(int traceId) const; // resolved for the current theme
    void setTraceVisible(int traceId, bool visible);
    bool isTraceVisible(int traceId) const;
    QString traceName(int traceId) const;
    std::vector<int> traceIds() const;
    void removeTrace(int traceId);
    void clear(); // traces, markers and circles

    // ---- Smith chart
    void setSmithZ0(double z0);
    double smithZ0() const { return m_z0; }
    void setAdmittanceGridVisible(bool visible);
    bool isAdmittanceGridVisible() const { return m_admittanceGrid; }
    int addCircle(const PlotCircle &circle);
    void removeCircle(int circleId);
    void clearCircles();

    // ---- Polar
    void setPolarScale(PolarScale scale, double floorDb = -40.0);
    PolarScale polarScale() const { return m_polarScale; }
    void setPolarOrientation(PolarOrientation orientation);
    // Outer radius value; NaN returns to autoscale.
    void setPolarMaximum(double maximum);
    double polarMinimum() const;
    double polarMaximum() const;

    // ---- Markers
    int addMarker(int traceId, double x);
    void setMarkerX(int markerId, double x);
    void removeMarker(int markerId);
    void clearMarkers();
    std::vector<PlotMarkerInfo> markers() const;
    std::optional<PlotMarkerInfo> marker(int markerId) const;
    void setReferenceMarker(int markerId); // -1: none; others then show deltas
    int referenceMarker() const { return m_referenceMarker; }
    // x of the largest or smallest shown value of a trace (within [xMin, xMax]).
    std::optional<double> findPeak(int traceId, bool maximum, double xMin = -INFINITY, double xMax = INFINITY) const;
    int addPeakMarker(int traceId, bool maximum); // -1 when the trace is empty
    // One line per marker, tab separated, as shown in the marker table.
    QStringList markerTable() const;

    // ---- View
    void autoscale();
    bool isAutoscaled() const;
    // Zoom about a widget position (factor < 1 zooms in).
    void zoomAt(const QPointF &position, double factor);

    // ---- Coordinate mapping (widget pixels), for tests and overlays
    QRectF plotArea() const;
    QPointF dataToPixel(double x, double y, PlotYAxis side = PlotYAxis::Left) const;
    QPointF pixelToData(const QPointF &pixel, PlotYAxis side = PlotYAxis::Left) const;
    QPointF gammaToPixel(std::complex<double> gamma) const; // Smith and polar (normalised radius)
    std::complex<double> pixelToGamma(const QPointF &pixel) const;
    QRectF legendItemRect(int traceId) const;

    // ---- Export
    QImage renderImage(const QSize &size, qreal devicePixelRatio = 1.0) const;
    void render(QPainter *painter, const QRectF &target) const;
    bool exportImage(const QString &path, const QSize &size = {}) const;
    static bool svgSupported();
    bool exportSvg(const QString &path, const QSize &size = {}) const;
    QString csvText() const;
    bool exportCsv(const QString &path) const;
    void copyImageToClipboard() const;

    PlotReadout hoverReadout() const { return m_hover; }
    const PlotTheme &theme() const { return m_theme; }

    // Actions for a toolbar or menu (also in the context menu).
    QAction *autoscaleAction() const { return m_autoscaleAction; }
    QList<QAction *> plotActions() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void markersChanged();
    void hoverReadoutChanged(const QString &text);
    void traceVisibilityChanged(int traceId, bool visible);
    void viewChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    struct Trace;
    struct Marker {
        int id = -1;
        int traceId = -1;
        int number = 0;
        double x = 0;
    };
    struct Axis {
        QString label;
        QString unit;
        AxisScale scale;
        bool autoRange = true;
    };
    struct Layout;
    struct SampledValue;

    Trace *findTrace(int traceId);
    const Trace *findTrace(int traceId) const;
    void traceDataChanged(Trace &trace);
    int nextColorSlot() const;
    void updateTheme();
    void createActions();
    void invalidate(); // geometry changed: drop the cached layout and repaint
    void refreshAutoRanges();
    static QString shownUnit(const Trace &trace);

    // Geometry, in the coordinates of the target being painted.
    Layout computeLayout(const QRectF &target, QPaintDevice *device, bool interactive) const;
    const Layout &widgetLayout() const;

    // Painting.
    void paintPlot(QPainter &painter, const QRectF &target, bool interactive) const;
    void paintRectangular(QPainter &painter, const Layout &layout) const;
    void paintSmithGrid(QPainter &painter, const Layout &layout) const;
    void paintPolarGrid(QPainter &painter, const Layout &layout) const;
    void paintComplexTraces(QPainter &painter, const Layout &layout) const;
    void paintMarkers(QPainter &painter, const Layout &layout) const;
    void paintLegend(QPainter &painter, const Layout &layout) const;
    void paintMarkerTable(QPainter &painter, const Layout &layout) const;
    void paintHover(QPainter &painter, const Layout &layout) const;

    // Values.
    std::optional<SampledValue> sample(const Trace &trace, double x) const;
    double polarRadius(double value) const;
    double polarValue(const Trace &trace, std::size_t i) const;
    double polarValueOf(std::complex<double> z) const;
    double polarAngle(const Trace &trace, std::size_t i) const;
    std::complex<double> polarPoint(double angleDeg, double value) const;
    // Point of a trace at x in the plot plane (Gamma or normalised polar).
    std::optional<std::complex<double>> planePoint(const Trace &trace, double x) const;
    std::optional<QPointF> markerPixel(const Layout &layout, const Marker &marker) const;
    QString markerLine(const Marker &marker) const;
    PlotMarkerInfo markerInfo(const Marker &marker) const;
    QString describe(const Trace &trace, double x) const;
    QString describeGamma(std::complex<double> gamma) const;

    // Interaction.
    int markerAt(const QPointF &position) const;
    int legendItemAt(const QPointF &position) const;
    std::optional<double> nearestX(const Trace &trace, const QPointF &position, double *distance = nullptr) const;
    void updateHover(const QPointF &position);
    void setHover(const PlotReadout &readout);
    void zoomToRect(const QRectF &rect);
    void zoomAxes(const QPointF &position, double factor, bool zoomX, bool zoomY);
    PlotKind m_kind = PlotKind::Rectangular;
    QString m_title;
    bool m_gridVisible = true;
    bool m_legendVisible = true;
    bool m_markerTableVisible = true;
    bool m_crosshairEnabled = true;

    Axis m_x;
    Axis m_yLeft;
    Axis m_yRight;

    std::vector<std::unique_ptr<Trace>> m_traces;
    std::vector<Marker> m_markers;
    std::vector<std::pair<int, PlotCircle>> m_circles;
    int m_nextTraceId = 1;
    int m_nextMarkerId = 1;
    int m_nextMarkerNumber = 1;
    int m_nextCircleId = 1;
    int m_referenceMarker = -1;

    double m_z0 = 50.0;
    bool m_admittanceGrid = false;
    PolarScale m_polarScale = PolarScale::Linear;
    double m_polarFloorDb = -40.0;
    double m_polarMax = NAN; // NaN: autoscale
    double m_polarAutoMax = 1.0;
    PolarOrientation m_polarOrientation = PolarOrientation::Mathematical;

    // Smith and polar view: centre and half-extent in the Gamma plane.
    std::complex<double> m_viewCentre = 0.0;
    double m_viewHalfSize = 0; // 0: the default view

    PlotTheme m_theme;

    // Interaction state.
    enum class Drag { None, RubberBand, Pan, Marker };
    Drag m_drag = Drag::None;
    QPointF m_pressPos;
    QPointF m_lastPos;
    int m_dragMarker = -1;
    QPointF m_hoverPos;
    bool m_hovering = false;
    PlotReadout m_hover;

    mutable std::unique_ptr<Layout> m_layout; // widget layout, rebuilt on demand

    QAction *m_autoscaleAction = nullptr;
    QAction *m_gridAction = nullptr;
    QAction *m_admittanceAction = nullptr;
    QAction *m_exportPngAction = nullptr;
    QAction *m_exportSvgAction = nullptr;
    QAction *m_exportCsvAction = nullptr;
    QAction *m_copyAction = nullptr;
};

} // namespace lvd

#pragma once

#include "plot/ColorMaps.h"
#include "plot/PlotAxis.h"
#include "plot/PlotTheme.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QAction;
class QActionGroup;
class QMenu;
class QPainter;

namespace lvd {

// Pen of an overlay shape drawn over a colour-map plot.
struct ColorMapOverlayStyle {
    QColor color; // invalid: the theme's text colour
    Qt::PenStyle style = Qt::SolidLine;
    double width = 1.5;
};

// 2D colour-map plot: a raster image of values (UV-space patterns, theta/phi
// maps) and/or a scatter of markers filled by value (element layouts), with
// a colour bar, overlays in data coordinates (circles, polylines, labelled
// markers), zoom, pan, read-out and export. Drawn with QPainter.
class ColorMapPlot : public QWidget
{
    Q_OBJECT
public:
    explicit ColorMapPlot(QWidget *parent = nullptr);
    ~ColorMapPlot() override;

    // ---- General
    void setTitle(const QString &title);
    QString title() const { return m_title; }
    // The unit selects the tick formatting as in PlotWidget: "m" takes SI
    // prefixes on the ticks; others ("deg") go in the axis title; empty for u, v.
    void setXAxis(const QString &label, const QString &unit = {});
    void setYAxis(const QString &label, const QString &unit = {});
    // Equal data scale on x and y (UV maps, element layouts): the view range
    // is widened on one axis to fill the plot area.
    void setAspectLocked(bool locked);
    bool isAspectLocked() const { return m_aspectLocked; }
    void setGridVisible(bool visible);
    bool isGridVisible() const { return m_gridVisible; }
    // Bilinear smoothing of the raster when it is magnified (off: cells).
    void setSmoothImage(bool smooth);
    bool isSmoothImage() const { return m_smooth; }

    // ---- Colour scale
    void setColorMap(ColorMap map);
    ColorMap colorMap() const { return m_colorMap; }
    void setColorBar(const QString &label, const QString &unit); // "Gain", "dBi"
    void setColorBarVisible(bool visible);
    bool isColorBarVisible() const { return m_colorBarVisible; }
    void setColorRange(double min, double max);
    void setColorAutoRange(); // finite data min..max of the image and scatter
    bool isColorAutoRange() const { return m_colorAuto; }
    std::pair<double, double> colorRange() const; // the range in use

    // ---- Raster: ny rows of nx columns, row-major (values[iy * nx + ix]).
    // Cell centres span x0..x1 and y0..y1 (either may be descending). NaN
    // cells are transparent (outside the visible region of a UV map).
    void setImage(int nx, int ny, double x0, double x1, double y0, double y1, std::vector<double> values);
    void clearImage();
    bool hasImage() const { return m_image.nx > 0; }

    // ---- Scatter of markers filled by value (NaN: hollow marker).
    // sizeData: marker diameter in data units (0: a fixed 8 px).
    void setScatter(std::vector<QPointF> positions, std::vector<double> values, double sizeData = 0);
    void setScatterValues(std::vector<double> values); // same positions, new values
    void setScatterCrossed(std::vector<int> indices);  // drawn crossed out (failed elements)
    std::vector<int> scatterCrossed() const { return m_crossed; }
    void clearScatter();
    int scatterCount() const { return static_cast<int>(m_positions.size()); }

    // ---- Overlays in data coordinates; the ids stay valid until removed.
    int addCircle(QPointF centre, double radius, const ColorMapOverlayStyle &style = {}, const QString &label = {});
    int addPolyline(std::vector<QPointF> points, const ColorMapOverlayStyle &style = {}, const QString &label = {});
    int addMarker(QPointF at, const QString &label, const ColorMapOverlayStyle &style = {}); // a plus with text
    void removeOverlay(int id);
    void clearOverlays();
    std::vector<int> overlayIds() const;

    // ---- View. Zoom with the wheel, pan by dragging, double-click autoscales.
    void setRange(double x0, double x1, double y0, double y1);
    void autoscale(); // the image and scatter extent (overlays only when there is no data)
    bool isAutoscaled() const { return m_autoView; }
    QRectF viewRange() const; // the range shown (aspect lock applied), in data units
    void zoomAt(const QPointF &position, double factor); // widget pixels, factor < 1 zooms in

    // ---- Read-out
    // Value of the scatter marker under the point, else the raster value of
    // the nearest cell; nothing outside the data or on NaN.
    std::optional<double> valueAt(QPointF point) const;
    // Index of the scatter marker at a widget position, -1 when none.
    int scatterIndexAt(const QPointF &pixel) const;
    QPointF dataToPixel(QPointF point) const;
    QPointF pixelToData(QPointF pixel) const;
    QRectF plotArea() const;
    QRectF colorBarArea() const;
    QString hoverText() const { return m_hoverText; }

    // ---- Export
    QImage renderImage(const QSize &size, qreal devicePixelRatio = 1.0) const;
    void render(QPainter *painter, const QRectF &target) const;
    bool exportImage(const QString &path, const QSize &size = {}) const;
    // Raster as x,y,value rows (iy outer, ix inner); without a raster, the
    // scatter as x,y,value rows. Empty cells give an empty value field.
    QString csvText() const;
    bool exportCsv(const QString &path) const;
    void copyImageToClipboard() const;

    const PlotTheme &theme() const { return m_theme; }

    // Autoscale, export PNG and CSV, copy image and the colour-map submenu,
    // for a toolbar or menu (also in the context menu).
    QList<QAction *> plotActions() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void hoverReadoutChanged(const QString &text); // "u 0.31, v -0.12: 18.4 dBi"
    // A click (no drag) in the plot area; scatterIndex is -1 off the markers.
    void pointClicked(QPointF point, int scatterIndex);
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
    struct Raster {
        int nx = 0;
        int ny = 0;
        double x0 = 0, x1 = 0, y0 = 0, y1 = 0; // cell centres, ascending
        double dx = 1, dy = 1;                 // cell size
        std::vector<double> values;
        QRectF extent() const; // cell edges
    };
    struct Overlay {
        enum class Kind { Circle, Polyline, Marker };
        int id = 0;
        Kind kind = Kind::Circle;
        std::vector<QPointF> points; // circle: the centre
        double radius = 0;
        ColorMapOverlayStyle style;
        QString label;
    };
    struct Layout;

    void updateTheme();
    void createActions();
    void invalidate();     // geometry changed: drop the cached layout and repaint
    void dataChanged();    // values changed: rebuild the image and colour range
    void refreshColorRange();
    QRectF dataBounds() const;
    QRectF effectiveRange(const QRectF &plotRect) const;
    void setView(const QRectF &range, bool automatic);

    Layout computeLayout(const QRectF &target, QPaintDevice *device) const;
    const Layout &widgetLayout() const;
    void paintPlot(QPainter &painter, const QRectF &target, bool interactive) const;
    void paintImage(QPainter &painter, const Layout &l) const;
    void paintScatter(QPainter &painter, const Layout &l) const;
    void paintOverlays(QPainter &painter, const Layout &l) const;
    void paintAxes(QPainter &painter, const Layout &l) const;
    void paintColorBar(QPainter &painter, const Layout &l) const;
    const QImage &rasterImage() const;
    double scatterDiameter(const Layout &l) const;
    int scatterIndexAt(const Layout &l, const QPointF &pixel) const;
    std::optional<double> rasterValueAt(QPointF point) const;
    QString describe(const QPointF &pixel) const;
    void updateHover(const QPointF &pixel);
    void setHoverText(const QString &text);

    QString m_title;
    QString m_xLabel = QStringLiteral("x");
    QString m_xUnit;
    QString m_yLabel = QStringLiteral("y");
    QString m_yUnit;
    QString m_barLabel;
    QString m_barUnit;
    bool m_aspectLocked = false;
    bool m_gridVisible = true;
    bool m_smooth = false;
    bool m_colorBarVisible = true;

    ColorMap m_colorMap = ColorMap::Viridis;
    bool m_colorAuto = true;
    double m_colorMin = 0;
    double m_colorMax = 1;

    Raster m_image;
    std::vector<QPointF> m_positions;
    std::vector<double> m_values;
    std::vector<int> m_crossed;
    double m_scatterSize = 0;

    std::vector<Overlay> m_overlays;
    int m_nextOverlayId = 1;

    QRectF m_view = QRectF(-1, -1, 2, 2); // requested range (x, y, width, height)
    bool m_autoView = true;

    PlotTheme m_theme;

    // Interaction state.
    bool m_dragging = false;
    bool m_moved = false;
    QPointF m_pressPos;
    QPointF m_lastPos;
    QString m_hoverText;

    mutable std::unique_ptr<Layout> m_layout;
    mutable QImage m_rasterCache; // nx x ny, rebuilt when the data or colours change
    mutable bool m_rasterDirty = true;

    QAction *m_autoscaleAction = nullptr;
    QAction *m_exportPngAction = nullptr;
    QAction *m_exportCsvAction = nullptr;
    QAction *m_copyAction = nullptr;
    QMenu *m_colorMapMenu = nullptr;
    QActionGroup *m_colorMapGroup = nullptr;
};

} // namespace lvd

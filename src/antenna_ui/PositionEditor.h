#pragma once

#include "antenna/ArrayGeometry.h"

#include <QDialog>
#include <QWidget>

#include <set>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;

namespace lvd::antui {

// Plane of the element canvas: the horizontal and vertical axes.
enum class LayoutPlane { XY, XZ, YZ };

// Interactive drawing of element positions: drag to move the selection,
// rubber band to select, double-click empty space to add, wheel to zoom,
// middle or right drag to pan, Home to fit, Delete to delete. Values are in
// display units (wavelengths or millimetres); the editor owns the elements.
class LayoutCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit LayoutCanvas(QWidget *parent = nullptr);

    void setElements(std::vector<ant::CustomElement> *elements, double displayScale, const QString &unit);
    void setPlane(LayoutPlane plane);
    LayoutPlane plane() const { return m_plane; }
    void setGrid(double step, bool snap);
    double snapValue(double v) const;

    const std::set<int> &selection() const { return m_selection; }
    void setSelection(const std::set<int> &selection);
    void fitView();
    void refresh(); // after the element list changed

    // Plane coordinates (display units) of an element, and widget pixels.
    QPointF planePoint(const ant::CustomElement &e) const;
    void setPlanePoint(ant::CustomElement &e, QPointF p) const;
    QPointF toPixel(QPointF plane) const;
    QPointF toPlane(QPointF pixel) const;
    QPointF elementPixel(int index) const;
    int elementAt(QPointF pixel) const;

    QSize sizeHint() const override { return {520, 420}; }

Q_SIGNALS:
    void selectionChanged();
    void elementsMoved();               // a drag ended
    void addRequested(QPointF planePoint);
    void deleteRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    double markerRadius() const;

    std::vector<ant::CustomElement> *m_elements = nullptr;
    double m_scale = 1;        // display units per spec unit
    QString m_unit;
    LayoutPlane m_plane = LayoutPlane::XY;
    double m_grid = 0.5;
    bool m_snap = true;
    QPointF m_centre;          // view centre, display units
    double m_pixels = 50;      // pixels per display unit
    bool m_fitted = false;
    std::set<int> m_selection;

    enum class Drag { None, Move, Band, Pan };
    Drag m_drag = Drag::None;
    QPointF m_press, m_last;
    int m_anchor = -1;
    QPointF m_anchorStart;
    std::vector<QPointF> m_startPoints;
    bool m_moved = false;
};

// Dialog that edits the element positions of an array (any geometry is
// converted to a custom element table first). Has its own undo.
class PositionEditor : public QDialog
{
    Q_OBJECT
public:
    PositionEditor(const ant::GeometrySpec &geometry, double designFrequency, QWidget *parent = nullptr);

    const std::vector<ant::CustomElement> &elements() const { return m_elements; }
    LayoutCanvas *canvas() const { return m_canvas; }
    QTableWidget *table() const { return m_table; }

    // Editing operations (also used by the buttons and tests). Selections
    // are element indices.
    void addElement(QPointF planePoint);
    void deleteSelected();
    void duplicateSelected();
    void moveSelected(double dx, double dy, double dz);       // display units
    void rotateSelected(double degrees);                      // about the axis normal to the view plane
    void mirrorSelected(bool horizontal);                     // in the view plane, about the selection centre
    void toggleSelected();                                    // switch on/off
    bool importCsv(const QString &path, QString *error = nullptr);
    bool exportCsv(const QString &path, QString *error = nullptr) const;
    void undo();
    void redo();

private:
    void pushState();
    void refreshTable();
    void refreshStatus();
    void readRow(int row);
    void syncSelectionFromTable();
    void syncSelectionToTable();
    QString unitLabel() const;

    std::vector<ant::CustomElement> m_elements;
    double m_scale = 1;          // display units per spec unit (1000 for metres -> mm)
    QString m_specUnit;
    std::vector<std::vector<ant::CustomElement>> m_history;
    std::size_t m_historyIndex = 0;
    bool m_updating = false;

    LayoutCanvas *m_canvas;
    QTableWidget *m_table;
    QComboBox *m_plane;
    QDoubleSpinBox *m_grid;
    QCheckBox *m_snap;
    QLabel *m_status;
};

} // namespace lvd::antui

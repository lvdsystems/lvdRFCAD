#pragma once

#include "core/ComponentDefinition.h"

#include <QGraphicsView>
#include <QList>

#include <optional>

namespace lvd {

// An element of a symbol that can be selected on the canvas.
struct ElementRef {
    enum class Kind { Graphic, Pin, Reference, Value };
    Kind kind = Kind::Graphic;
    int index = 0; // graphic or symbol pin index

    bool operator==(const ElementRef &) const = default;
};

// Drawing area of the symbol editor. Holds a copy of the definition being
// edited and reports every change through definitionEdited(); the editor
// records it for undo and hands the result back with setDefinition().
// Coordinates are symbol units: 10-unit grid, Y down, origin at the centre.
class SymbolCanvas : public QGraphicsView
{
    Q_OBJECT
public:
    enum class Tool { Select, Line, Polygon, Rect, Circle, Arc, Text, Pin };

    explicit SymbolCanvas(QWidget *parent = nullptr);

    // Keeps the selection where it still refers to existing elements.
    void setDefinition(const ComponentDefinition &definition);
    const ComponentDefinition &definition() const { return m_definition; }

    Tool tool() const { return m_tool; }
    void setTool(Tool tool);

    // Snap grid for graphics: 1, 5 or 10 units. Pins always snap to 10.
    int graphicsGrid() const { return m_graphicsGrid; }
    void setGraphicsGrid(int grid);

    QList<ElementRef> selection() const { return m_selection; }
    void setSelection(const QList<ElementRef> &selection);
    void selectAll();
    void deleteSelection();
    void duplicateSelection();
    void moveSelection(QPoint delta);

    void zoomToFit();
    double zoom() const;

    // Viewport position of a symbol point, and the reverse; for tests and status display.
    QPoint mapFromSymbol(QPointF point) const;
    QPointF mapToSymbol(QPoint viewportPosition) const;

    // A free pin name: "p1", "p2", ... or the next number when all pins are numbered.
    static QString newPinName(const ComponentDefinition &definition);
    // Rectangle covered by one element, in symbol coordinates.
    static QRectF elementBounds(const ComponentDefinition &definition, const ElementRef &element);

Q_SIGNALS:
    void definitionEdited(const lvd::ComponentDefinition &definition, const QString &description);
    void selectionChanged();
    void toolChanged(lvd::SymbolCanvas::Tool tool);
    void elementCreated(const lvd::ElementRef &element);
    void cursorMoved(QPointF symbolPosition);
    void zoomChanged(double pixelsPerUnit);

protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void drawForeground(QPainter *painter, const QRectF &rect) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class Drag { None, Move, Handle, Band, Pan, Create };

    std::optional<ElementRef> hitTest(QPointF point) const;
    int hitHandle(QPointF point) const;
    QList<QPointF> handles() const;
    double tolerance() const; // a few pixels, in symbol units
    QPoint snap(QPointF point, int grid) const;
    bool selectionHasPins() const;

    void commit(const ComponentDefinition &definition, const QString &description);
    void selectOnly(const ElementRef &element);
    void pruneSelection();
    void cancelCreation();
    void addCreationPoint(QPoint point);
    void finishCreation();
    std::optional<SymbolGraphic> creationPreview() const;
    void createPin(QPoint at);

    ComponentDefinition m_definition;
    Tool m_tool = Tool::Select;
    int m_graphicsGrid = 5;
    QList<ElementRef> m_selection;

    Drag m_drag = Drag::None;
    QPointF m_pressPoint;           // symbol coordinates
    QPoint m_lastPanPosition;       // viewport coordinates
    ComponentDefinition m_dragStart;
    int m_handle = -1;
    QRectF m_band;
    bool m_moved = false;

    QList<QPoint> m_points;         // points of the shape being created
    QPointF m_cursor;               // symbol coordinates, not snapped
    bool m_fitPending = true; // fit the symbol on resize until the user zooms or pans
};

} // namespace lvd

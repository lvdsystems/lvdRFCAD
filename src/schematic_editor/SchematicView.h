#pragma once

#include "core/Schematic.h"

#include <QGraphicsView>
#include <QHash>
#include <QSet>

#include <functional>
#include <memory>
#include <optional>

class QGraphicsRectItem;

namespace lvd {

class ComponentItem;
class SchematicItem;
class SchematicScene;
class WireItem;

// A value shown on the drawing after a simulation (requirement SP-6), at a
// sheet point: a node voltage on a wire, a current or power at a part.
struct SchematicAnnotation {
    enum class Kind { Voltage, Current, Power };
    QPointF at;
    QString text; // may hold several lines
    Kind kind = Kind::Voltage;
};

// The canvas: navigation, tools and the editing operations behind the
// editor's actions. Every change goes through the Schematic API, one undo
// step per user action, with junctions fixed up in the same step.
class SchematicView : public QGraphicsView
{
    Q_OBJECT
public:
    enum class Tool { Select, Wire, Label, Text, Highlight };

    // Asks the user for a text; returns nullopt when cancelled. Replaceable
    // for tests and scripting.
    using TextPrompt = std::function<std::optional<QString>(const QString &title, const QString &label,
                                                            const QString &initial, const QStringList &suggestions,
                                                            bool multiLine)>;

    explicit SchematicView(SchematicScene *scene, QWidget *parent = nullptr);
    ~SchematicView() override;

    SchematicScene *schematicScene() const { return m_scene; }
    Schematic *schematic() const;

    Tool tool() const { return m_tool; }
    void setTool(Tool tool);

    // Placement mode: a ghost follows the cursor, each click places a part.
    bool startPlacing(const QString &definitionKey);
    bool isPlacing() const { return !m_placingKey.isEmpty(); }
    QString placingKey() const { return m_placingKey; }
    bool isFloating() const { return m_floating != nullptr; }
    bool isDrawingWire() const { return !m_wirePoints.isEmpty(); }

    // Leaves the innermost mode: floating items, placement, a wire being
    // drawn, then the current tool; finally clears selection and highlight.
    void cancel();
    // Ends interactions that would conflict with undo or redo.
    void cancelTransient();

    void deleteSelection();
    void rotateSelection();
    void mirrorSelection();
    void copySelection();
    void cutSelection();
    void paste();
    void duplicateSelection();
    void selectAll();
    void nudge(QPoint delta);
    bool hasSelection() const;

    void zoomToFit();
    void zoomToItem(const QString &id);
    double zoom() const;
    void setZoom(double pixelsPerUnit);
    void zoomAt(double factor, QPoint viewportPos);

    // Highlights the net at a sheet point (pin, wire, junction or label);
    // returns false and clears the highlight if there is none.
    bool highlightNetAt(QPoint at);
    void clearHighlight();

    void editItem(const QString &id);
    void setTextPrompt(TextPrompt prompt) { m_prompt = std::move(prompt); }
    void setContextActions(const QList<QAction *> &actions) { m_contextActions = actions; }

    // Sheet point to viewport pixel and back; for tests.
    QPoint viewportPoint(QPointF sheetPoint) const;

    // Simulation values drawn over the schematic; they do not belong to the
    // document and are not selectable.
    void setAnnotations(const QList<SchematicAnnotation> &annotations);
    void clearAnnotations();
    QList<SchematicAnnotation> annotations() const { return m_annotations; }

    static constexpr double MinZoom = 0.05;
    static constexpr double MaxZoom = 40.0;

Q_SIGNALS:
    void statusMessage(const QString &text);
    void toolChanged(lvd::SchematicView::Tool tool);
    void componentActivated(const QString &id);
    void modeChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void drawForeground(QPainter *painter, const QRectF &rect) override;

private:
    struct DragState {
        QStringList ids;
        QHash<QString, QList<QPoint>> pins; // before the move
        QStringList stretchWires;           // unselected wires ending on a moving point
        QPoint delta;
    };

    SchematicItem *itemAt(QPoint viewportPos) const;
    QString pinAt(QPoint at) const; // "componentId/pin" or empty
    bool isConnectionPoint(QPoint at) const;
    // Net seed under the cursor: a pin near it, else the wire, junction or label hit.
    QString netSeedAt(QPoint viewportPos, SchematicItem *item) const;
    bool highlightSeed(const QString &seed);
    void setHover(SchematicItem *item);
    void updateCursorShape();
    void setCursorPoint(QPoint viewportPos);

    // Placement
    void updatePlacementGhost();
    void removePlacementGhost();
    bool placeComponent(const QString &key, QPoint at, int rotation, bool mirror);

    // Floating items (paste, duplicate)
    void startFloating(std::unique_ptr<Schematic> items, QPoint initialOffset);
    void rebuildFloatingGhosts();
    void updateFloating();
    void commitFloating();
    void endFloating();

    // Dragging the selection
    void beginDrag();
    void updateDrag(QPoint delta);
    void finishDrag(bool commit);

    // Rubber band
    void updateRubberBand(QPointF scenePos);
    void finishRubberBand(QPointF scenePos, Qt::KeyboardModifiers modifiers);

    // Wire tool
    void wireClick(QPoint at);
    void updateWirePreview();
    void finishWire();
    void cancelWire();

    void placeLabel(QPoint at);
    void placeText(QPoint at);
    std::optional<QString> prompt(const QString &title, const QString &label, const QString &initial,
                                  const QStringList &suggestions, bool multiLine);
    QStringList existingNetNames();
    void drawAnnotations(QPainter *painter);
    QPoint selectionCenter(const QStringList &ids) const;
    void reportNet(int index);

    SchematicScene *m_scene;
    Tool m_tool = Tool::Select;
    TextPrompt m_prompt;
    QList<QAction *> m_contextActions;

    QPoint m_cursor;          // snapped, sheet coordinates
    bool m_cursorInView = false;

    // Mouse state
    bool m_leftPressed = false;
    QPoint m_pressViewport;
    QPointF m_pressScene;
    QString m_pressItemId;
    bool m_pressSelectOnly = false; // on release without drag, select only the pressed item
    std::optional<DragState> m_drag;
    QGraphicsRectItem *m_rubberBand = nullptr;
    bool m_panning = false;
    QPoint m_panLast;
    bool m_spaceHeld = false;
    QString m_hoverId;
    QString m_hoverNet;

    // Placement
    QString m_placingKey;
    int m_placeRotation = 0;
    bool m_placeMirror = false;
    ComponentItem *m_ghost = nullptr;
    QString m_dropKey; // component dragged in from the library browser

    // Floating items
    std::unique_ptr<Schematic> m_floating;
    QList<SchematicItem *> m_floatingItems;
    QPoint m_floatingAnchor;
    QPoint m_floatingOffset;
    QString m_lastLabel;

    // Wire being drawn
    QList<QPoint> m_wirePoints;
    bool m_horizontalFirst = true;
    WireItem *m_wirePreview = nullptr;

    QList<SchematicAnnotation> m_annotations;
};

} // namespace lvd

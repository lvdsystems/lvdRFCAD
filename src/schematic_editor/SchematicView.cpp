#include "schematic_editor/SchematicView.h"

#include "core/Document.h"
#include "schematic_editor/EditorSupport.h"
#include "schematic_editor/SchematicItems.h"
#include "schematic_editor/SchematicScene.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGraphicsRectItem>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace lvd {

namespace {

constexpr int HitPixels = 4;        // pick tolerance around the cursor
constexpr double PinPickUnits = 6;  // how close to a pin a Ctrl+click must be
constexpr double DefaultZoom = 2.0; // pixels per drawing unit

bool isSchematicItem(const QGraphicsItem *item)
{
    return item->type() >= ComponentItem::Type && item->type() <= TextItem::Type;
}

} // namespace

SchematicView::SchematicView(SchematicScene *scene, QWidget *parent) : QGraphicsView(scene, parent), m_scene(scene)
{
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setViewportUpdateMode(SmartViewportUpdate);
    setCacheMode(CacheBackground);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(AnchorViewCenter);
    setResizeAnchor(AnchorViewCenter);
    setDragMode(NoDrag);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setFrameShape(QFrame::NoFrame);
    setTransform(QTransform::fromScale(DefaultZoom, DefaultZoom));
    centerOn(0, 0);
    updateCursorShape();

    // Items removed by undo or by another view must not stay hovered.
    connect(schematic(), &Schematic::itemRemoved, this, [this](const QString &id) {
        if (id == m_hoverId)
            m_hoverId.clear();
    });
}

SchematicView::~SchematicView()
{
    // Ghost items belong to the scene, which may outlive the view.
    removePlacementGhost();
    endFloating();
    cancelWire();
    delete m_rubberBand;
}

Schematic *SchematicView::schematic() const
{
    return m_scene->schematic();
}

// Tools and modes

void SchematicView::setTool(Tool tool)
{
    cancelTransient();
    if (isPlacing()) {
        m_placingKey.clear();
        removePlacementGhost();
    }
    m_tool = tool;
    setHover(nullptr);
    updateCursorShape();
    viewport()->update();
    Q_EMIT toolChanged(tool);
    Q_EMIT modeChanged();
    switch (tool) {
    case Tool::Select: Q_EMIT statusMessage(QString()); break;
    case Tool::Wire:
        Q_EMIT statusMessage(tr("Wire: click to start and to add corners, double-click or click a pin to end; "
                                "Space or / switches the corner, Backspace removes the last point"));
        break;
    case Tool::Label: Q_EMIT statusMessage(tr("Net label: click on a wire or pin to place a label")); break;
    case Tool::Text: Q_EMIT statusMessage(tr("Text: click to place a note")); break;
    case Tool::Highlight: Q_EMIT statusMessage(tr("Net highlight: click a wire or pin")); break;
    }
}

bool SchematicView::startPlacing(const QString &definitionKey)
{
    const Resolution resolution = m_scene->resolution(definitionKey);
    if (!resolution.definition) {
        Q_EMIT statusMessage(tr("Cannot place %1: the definition was not found").arg(definitionKey));
        return false;
    }
    if (definitionKey == Project::sheetKey(m_scene->document()->name())) {
        Q_EMIT statusMessage(tr("A schematic cannot contain itself"));
        return false;
    }
    if (m_tool != Tool::Select)
        setTool(Tool::Select);
    cancelTransient();
    m_placingKey = definitionKey;
    m_placeRotation = 0;
    m_placeMirror = false;
    updatePlacementGhost();
    updateCursorShape();
    setFocus();
    Q_EMIT modeChanged();
    Q_EMIT statusMessage(tr("Placing %1: click to place, R rotates, X mirrors, Esc ends")
                             .arg(resolution.definition->name));
    return true;
}

void SchematicView::cancel()
{
    if (m_drag) {
        finishDrag(false);
        return;
    }
    if (m_rubberBand) {
        delete m_rubberBand;
        m_rubberBand = nullptr;
        m_leftPressed = false;
        return;
    }
    if (m_floating) {
        endFloating();
        Q_EMIT statusMessage(QString());
        return;
    }
    if (isPlacing()) {
        m_placingKey.clear();
        removePlacementGhost();
        updateCursorShape();
        Q_EMIT modeChanged();
        Q_EMIT statusMessage(QString());
        return;
    }
    if (isDrawingWire()) {
        cancelWire();
        return;
    }
    if (m_tool != Tool::Select) {
        setTool(Tool::Select);
        return;
    }
    m_scene->clearSelection();
    clearHighlight();
}

void SchematicView::cancelTransient()
{
    if (m_drag)
        finishDrag(false);
    delete m_rubberBand;
    m_rubberBand = nullptr;
    m_leftPressed = false;
    m_pressItemId.clear();
    endFloating();
    cancelWire();
}

// Editing operations

bool SchematicView::hasSelection() const
{
    return !m_scene->selectedIds().isEmpty();
}

void SchematicView::deleteSelection()
{
    const QStringList ids = m_scene->selectedIds();
    if (ids.isEmpty())
        return;
    setHover(nullptr);
    Schematic::Edit edit(schematic(), tr("Delete"));
    schematic()->removeItems(ids);
    fixJunctions(*schematic(), m_scene->lookup());
}

QPoint SchematicView::selectionCenter(const QStringList &ids) const
{
    if (ids.size() == 1 && Schematic::kindOf(ids.first()) == ItemKind::Component)
        return schematic()->component(ids.first())->at;
    return gridCenter(anchorBounds(*schematic(), ids));
}

void SchematicView::rotateSelection()
{
    if (isPlacing()) {
        m_placeRotation = normalizedRotation(m_placeRotation + 90);
        updatePlacementGhost();
        return;
    }
    if (m_floating) {
        m_floating->rotateItems(m_floating->itemIds(), m_floatingAnchor, 90);
        rebuildFloatingGhosts();
        return;
    }
    const QStringList ids = m_scene->selectedIds();
    if (ids.isEmpty() || m_drag)
        return;
    Schematic::Edit edit(schematic(), tr("Rotate"));
    schematic()->rotateItems(ids, selectionCenter(ids), 90);
    fixJunctions(*schematic(), m_scene->lookup());
}

void SchematicView::mirrorSelection()
{
    if (isPlacing()) {
        // As Schematic::mirrorItems: mirroring the screen image.
        m_placeRotation = normalizedRotation(360 - m_placeRotation);
        m_placeMirror = !m_placeMirror;
        updatePlacementGhost();
        return;
    }
    if (m_floating) {
        m_floating->mirrorItems(m_floating->itemIds(), m_floatingAnchor);
        rebuildFloatingGhosts();
        return;
    }
    const QStringList ids = m_scene->selectedIds();
    if (ids.isEmpty() || m_drag)
        return;
    Schematic::Edit edit(schematic(), tr("Mirror"));
    schematic()->mirrorItems(ids, selectionCenter(ids));
    fixJunctions(*schematic(), m_scene->lookup());
}

void SchematicView::nudge(QPoint delta)
{
    const QStringList ids = m_scene->selectedIds();
    if (ids.isEmpty() || delta.isNull())
        return;
    QHash<QString, QList<QPoint>> pins;
    for (const QString &id : ids) {
        if (ComponentItem *component = m_scene->componentItem(id))
            pins.insert(id, component->pinPositions());
    }
    Schematic::Edit edit(schematic(), tr("Move"));
    schematic()->moveItems(ids, delta, pins, true);
    fixJunctions(*schematic(), m_scene->lookup());
}

void SchematicView::copySelection()
{
    const QStringList ids = m_scene->selectedIds();
    if (ids.isEmpty())
        return;
    const QByteArray bytes =
        QJsonDocument(itemsToJson(*schematic(), ids, m_scene->lookup())).toJson(QJsonDocument::Compact);
    auto *mime = new QMimeData;
    mime->setData(itemsMimeType(), bytes);
    QApplication::clipboard()->setMimeData(mime);
    Q_EMIT statusMessage(tr("Copied %n item(s)", nullptr, int(ids.size())));
}

void SchematicView::cutSelection()
{
    if (!hasSelection())
        return;
    copySelection();
    deleteSelection();
}

void SchematicView::paste()
{
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(itemsMimeType())) {
        Q_EMIT statusMessage(tr("The clipboard holds no schematic items"));
        return;
    }
    auto items = itemsFromJson(QJsonDocument::fromJson(mime->data(itemsMimeType())).object());
    if (!items || items->itemIds().isEmpty()) {
        Q_EMIT statusMessage(tr("The clipboard holds no schematic items"));
        return;
    }
    // Without the cursor in the view, paste at the view's centre.
    const QPoint centre = snapToGrid(mapToScene(viewport()->rect().center()));
    const QPoint anchor = gridCenter(anchorBounds(*items));
    startFloating(std::move(items), centre - anchor);
}

void SchematicView::duplicateSelection()
{
    const QStringList ids = m_scene->selectedIds();
    if (ids.isEmpty())
        return;
    auto items = itemsFromJson(itemsToJson(*schematic(), ids, m_scene->lookup()));
    if (items)
        startFloating(std::move(items), QPoint(2 * GridStep, 2 * GridStep));
}

void SchematicView::selectAll()
{
    if (m_tool != Tool::Select)
        setTool(Tool::Select);
    m_scene->selectIds(m_scene->schematicItems().keys());
}

// Zoom

double SchematicView::zoom() const
{
    return transform().m11();
}

void SchematicView::setZoom(double pixelsPerUnit)
{
    const double target = std::clamp(pixelsPerUnit, MinZoom, MaxZoom);
    const QPointF centre = mapToScene(viewport()->rect().center());
    setTransform(QTransform::fromScale(target, target));
    centerOn(centre);
}

void SchematicView::zoomAt(double factor, QPoint viewportPos)
{
    const double target = std::clamp(zoom() * factor, MinZoom, MaxZoom);
    const QPointF before = mapToScene(viewportPos);
    const QPointF centre = mapToScene(viewport()->rect().center());
    setTransform(QTransform::fromScale(target, target));
    centerOn(centre);
    const QPointF after = mapToScene(viewportPos);
    centerOn(mapToScene(viewport()->rect().center()) - (after - before));
}

void SchematicView::zoomToFit()
{
    const QRectF bounds = m_scene->contentBounds();
    if (bounds.isEmpty()) {
        setZoom(DefaultZoom);
        centerOn(0, 0);
        return;
    }
    const QRectF target = bounds.adjusted(-40, -40, 40, 40);
    const QSize size = viewport()->size();
    const double scale = std::min(size.width() / target.width(), size.height() / target.height());
    setTransform(QTransform::fromScale(std::clamp(scale, MinZoom, 4.0), std::clamp(scale, MinZoom, 4.0)));
    centerOn(target.center());
}

void SchematicView::zoomToItem(const QString &id)
{
    SchematicItem *item = m_scene->item(id);
    if (!item)
        return;
    const QRectF target = item->sceneBoundingRect().adjusted(-80, -80, 80, 80);
    const QSize size = viewport()->size();
    const double scale = std::min(size.width() / target.width(), size.height() / target.height());
    // Never zoom out to show an item, only in (up to a comfortable size).
    if (scale > zoom() || !mapToScene(viewport()->rect()).boundingRect().contains(target))
        setTransform(QTransform::fromScale(std::clamp(scale, MinZoom, 4.0), std::clamp(scale, MinZoom, 4.0)));
    centerOn(target.center());
    m_scene->selectIds({id});
}

QPoint SchematicView::viewportPoint(QPointF sheetPoint) const
{
    return mapFromScene(sheetPoint);
}

// Picking

SchematicItem *SchematicView::itemAt(QPoint viewportPos) const
{
    const QRectF area(mapToScene(viewportPos - QPoint(HitPixels, HitPixels)),
                      mapToScene(viewportPos + QPoint(HitPixels, HitPixels)));
    const QList<QGraphicsItem *> found = m_scene->items(area.normalized(), Qt::IntersectsItemShape, Qt::DescendingOrder,
                                                         transform());
    for (QGraphicsItem *candidate : found) {
        if (!isSchematicItem(candidate))
            continue;
        auto *item = static_cast<SchematicItem *>(candidate);
        if (!item->isGhost() && m_scene->item(item->id()) == item)
            return item;
    }
    return nullptr;
}

QString SchematicView::pinAt(QPoint at) const
{
    const QRectF area(QPointF(at) - QPointF(1, 1), QSizeF(2, 2));
    for (QGraphicsItem *candidate : m_scene->items(area, Qt::IntersectsItemBoundingRect)) {
        if (candidate->type() != ComponentItem::Type)
            continue;
        auto *component = static_cast<ComponentItem *>(candidate);
        if (component->isGhost() || m_scene->item(component->id()) != component)
            continue;
        const QString pin = component->pinAt(at);
        if (!pin.isEmpty())
            return component->id() + QLatin1Char('/') + pin;
    }
    return {};
}

bool SchematicView::isConnectionPoint(QPoint at) const
{
    if (!pinAt(at).isEmpty())
        return true;
    const QRectF area(QPointF(at) - QPointF(1, 1), QSizeF(2, 2));
    for (QGraphicsItem *candidate : m_scene->items(area, Qt::IntersectsItemBoundingRect)) {
        if (!isSchematicItem(candidate))
            continue;
        auto *item = static_cast<SchematicItem *>(candidate);
        if (item->isGhost() || m_scene->item(item->id()) != item)
            continue;
        if (auto *wire = qgraphicsitem_cast<WireItem *>(item)) {
            if (wireContains(wire->wire(), at))
                return true;
        } else if (item->kind() == ItemKind::Junction || item->kind() == ItemKind::Label) {
            if (item->anchor() == at)
                return true;
        }
    }
    return false;
}

QString SchematicView::netSeedAt(QPoint viewportPos, SchematicItem *item) const
{
    const QPointF exact = mapToScene(viewportPos);
    const QPoint snapped = snapToGrid(exact);
    if (QLineF(exact, QPointF(snapped)).length() <= PinPickUnits) {
        const QString pin = pinAt(snapped);
        if (!pin.isEmpty())
            return pin;
    }
    if (item && item->kind() != ItemKind::Component && item->kind() != ItemKind::Text)
        return item->id();
    return {};
}

bool SchematicView::highlightSeed(const QString &seed)
{
    if (seed.isEmpty()) {
        clearHighlight();
        return false;
    }
    m_scene->setHighlightSeed(seed);
    const int index = m_scene->highlightedNet();
    reportNet(index);
    return index >= 0;
}

bool SchematicView::highlightNetAt(QPoint at)
{
    QString seed = pinAt(at);
    if (seed.isEmpty()) {
        const QRectF area(QPointF(at) - QPointF(1, 1), QSizeF(2, 2));
        for (QGraphicsItem *candidate : m_scene->items(area, Qt::IntersectsItemBoundingRect, Qt::DescendingOrder)) {
            if (!isSchematicItem(candidate))
                continue;
            auto *item = static_cast<SchematicItem *>(candidate);
            if (item->isGhost() || m_scene->item(item->id()) != item)
                continue;
            if (auto *wire = qgraphicsitem_cast<WireItem *>(item); wire && wireContains(wire->wire(), at)) {
                seed = item->id();
                break;
            }
            if ((item->kind() == ItemKind::Junction || item->kind() == ItemKind::Label) && item->anchor() == at) {
                seed = item->id();
                break;
            }
        }
    }
    return highlightSeed(seed);
}

void SchematicView::clearHighlight()
{
    if (!m_scene->highlightSeed().isEmpty()) {
        m_scene->setHighlightSeed(QString());
        Q_EMIT statusMessage(QString());
    }
}

void SchematicView::reportNet(int index)
{
    const Connectivity &connectivity = m_scene->connectivity();
    if (index < 0 || index >= connectivity.nets.size()) {
        Q_EMIT statusMessage(QString());
        return;
    }
    const SheetNet &net = connectivity.nets.at(index);
    Q_EMIT statusMessage(tr("Net %1: %n pin(s)", nullptr, int(net.pins.size())).arg(net.name));
}

void SchematicView::setHover(SchematicItem *item)
{
    const QString id = item ? item->id() : QString();
    if (id == m_hoverId)
        return;
    if (SchematicItem *previous = m_scene->item(m_hoverId))
        previous->setHovered(false);
    m_hoverId = id;
    if (item)
        item->setHovered(true);

    // Hovering a wire names its net.
    QString net;
    if (item && item->kind() == ItemKind::Wire) {
        const Connectivity &connectivity = m_scene->connectivity();
        const int index = connectivity.netOfItem(id);
        if (index >= 0)
            net = connectivity.nets.at(index).name;
    }
    if (net != m_hoverNet) {
        m_hoverNet = net;
        Q_EMIT statusMessage(net.isEmpty() ? QString() : tr("Net %1").arg(net));
    }
}

void SchematicView::updateCursorShape()
{
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (m_panning)
        shape = Qt::ClosedHandCursor;
    else if (m_spaceHeld)
        shape = Qt::OpenHandCursor;
    else if (isPlacing() || m_floating || m_tool == Tool::Wire || m_tool == Tool::Label || m_tool == Tool::Text)
        shape = Qt::CrossCursor;
    else if (m_tool == Tool::Highlight)
        shape = Qt::PointingHandCursor;
    viewport()->setCursor(shape);
}

void SchematicView::setCursorPoint(QPoint viewportPos)
{
    const QPoint previous = m_cursor;
    m_cursor = snapToGrid(mapToScene(viewportPos));
    m_cursorInView = true;
    if (previous != m_cursor) {
        const QPoint margin(12, 12);
        viewport()->update(QRect(mapFromScene(previous) - margin, mapFromScene(previous) + margin));
        viewport()->update(QRect(mapFromScene(m_cursor) - margin, mapFromScene(m_cursor) + margin));
    }
}

// Placement

void SchematicView::updatePlacementGhost()
{
    const QString key = m_dropKey.isEmpty() ? m_placingKey : m_dropKey;
    if (key.isEmpty()) {
        removePlacementGhost();
        return;
    }
    const Resolution resolution = m_scene->resolution(key);
    PlacedComponent component;
    component.definition = key;
    component.at = m_cursor;
    component.rotation = m_placeRotation;
    component.mirror = m_placeMirror;
    component.reference = schematic()->nextReference(
        resolution.definition ? resolution.definition->referencePrefix : QStringLiteral("U"));
    if (!m_ghost) {
        m_ghost = new ComponentItem(component, resolution, m_scene);
        m_ghost->setGhost(true);
        m_scene->addItem(m_ghost);
    } else {
        m_ghost->setComponent(component, resolution);
    }
    m_ghost->setVisible(m_cursorInView || !m_dropKey.isEmpty());
}

void SchematicView::removePlacementGhost()
{
    delete m_ghost;
    m_ghost = nullptr;
}

bool SchematicView::placeComponent(const QString &key, QPoint at, int rotation, bool mirror)
{
    const Resolution resolution = m_scene->resolution(key);
    if (!resolution.definition)
        return false;
    Schematic::Edit edit(schematic(), tr("Place %1").arg(resolution.definition->name));
    const QString id = schematic()->addComponent(key, resolution.definition.get(), at, rotation, mirror);
    fixJunctions(*schematic(), m_scene->lookup());
    return !id.isEmpty();
}

// Floating items

void SchematicView::startFloating(std::unique_ptr<Schematic> items, QPoint initialOffset)
{
    cancelTransient();
    if (isPlacing()) {
        m_placingKey.clear();
        removePlacementGhost();
    }
    if (m_tool != Tool::Select)
        setTool(Tool::Select);
    m_floating = std::move(items);
    m_floatingAnchor = gridCenter(anchorBounds(*m_floating));
    m_floatingOffset = m_cursorInView ? m_cursor - m_floatingAnchor : initialOffset;
    rebuildFloatingGhosts();
    updateCursorShape();
    setFocus();
    Q_EMIT modeChanged();
    Q_EMIT statusMessage(tr("Click to place the items, R rotates, X mirrors, Esc cancels"));
}

void SchematicView::rebuildFloatingGhosts()
{
    qDeleteAll(m_floatingItems);
    m_floatingItems.clear();
    if (!m_floating)
        return;
    const Schematic &items = *m_floating;
    for (const PlacedComponent &component : items.components()) {
        Resolution resolution = m_scene->resolution(component.definition);
        if (!resolution.definition && items.cachedDefinition(component.definition)) {
            resolution.definition = items.cachedDefinition(component.definition);
            resolution.source = Resolution::Source::Cache;
        }
        m_floatingItems.append(new ComponentItem(component, resolution, m_scene));
    }
    for (const Wire &wire : items.wires())
        m_floatingItems.append(new WireItem(wire));
    for (const Junction &junction : items.junctions())
        m_floatingItems.append(new JunctionItem(junction));
    for (const NetLabel &label : items.labels())
        m_floatingItems.append(new LabelItem(label, m_scene));
    for (const TextNote &text : items.texts())
        m_floatingItems.append(new TextItem(text, m_scene));
    for (SchematicItem *item : std::as_const(m_floatingItems)) {
        item->setGhost(true);
        m_scene->addItem(item);
    }
    updateFloating();
}

void SchematicView::updateFloating()
{
    if (m_cursorInView)
        m_floatingOffset = m_cursor - m_floatingAnchor;
    for (SchematicItem *item : std::as_const(m_floatingItems))
        item->setPreviewOffset(m_floatingOffset);
}

void SchematicView::commitFloating()
{
    if (!m_floating)
        return;
    const QPoint offset = m_floatingOffset;
    const Schematic &items = *m_floating;
    Schematic *target = schematic();
    QStringList added;
    {
        Schematic::Edit edit(target, tr("Paste"));
        for (const PlacedComponent &component : items.components()) {
            Resolution resolution = m_scene->resolution(component.definition);
            if (!resolution.definition) {
                // Unknown here: keep the copied definition as this schematic's cache.
                if (const DefinitionPtr cached = items.cachedDefinition(component.definition)) {
                    target->setCachedDefinition(component.definition, cached);
                    m_scene->invalidateDefinitions(component.definition);
                    resolution = m_scene->resolution(component.definition);
                }
            }
            const QString id = target->addComponent(component.definition, resolution.definition.get(),
                                                    component.at + offset, component.rotation, component.mirror);
            if (!component.parameters.isEmpty()) {
                PlacedComponent placed = *target->component(id);
                placed.parameters = component.parameters;
                target->updateComponent(placed);
            }
            added.append(id);
        }
        for (const Wire &wire : items.wires()) {
            QList<QPoint> points = wire.points;
            for (QPoint &point : points)
                point += offset;
            added.append(target->addWire(points));
        }
        for (const Junction &junction : items.junctions())
            added.append(target->addJunction(junction.at + offset));
        for (const NetLabel &label : items.labels())
            added.append(target->addLabel(label.net, label.at + offset));
        for (const TextNote &text : items.texts())
            added.append(target->addText(text.text, text.at + offset, text.size));
        fixJunctions(*target, m_scene->lookup());
    }
    endFloating();
    added.removeAll(QString());
    m_scene->selectIds(added);
    Q_EMIT statusMessage(tr("Placed %n item(s)", nullptr, int(added.size())));
}

void SchematicView::endFloating()
{
    if (!m_floating && m_floatingItems.isEmpty())
        return;
    qDeleteAll(m_floatingItems);
    m_floatingItems.clear();
    m_floating.reset();
    updateCursorShape();
    Q_EMIT modeChanged();
}

// Dragging

void SchematicView::beginDrag()
{
    DragState drag;
    drag.ids = m_scene->selectedIds();
    if (drag.ids.isEmpty())
        return;
    QSet<QPoint> moving;
    const QSet<QString> selected(drag.ids.begin(), drag.ids.end());
    for (const QString &id : std::as_const(drag.ids)) {
        if (ComponentItem *component = m_scene->componentItem(id)) {
            const QList<QPoint> pins = component->pinPositions();
            drag.pins.insert(id, pins);
            for (const QPoint &pin : pins)
                moving.insert(pin);
        } else if (Schematic::kindOf(id) == ItemKind::Wire) {
            const Wire &wire = schematic()->wires()[id];
            moving.insert(wire.points.first());
            moving.insert(wire.points.last());
        }
    }
    for (const Wire &wire : schematic()->wires()) {
        if (!selected.contains(wire.id)
            && (moving.contains(wire.points.first()) || moving.contains(wire.points.last())))
            drag.stretchWires.append(wire.id);
    }
    m_drag = drag;
    setHover(nullptr);
}

void SchematicView::updateDrag(QPoint delta)
{
    if (!m_drag || delta == m_drag->delta)
        return;
    m_drag->delta = delta;

    // Wires, moved or stretched, are previewed through the same code that
    // applies the move, run on a scratch copy of just those wires.
    QJsonArray wires;
    QStringList wireIds;
    for (const QString &id : std::as_const(m_drag->ids)) {
        if (Schematic::kindOf(id) == ItemKind::Wire) {
            wires.append(wireToJson(schematic()->wires()[id]));
            wireIds.append(id);
        } else if (SchematicItem *item = m_scene->item(id)) {
            item->setPreviewOffset(delta);
        }
    }
    for (const QString &id : std::as_const(m_drag->stretchWires)) {
        wires.append(wireToJson(schematic()->wires()[id]));
        wireIds.append(id);
    }
    if (wireIds.isEmpty())
        return;
    auto scratch = Schematic::fromJson(QJsonObject{{QStringLiteral("wires"), wires}});
    if (!scratch)
        return;
    scratch->moveItems(m_drag->ids, delta, m_drag->pins, true);
    for (const QString &id : std::as_const(wireIds)) {
        if (auto *item = qgraphicsitem_cast<WireItem *>(m_scene->item(id)))
            item->setPreviewPoints(scratch->wires().contains(id) ? scratch->wires()[id].points : QList<QPoint>());
    }
}

void SchematicView::finishDrag(bool commit)
{
    if (!m_drag)
        return;
    const DragState drag = *m_drag;
    m_drag.reset();
    for (const QString &id : drag.ids + drag.stretchWires) {
        if (SchematicItem *item = m_scene->item(id))
            item->resetPreview();
    }
    if (commit && !drag.delta.isNull()) {
        Schematic::Edit edit(schematic(), tr("Move"));
        schematic()->moveItems(drag.ids, drag.delta, drag.pins, true);
        fixJunctions(*schematic(), m_scene->lookup());
    }
}

// Rubber band

void SchematicView::updateRubberBand(QPointF scenePos)
{
    if (!m_rubberBand)
        return;
    m_rubberBand->setRect(QRectF(m_pressScene, scenePos).normalized());
    m_rubberBand->setVisible(true);
}

void SchematicView::finishRubberBand(QPointF scenePos, Qt::KeyboardModifiers modifiers)
{
    const QRectF area = QRectF(m_pressScene, scenePos).normalized();
    delete m_rubberBand;
    m_rubberBand = nullptr;
    if ((mapFromScene(scenePos) - m_pressViewport).manhattanLength() < QApplication::startDragDistance())
        return;
    // Left to right selects what is fully inside, right to left what is touched.
    const Qt::ItemSelectionMode mode =
        scenePos.x() >= m_pressScene.x() ? Qt::ContainsItemShape : Qt::IntersectsItemShape;
    QStringList ids;
    for (QGraphicsItem *candidate : m_scene->items(area, mode)) {
        if (!isSchematicItem(candidate))
            continue;
        auto *item = static_cast<SchematicItem *>(candidate);
        if (!item->isGhost() && m_scene->item(item->id()) == item)
            ids.append(item->id());
    }
    m_scene->selectIds(ids, !(modifiers & (Qt::ShiftModifier | Qt::ControlModifier)));
}

// Wires

void SchematicView::wireClick(QPoint at)
{
    if (m_wirePoints.isEmpty()) {
        m_wirePoints = {at};
        updateWirePreview();
        return;
    }
    if (at == m_wirePoints.last()) {
        finishWire();
        return;
    }
    m_wirePoints += orthogonalRoute(m_wirePoints.last(), at, m_horizontalFirst);
    if (isConnectionPoint(at))
        finishWire();
    else
        updateWirePreview();
}

void SchematicView::updateWirePreview()
{
    if (m_wirePoints.isEmpty()) {
        delete m_wirePreview;
        m_wirePreview = nullptr;
        return;
    }
    QList<QPoint> points = m_wirePoints;
    if (m_cursorInView)
        points += orthogonalRoute(points.last(), m_cursor, m_horizontalFirst);
    Wire wire{QString(), simplifyPolyline(points)};
    if (wire.points.size() == 1)
        wire.points.append(wire.points.first());
    if (!m_wirePreview) {
        m_wirePreview = new WireItem(wire);
        m_wirePreview->setGhost(true);
        m_scene->addItem(m_wirePreview);
    } else {
        m_wirePreview->setWire(wire);
    }
}

void SchematicView::finishWire()
{
    const QList<QPoint> points = simplifyPolyline(m_wirePoints);
    cancelWire();
    if (points.size() < 2)
        return;
    Schematic::Edit edit(schematic(), tr("Add wire"));
    schematic()->addWire(points);
    fixJunctions(*schematic(), m_scene->lookup());
}

void SchematicView::cancelWire()
{
    m_wirePoints.clear();
    delete m_wirePreview;
    m_wirePreview = nullptr;
}

// Labels and texts

std::optional<QString> SchematicView::prompt(const QString &title, const QString &label, const QString &initial,
                                             const QStringList &suggestions, bool multiLine)
{
    if (m_prompt)
        return m_prompt(title, label, initial, suggestions, multiLine);
    bool ok = false;
    QString text;
    if (multiLine) {
        text = QInputDialog::getMultiLineText(this, title, label, initial, &ok);
    } else if (!suggestions.isEmpty()) {
        QStringList choices = suggestions;
        if (!initial.isEmpty() && !choices.contains(initial))
            choices.prepend(initial);
        text = QInputDialog::getItem(this, title, label, choices, std::max<int>(0, int(choices.indexOf(initial))), true,
                                     &ok);
    } else {
        text = QInputDialog::getText(this, title, label, QLineEdit::Normal, initial, &ok);
    }
    if (!ok)
        return std::nullopt;
    return text;
}

QStringList SchematicView::existingNetNames()
{
    QSet<QString> names;
    for (const NetLabel &label : schematic()->labels())
        names.insert(label.net);
    QStringList sorted(names.begin(), names.end());
    sorted.sort(Qt::CaseInsensitive);
    return sorted;
}

void SchematicView::placeLabel(QPoint at)
{
    const auto name = prompt(tr("Net Label"), tr("Net name:"), m_lastLabel, existingNetNames(), false);
    if (!name || name->trimmed().isEmpty())
        return;
    m_lastLabel = name->trimmed();
    Schematic::Edit edit(schematic(), tr("Add net label"));
    schematic()->addLabel(m_lastLabel, at);
}

void SchematicView::placeText(QPoint at)
{
    const auto text = prompt(tr("Text"), tr("Text:"), QString(), {}, true);
    if (!text || text->trimmed().isEmpty())
        return;
    Schematic::Edit edit(schematic(), tr("Add text"));
    schematic()->addText(*text, at);
}

void SchematicView::editItem(const QString &id)
{
    switch (Schematic::kindOf(id)) {
    case ItemKind::Label: {
        NetLabel label = schematic()->labels().value(id);
        const auto name = prompt(tr("Net Label"), tr("Net name:"), label.net, existingNetNames(), false);
        if (name && !name->trimmed().isEmpty() && name->trimmed() != label.net) {
            label.net = name->trimmed();
            schematic()->updateLabel(label);
        }
        break;
    }
    case ItemKind::Text: {
        TextNote text = schematic()->texts().value(id);
        const auto changed = prompt(tr("Text"), tr("Text:"), text.text, {}, true);
        if (changed && !changed->trimmed().isEmpty() && *changed != text.text) {
            text.text = *changed;
            schematic()->updateText(text);
        }
        break;
    }
    case ItemKind::Component: Q_EMIT componentActivated(id); break;
    case ItemKind::Wire:
    case ItemKind::Junction:
    case ItemKind::None: break;
    }
}

// Events

void SchematicView::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    setCursorPoint(pos);
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && m_spaceHeld)) {
        m_panning = true;
        m_panLast = pos;
        updateCursorShape();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->accept();
        return;
    }
    if (m_floating) {
        commitFloating();
        return;
    }
    if (isPlacing()) {
        placeComponent(m_placingKey, m_cursor, m_placeRotation, m_placeMirror);
        updatePlacementGhost(); // next reference
        return;
    }
    switch (m_tool) {
    case Tool::Wire: wireClick(m_cursor); return;
    case Tool::Label: placeLabel(m_cursor); return;
    case Tool::Text: placeText(m_cursor); return;
    case Tool::Highlight: highlightSeed(netSeedAt(pos, itemAt(pos))); return;
    case Tool::Select: break;
    }

    SchematicItem *item = itemAt(pos);
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    if (modifiers & Qt::ControlModifier) {
        const QString seed = netSeedAt(pos, item);
        if (!seed.isEmpty()) {
            highlightSeed(seed);
            return;
        }
    }
    m_leftPressed = true;
    m_pressViewport = pos;
    m_pressScene = mapToScene(pos);
    m_pressItemId.clear();
    m_pressSelectOnly = false;
    if (item) {
        if (modifiers & (Qt::ShiftModifier | Qt::ControlModifier)) {
            item->setSelected(!item->isSelected());
            return;
        }
        m_pressItemId = item->id();
        if (!item->isSelected())
            m_scene->selectIds({item->id()});
        else
            m_pressSelectOnly = true;
        return;
    }
    if (!(modifiers & (Qt::ShiftModifier | Qt::ControlModifier)))
        m_scene->clearSelection();
    m_rubberBand = new QGraphicsRectItem;
    QPen pen(m_scene->style().selection, 1.0);
    pen.setCosmetic(true);
    pen.setStyle(Qt::DashLine);
    QColor fill = m_scene->style().selection;
    fill.setAlpha(30);
    m_rubberBand->setPen(pen);
    m_rubberBand->setBrush(fill);
    m_rubberBand->setZValue(200);
    m_rubberBand->setVisible(false);
    m_scene->addItem(m_rubberBand);
}

void SchematicView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    if (m_panning) {
        const QPoint delta = pos - m_panLast;
        m_panLast = pos;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        return;
    }
    setCursorPoint(pos);
    if (m_floating) {
        updateFloating();
        return;
    }
    if (m_ghost) {
        m_ghost->setPos(m_cursor);
        m_ghost->setVisible(true);
    }
    if (isDrawingWire())
        updateWirePreview();
    if (m_leftPressed) {
        if (m_rubberBand) {
            updateRubberBand(mapToScene(pos));
        } else if (!m_pressItemId.isEmpty()) {
            if (!m_drag && (pos - m_pressViewport).manhattanLength() >= QApplication::startDragDistance())
                beginDrag();
            if (m_drag)
                updateDrag(m_cursor - snapToGrid(m_pressScene));
        }
        return;
    }
    if ((m_tool == Tool::Select || m_tool == Tool::Highlight) && !isPlacing())
        setHover(itemAt(pos));
}

void SchematicView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        m_panning = false;
        updateCursorShape();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    if (m_drag) {
        finishDrag(true);
    } else if (m_rubberBand) {
        finishRubberBand(mapToScene(event->position().toPoint()), event->modifiers());
    } else if (m_pressSelectOnly && !m_pressItemId.isEmpty()) {
        m_scene->selectIds({m_pressItemId});
    }
    m_leftPressed = false;
    m_pressItemId.clear();
    m_pressSelectOnly = false;
}

void SchematicView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->accept();
        return;
    }
    if (m_tool == Tool::Wire) {
        if (isDrawingWire())
            finishWire();
        return;
    }
    // The first click already placed; a second one at the same spot would stack.
    if (m_tool != Tool::Select || isPlacing() || m_floating)
        return;
    if (SchematicItem *item = itemAt(event->position().toPoint()))
        editItem(item->id());
}

void SchematicView::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    zoomAt(std::pow(1.0015, delta), event->position().toPoint());
    setCursorPoint(event->position().toPoint());
    event->accept();
}

void SchematicView::keyPressEvent(QKeyEvent *event)
{
    const bool moveKeys = m_tool == Tool::Select && !isPlacing() && !m_floating && !m_drag;
    const int step = event->modifiers() & Qt::ShiftModifier ? 5 * GridStep : GridStep;
    switch (event->key()) {
    case Qt::Key_Escape: cancel(); return;
    case Qt::Key_Backspace:
        if (isDrawingWire()) {
            m_wirePoints.removeLast();
            if (m_wirePoints.isEmpty())
                cancelWire();
            else
                updateWirePreview();
            return;
        }
        break;
    case Qt::Key_Space:
        if (isDrawingWire()) {
            if (!event->isAutoRepeat()) {
                m_horizontalFirst = !m_horizontalFirst;
                updateWirePreview();
            }
            return;
        }
        if (!event->isAutoRepeat()) {
            m_spaceHeld = true;
            updateCursorShape();
        }
        return;
    case Qt::Key_Slash:
        if (isDrawingWire()) {
            m_horizontalFirst = !m_horizontalFirst;
            updateWirePreview();
            return;
        }
        break;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (moveKeys && hasSelection()) {
            const QPoint delta = event->key() == Qt::Key_Left    ? QPoint(-step, 0)
                                 : event->key() == Qt::Key_Right ? QPoint(step, 0)
                                 : event->key() == Qt::Key_Up    ? QPoint(0, -step)
                                                                 : QPoint(0, step);
            nudge(delta);
            return;
        }
        break;
    default: break;
    }
    QGraphicsView::keyPressEvent(event);
}

void SchematicView::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = false;
        updateCursorShape();
        return;
    }
    QGraphicsView::keyReleaseEvent(event);
}

void SchematicView::contextMenuEvent(QContextMenuEvent *event)
{
    // A right click ends the current mode rather than opening a menu.
    if (isDrawingWire()) {
        finishWire();
        return;
    }
    if (m_floating || isPlacing() || m_tool != Tool::Select) {
        cancel();
        return;
    }
    if (m_contextActions.isEmpty())
        return;
    if (SchematicItem *item = itemAt(event->pos()); item && !item->isSelected())
        m_scene->selectIds({item->id()});
    QMenu menu(this);
    menu.addActions(m_contextActions);
    menu.exec(event->globalPos());
}

void SchematicView::leaveEvent(QEvent *event)
{
    m_cursorInView = false;
    if (m_ghost)
        m_ghost->setVisible(false);
    setHover(nullptr);
    if (isDrawingWire())
        updateWirePreview();
    viewport()->update();
    QGraphicsView::leaveEvent(event);
}

void SchematicView::focusOutEvent(QFocusEvent *event)
{
    m_spaceHeld = false;
    updateCursorShape();
    QGraphicsView::focusOutEvent(event);
}

void SchematicView::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasFormat(componentMimeType())) {
        event->ignore();
        return;
    }
    const QString key = QString::fromUtf8(event->mimeData()->data(componentMimeType())).trimmed();
    if (!m_scene->resolution(key).definition || key == Project::sheetKey(m_scene->document()->name())) {
        event->ignore();
        return;
    }
    m_dropKey = key;
    setCursorPoint(event->position().toPoint());
    updatePlacementGhost();
    event->acceptProposedAction();
}

void SchematicView::dragMoveEvent(QDragMoveEvent *event)
{
    if (m_dropKey.isEmpty()) {
        event->ignore();
        return;
    }
    setCursorPoint(event->position().toPoint());
    if (m_ghost) {
        m_ghost->setPos(m_cursor);
        m_ghost->setVisible(true);
    }
    event->acceptProposedAction();
}

void SchematicView::dragLeaveEvent(QDragLeaveEvent *event)
{
    m_dropKey.clear();
    updatePlacementGhost();
    event->accept();
}

void SchematicView::dropEvent(QDropEvent *event)
{
    if (m_dropKey.isEmpty()) {
        event->ignore();
        return;
    }
    const QString key = m_dropKey;
    m_dropKey.clear();
    setCursorPoint(event->position().toPoint());
    updatePlacementGhost();
    if (placeComponent(key, m_cursor, isPlacing() ? m_placeRotation : 0, isPlacing() && m_placeMirror))
        event->acceptProposedAction();
    setFocus();
}

void SchematicView::setAnnotations(const QList<SchematicAnnotation> &annotations)
{
    m_annotations = annotations;
    viewport()->update();
}

void SchematicView::clearAnnotations()
{
    if (m_annotations.isEmpty())
        return;
    m_annotations.clear();
    viewport()->update();
}

void SchematicView::drawAnnotations(QPainter *painter)
{
    if (m_annotations.isEmpty())
        return;
    // Drawn in viewport pixels so the text stays readable at any zoom.
    painter->save();
    painter->resetTransform();
    QFont font = this->font();
    font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1.0));
    painter->setFont(font);
    const QFontMetrics metrics(font);
    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    for (const SchematicAnnotation &annotation : std::as_const(m_annotations)) {
        const QPoint anchor = mapFromScene(annotation.at);
        QColor colour;
        switch (annotation.kind) {
        case SchematicAnnotation::Kind::Voltage:
            colour = dark ? QColor(0x64, 0xb5, 0xf6) : QColor(0x15, 0x65, 0xc0);
            break;
        case SchematicAnnotation::Kind::Current:
            colour = dark ? QColor(0xff, 0xb7, 0x4d) : QColor(0xc6, 0x28, 0x28);
            break;
        case SchematicAnnotation::Kind::Power:
            colour = dark ? QColor(0xba, 0x68, 0xc8) : QColor(0x6a, 0x1b, 0x9a);
            break;
        }
        const QStringList lines = annotation.text.split(QLatin1Char('\n'));
        int width = 0;
        for (const QString &line : lines)
            width = std::max(width, metrics.horizontalAdvance(line));
        const int height = metrics.height() * int(lines.size());
        QRect box(anchor.x() + 4, anchor.y() - height - 4, width + 6, height + 2);
        QColor background = palette().color(QPalette::Base);
        background.setAlpha(225);
        painter->setPen(QPen(colour, 1.0));
        painter->setBrush(background);
        painter->drawRoundedRect(box, 3, 3);
        painter->drawLine(anchor, QPoint(box.left(), box.bottom()));
        painter->setPen(colour);
        for (int i = 0; i < lines.size(); ++i)
            painter->drawText(QPoint(box.left() + 3, box.top() + 1 + metrics.ascent() + i * metrics.height()), lines.at(i));
    }
    painter->restore();
}

void SchematicView::drawForeground(QPainter *painter, const QRectF &)
{
    drawAnnotations(painter);
    const bool crosshair = isPlacing() || m_tool == Tool::Wire || m_tool == Tool::Label || m_tool == Tool::Text;
    if (!m_cursorInView || !crosshair)
        return;
    const double size = 7.0 / zoom();
    QPen pen(m_scene->style().selection, 1.0);
    pen.setCosmetic(true);
    painter->save();
    painter->setPen(pen);
    const QPointF c(m_cursor);
    painter->drawLine(c - QPointF(size, 0), c + QPointF(size, 0));
    painter->drawLine(c - QPointF(0, size), c + QPointF(0, size));
    painter->restore();
}

} // namespace lvd

#include "schematic_editor/SchematicScene.h"

#include "core/Document.h"
#include "schematic_editor/SchematicItems.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace lvd {

namespace {

// Connectivity is recomputed this long after the last edit.
constexpr int ConnectivityDelayMs = 40;
// Scene extent: large enough to never get in the way of panning.
constexpr double SceneExtent = 1.0e6;

} // namespace

SchematicScene::SchematicScene(Project *project, Document *document, QObject *parent)
    : QGraphicsScene(parent), m_project(project), m_document(document), m_schematic(document->schematic()),
      m_style(SchematicStyle::light()), m_painter(m_style), m_metrics(m_style.font(10))
{
    Q_ASSERT(m_project && m_schematic);
    setSceneRect(-SceneExtent, -SceneExtent, 2 * SceneExtent, 2 * SceneExtent);
    setItemIndexMethod(BspTreeIndex);

    m_connectivityTimer.setSingleShot(true);
    m_connectivityTimer.setInterval(ConnectivityDelayMs);
    connect(&m_connectivityTimer, &QTimer::timeout, this, [this] {
        if (m_connectivityDirty)
            recomputeConnectivity();
    });

    connect(m_schematic, &Schematic::itemAdded, this, &SchematicScene::addItemFor);
    connect(m_schematic, &Schematic::itemChanged, this, &SchematicScene::updateItemFor);
    connect(m_schematic, &Schematic::itemRemoved, this, &SchematicScene::removeItemFor);
    connect(m_schematic, &Schematic::changed, this, &SchematicScene::scheduleConnectivity);

    if (LibraryManager *libraries = m_project->libraryManager())
        connect(libraries, &LibraryManager::librariesChanged, this, [this] { invalidateDefinitions(); });
    // Saving refreshes the definition cache, which clears "changed in library" flags.
    connect(m_document, &Document::modifiedChanged, this, [this](bool modified) {
        if (!modified)
            invalidateDefinitions();
    });
    connect(m_project, &Project::documentAdded, this, [this](Document *added) {
        watchSheet(added);
        invalidateDefinitions(Project::sheetKey(added->name()));
    });
    connect(m_project, &Project::documentAboutToBeRemoved, this, [this](Document *removed) {
        const QString key = Project::sheetKey(removed->name());
        QTimer::singleShot(0, this, [this, key] { invalidateDefinitions(key); });
    });
    for (Document *other : m_project->documents())
        watchSheet(other);

    for (const QString &id : m_schematic->itemIds())
        addItemFor(id);
    scheduleConnectivity();
}

SchematicScene::~SchematicScene() = default;

void SchematicScene::watchSheet(Document *other)
{
    if (!other || other == m_document || !other->schematic())
        return;
    const QString key = Project::sheetKey(other->name());
    connect(other->schematic(), &Schematic::changed, this, [this, key] {
        if (m_resolutions.contains(key))
            invalidateDefinitions(key);
    });
}

void SchematicScene::setStyle(const SchematicStyle &style)
{
    m_style = style;
    m_painter = SymbolPainter(style);
    m_metrics = QFontMetricsF(style.font(10));
    for (SchematicItem *item : std::as_const(m_items)) {
        if (auto *component = qgraphicsitem_cast<ComponentItem *>(item))
            component->setComponent(component->component(), component->resolution());
        item->update();
    }
    update();
}

QSizeF SchematicScene::textSize(const QString &text, double size) const
{
    // Matches SymbolPainter::paintText, which scales a 10 pt font.
    const double scale = size / m_metrics.height() * 1.25;
    return QSizeF(m_metrics.horizontalAdvance(text) * scale, m_metrics.height() * scale);
}

Resolution SchematicScene::resolution(const QString &key)
{
    const auto it = m_resolutions.constFind(key);
    if (it != m_resolutions.cend())
        return *it;
    const Resolution resolved = m_project->resolve(*m_schematic, key);
    m_resolutions.insert(key, resolved);
    return resolved;
}

DefinitionLookup SchematicScene::lookup()
{
    return [this](const QString &key) { return resolution(key).definition; };
}

void SchematicScene::invalidateDefinitions(const QString &key)
{
    if (key.isEmpty())
        m_resolutions.clear();
    else if (!m_resolutions.remove(key))
        return;
    for (SchematicItem *item : std::as_const(m_items)) {
        auto *component = qgraphicsitem_cast<ComponentItem *>(item);
        if (component && (key.isEmpty() || component->component().definition == key))
            component->setComponent(component->component(), resolution(component->component().definition));
    }
    scheduleConnectivity();
}

ComponentItem *SchematicScene::componentItem(const QString &id) const
{
    return qgraphicsitem_cast<ComponentItem *>(m_items.value(id));
}

void SchematicScene::addItemFor(const QString &id)
{
    if (m_items.contains(id)) {
        updateItemFor(id);
        return;
    }
    SchematicItem *item = nullptr;
    switch (Schematic::kindOf(id)) {
    case ItemKind::Component:
        if (const PlacedComponent *component = m_schematic->component(id))
            item = new ComponentItem(*component, resolution(component->definition), this);
        break;
    case ItemKind::Wire:
        if (m_schematic->wires().contains(id))
            item = new WireItem(m_schematic->wires().value(id));
        break;
    case ItemKind::Junction:
        if (m_schematic->junctions().contains(id))
            item = new JunctionItem(m_schematic->junctions().value(id));
        break;
    case ItemKind::Label:
        if (m_schematic->labels().contains(id))
            item = new LabelItem(m_schematic->labels().value(id), this);
        break;
    case ItemKind::Text:
        if (m_schematic->texts().contains(id))
            item = new TextItem(m_schematic->texts().value(id), this);
        break;
    case ItemKind::None:
        break;
    }
    if (!item)
        return;
    m_items.insert(id, item);
    addItem(item);
    for (const RuleViolation &violation : std::as_const(m_violations)) {
        if (violation.itemId == id && violation.document == m_document->name())
            item->setMark(violation.severity);
    }
    m_connectivityDirty = true;
}

void SchematicScene::updateItemFor(const QString &id)
{
    SchematicItem *item = m_items.value(id);
    if (!item) {
        addItemFor(id);
        return;
    }
    if (auto *component = qgraphicsitem_cast<ComponentItem *>(item)) {
        if (const PlacedComponent *data = m_schematic->component(id))
            component->setComponent(*data, resolution(data->definition));
    } else if (auto *wire = qgraphicsitem_cast<WireItem *>(item)) {
        wire->setWire(m_schematic->wires().value(id));
    } else if (auto *junction = qgraphicsitem_cast<JunctionItem *>(item)) {
        junction->setJunction(m_schematic->junctions().value(id));
    } else if (auto *label = qgraphicsitem_cast<LabelItem *>(item)) {
        label->setLabel(m_schematic->labels().value(id));
    } else if (auto *text = qgraphicsitem_cast<TextItem *>(item)) {
        text->setText(m_schematic->texts().value(id));
    }
    m_connectivityDirty = true;
}

void SchematicScene::removeItemFor(const QString &id)
{
    SchematicItem *item = m_items.take(id);
    if (!item)
        return;
    m_highlightedIds.remove(id);
    removeItem(item);
    delete item;
    m_connectivityDirty = true;
}

void SchematicScene::scheduleConnectivity()
{
    m_connectivityDirty = true;
    m_connectivityTimer.start();
}

const Connectivity &SchematicScene::connectivity()
{
    if (m_connectivityDirty)
        recomputeConnectivity();
    return m_connectivity;
}

void SchematicScene::recomputeConnectivity()
{
    m_connectivityTimer.stop();
    m_connectivityDirty = false;
    m_connectivity = computeConnectivity(*m_schematic, lookup());

    // A pin alone on its net, with no wire or label, is unconnected.
    QHash<QString, QSet<QString>> unconnected;
    for (const SheetNet &net : std::as_const(m_connectivity.nets)) {
        if (net.pins.size() == 1 && net.wireIds.isEmpty() && net.labelIds.isEmpty())
            unconnected[net.pins.first().componentId].insert(net.pins.first().pin);
    }
    for (SchematicItem *item : std::as_const(m_items)) {
        if (auto *component = qgraphicsitem_cast<ComponentItem *>(item))
            component->setUnconnectedPins(unconnected.value(component->id()));
    }
    applyHighlight();
    Q_EMIT connectivityUpdated();
}

void SchematicScene::setHighlightSeed(const QString &seed)
{
    m_highlightSeed = seed;
    if (m_connectivityDirty)
        recomputeConnectivity(); // also applies the highlight
    else
        applyHighlight();
}

int SchematicScene::highlightedNet()
{
    if (m_highlightSeed.isEmpty())
        return -1;
    const Connectivity &nets = connectivity();
    const qsizetype slash = m_highlightSeed.indexOf(QLatin1Char('/'));
    if (slash > 0)
        return nets.netOfPin(m_highlightSeed.left(slash), m_highlightSeed.mid(slash + 1));
    return nets.netOfItem(m_highlightSeed);
}

void SchematicScene::applyHighlight()
{
    QSet<QString> ids;
    QHash<QString, QSet<QString>> pins;
    const int index = highlightedNet();
    if (index >= 0) {
        const SheetNet &net = m_connectivity.nets.at(index);
        for (const QString &id : net.wireIds + net.junctionIds + net.labelIds)
            ids.insert(id);
        for (const NetPin &pin : net.pins) {
            pins[pin.componentId].insert(pin.pin);
            ids.insert(pin.componentId);
        }
    }
    for (const QString &id : std::as_const(m_highlightedIds)) {
        if (ids.contains(id))
            continue;
        if (SchematicItem *item = m_items.value(id)) {
            item->setHighlighted(false);
            if (auto *component = qgraphicsitem_cast<ComponentItem *>(item))
                component->setHighlightedPins({});
        }
    }
    for (const QString &id : std::as_const(ids)) {
        SchematicItem *item = m_items.value(id);
        if (!item)
            continue;
        if (auto *component = qgraphicsitem_cast<ComponentItem *>(item))
            component->setHighlightedPins(pins.value(id));
        else
            item->setHighlighted(true);
    }
    m_highlightedIds = ids;
}

void SchematicScene::setViolations(const QList<RuleViolation> &violations)
{
    m_violations = violations;
    applyMarks();
}

void SchematicScene::applyMarks()
{
    QHash<QString, RuleViolation::Severity> worst;
    for (const RuleViolation &violation : std::as_const(m_violations)) {
        if (violation.itemId.isEmpty() || violation.document != m_document->name())
            continue;
        const auto it = worst.constFind(violation.itemId);
        // Error < Warning < Info in the enumeration.
        if (it == worst.cend() || violation.severity < *it)
            worst.insert(violation.itemId, violation.severity);
    }
    for (SchematicItem *item : std::as_const(m_items)) {
        const auto it = worst.constFind(item->id());
        item->setMark(it == worst.cend() || *it == RuleViolation::Severity::Info
                          ? std::nullopt
                          : std::optional<RuleViolation::Severity>(*it));
    }
}

QStringList SchematicScene::selectedIds() const
{
    QStringList ids;
    for (QGraphicsItem *item : selectedItems()) {
        auto *schematicItem = static_cast<SchematicItem *>(item);
        if (!schematicItem->isGhost() && m_items.value(schematicItem->id()) == schematicItem)
            ids.append(schematicItem->id());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void SchematicScene::selectIds(const QStringList &ids, bool clearFirst)
{
    const bool blocked = blockSignals(true);
    if (clearFirst)
        clearSelection();
    for (const QString &id : ids) {
        if (SchematicItem *item = m_items.value(id))
            item->setSelected(true);
    }
    blockSignals(blocked);
    Q_EMIT selectionChanged();
}

QRectF SchematicScene::contentBounds() const
{
    QRectF bounds;
    for (SchematicItem *item : std::as_const(m_items))
        bounds = bounds.united(item->sceneBoundingRect());
    return bounds;
}

void SchematicScene::setGridVisible(bool visible)
{
    m_gridVisible = visible;
    update();
}

void SchematicScene::drawBackground(QPainter *painter, const QRectF &rect)
{
    painter->fillRect(rect, m_style.background);
    if (!m_gridVisible)
        return;
    const double scale = painter->worldTransform().m11() == 0 ? 1.0 : std::abs(painter->worldTransform().m11());
    // Coarser grid when dots would be closer than 8 pixels.
    int step = 10;
    while (step * scale < 8)
        step *= 5;
    const double left = std::floor(rect.left() / step) * step;
    const double top = std::floor(rect.top() / step) * step;
    QList<QPointF> dots;
    for (double x = left; x <= rect.right(); x += step) {
        for (double y = top; y <= rect.bottom(); y += step)
            dots.append(QPointF(x, y));
    }
    QPen pen(m_style.grid, std::clamp(scale * 0.18, 1.0, 2.5));
    pen.setCosmetic(true);
    pen.setCapStyle(Qt::RoundCap);
    painter->save();
    painter->setPen(pen);
    painter->drawPoints(dots.constData(), int(dots.size()));
    painter->restore();
}

} // namespace lvd

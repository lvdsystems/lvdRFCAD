#include "library_ui/SymbolCanvas.h"

#include "library_ui/LibraryUiCommon.h"
#include "widgets/SymbolPainter.h"

#include <QGraphicsScene>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lvd {

namespace {

constexpr double minimumZoom = 0.5;
constexpr double maximumZoom = 60;
constexpr double handleSize = 7; // pixels

QPointF directionVector(PinDirection direction)
{
    switch (direction) {
    case PinDirection::Left: return QPointF(-1, 0);
    case PinDirection::Right: return QPointF(1, 0);
    case PinDirection::Up: return QPointF(0, -1);
    case PinDirection::Down: return QPointF(0, 1);
    }
    return {};
}

QPointF pinEnd(const SymbolPin &pin)
{
    return QPointF(pin.at) + directionVector(pin.direction) * pin.length;
}

double distanceToSegment(QPointF point, QPointF a, QPointF b)
{
    const QPointF ab = b - a;
    const double lengthSquared = QPointF::dotProduct(ab, ab);
    if (lengthSquared <= 0)
        return QLineF(point, a).length();
    const double t = std::clamp(QPointF::dotProduct(point - a, ab) / lengthSquared, 0.0, 1.0);
    return QLineF(point, a + ab * t).length();
}

// Angle of a point around a centre, degrees counter-clockwise on screen from +X, in [0, 360).
double screenAngle(QPointF center, QPointF point)
{
    double angle = std::atan2(-(point.y() - center.y()), point.x() - center.x()) * 180 / std::numbers::pi;
    if (angle < 0)
        angle += 360;
    return angle;
}

QPointF pointAtAngle(QPointF center, double radius, double degrees)
{
    const double radians = degrees * std::numbers::pi / 180;
    return center + QPointF(std::cos(radians) * radius, -std::sin(radians) * radius);
}

bool angleInSpan(double angle, double start, double span)
{
    if (std::abs(span) >= 360)
        return true;
    if (span >= 0)
        return std::fmod(angle - start + 720, 360) <= span;
    return std::fmod(start - angle + 720, 360) <= -span;
}

// Span from start to end, counter-clockwise, in (0, 360].
double positiveSpan(double start, double end)
{
    double span = std::fmod(end - start + 720, 360);
    return span <= 0 ? 360 : span;
}

void translateGraphic(SymbolGraphic &graphic, QPoint delta)
{
    for (QPoint &point : graphic.points)
        point += delta;
    graphic.from += delta;
    graphic.to += delta;
    graphic.center += delta;
    graphic.at += delta;
}

} // namespace

SymbolCanvas::SymbolCanvas(QWidget *parent) : QGraphicsView(parent)
{
    auto *scene = new QGraphicsScene(this);
    scene->setSceneRect(-2000, -2000, 4000, 4000);
    setScene(scene);
    setRenderHint(QPainter::Antialiasing);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setDragMode(QGraphicsView::NoDrag);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);
    setMinimumSize(200, 160);
    scale(4, 4);
    centerOn(0, 0);
}

void SymbolCanvas::setDefinition(const ComponentDefinition &definition)
{
    m_definition = definition;
    if (m_drag == Drag::Move || m_drag == Drag::Handle)
        m_drag = Drag::None;
    const auto before = m_selection;
    pruneSelection();
    viewport()->update();
    if (before != m_selection)
        Q_EMIT selectionChanged();
}

void SymbolCanvas::setTool(Tool tool)
{
    cancelCreation();
    if (m_tool == tool)
        return;
    m_tool = tool;
    viewport()->setCursor(tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    viewport()->update();
    Q_EMIT toolChanged(tool);
}

void SymbolCanvas::setGraphicsGrid(int grid)
{
    m_graphicsGrid = grid == 1 || grid == 5 || grid == 10 ? grid : 5;
    viewport()->update();
}

void SymbolCanvas::setSelection(const QList<ElementRef> &selection)
{
    if (selection == m_selection)
        return;
    m_selection = selection;
    pruneSelection();
    viewport()->update();
    Q_EMIT selectionChanged();
}

void SymbolCanvas::selectAll()
{
    QList<ElementRef> all;
    for (int i = 0; i < m_definition.symbol.graphics.size(); ++i)
        all.append(ElementRef{ElementRef::Kind::Graphic, i});
    for (int i = 0; i < m_definition.symbol.pins.size(); ++i)
        all.append(ElementRef{ElementRef::Kind::Pin, i});
    setSelection(all);
}

void SymbolCanvas::pruneSelection()
{
    QList<ElementRef> kept;
    for (const ElementRef &element : std::as_const(m_selection)) {
        const bool valid = (element.kind == ElementRef::Kind::Graphic && element.index >= 0
                            && element.index < m_definition.symbol.graphics.size())
                           || (element.kind == ElementRef::Kind::Pin && element.index >= 0
                               && element.index < m_definition.symbol.pins.size())
                           || element.kind == ElementRef::Kind::Reference || element.kind == ElementRef::Kind::Value;
        if (valid && !kept.contains(element))
            kept.append(element);
    }
    m_selection = kept;
}

void SymbolCanvas::selectOnly(const ElementRef &element)
{
    setSelection({element});
}

void SymbolCanvas::commit(const ComponentDefinition &definition, const QString &description)
{
    if (definition == m_definition && !(m_drag == Drag::Move || m_drag == Drag::Handle))
        return;
    m_definition = definition;
    viewport()->update();
    Q_EMIT definitionEdited(definition, description);
}

bool SymbolCanvas::selectionHasPins() const
{
    return std::any_of(m_selection.begin(), m_selection.end(), [](const ElementRef &element) {
        return element.kind != ElementRef::Kind::Graphic;
    });
}

void SymbolCanvas::deleteSelection()
{
    ComponentDefinition next = m_definition;
    QList<int> graphics, pins;
    for (const ElementRef &element : std::as_const(m_selection)) {
        if (element.kind == ElementRef::Kind::Graphic)
            graphics.append(element.index);
        else if (element.kind == ElementRef::Kind::Pin)
            pins.append(element.index);
    }
    if (graphics.isEmpty() && pins.isEmpty())
        return;
    std::sort(graphics.begin(), graphics.end(), std::greater<>());
    std::sort(pins.begin(), pins.end(), std::greater<>());
    for (int index : std::as_const(graphics))
        next.symbol.graphics.removeAt(index);
    for (int index : std::as_const(pins)) {
        // The pin list and the symbol stay in sync: a deleted symbol pin is removed from the component.
        next.pins.removeAll(next.symbol.pins.at(index).pin);
        next.symbol.pins.removeAt(index);
    }
    m_selection.clear();
    commit(next, tr("Delete"));
    Q_EMIT selectionChanged();
}

void SymbolCanvas::duplicateSelection()
{
    ComponentDefinition next = m_definition;
    QList<ElementRef> copies;
    const QPoint offset(10, 10);
    for (const ElementRef &element : std::as_const(m_selection)) {
        if (element.kind == ElementRef::Kind::Graphic) {
            SymbolGraphic graphic = m_definition.symbol.graphics.at(element.index);
            translateGraphic(graphic, offset);
            next.symbol.graphics.append(graphic);
            copies.append(ElementRef{ElementRef::Kind::Graphic, static_cast<int>(next.symbol.graphics.size() - 1)});
        } else if (element.kind == ElementRef::Kind::Pin) {
            SymbolPin pin = m_definition.symbol.pins.at(element.index);
            pin.pin = newPinName(next);
            pin.at += offset;
            next.pins.append(pin.pin);
            next.symbol.pins.append(pin);
            copies.append(ElementRef{ElementRef::Kind::Pin, static_cast<int>(next.symbol.pins.size() - 1)});
        }
    }
    if (copies.isEmpty())
        return;
    commit(next, tr("Duplicate"));
    setSelection(copies);
}

void SymbolCanvas::moveSelection(QPoint delta)
{
    if (m_selection.isEmpty() || delta.isNull())
        return;
    ComponentDefinition next = m_definition;
    for (const ElementRef &element : std::as_const(m_selection)) {
        switch (element.kind) {
        case ElementRef::Kind::Graphic: translateGraphic(next.symbol.graphics[element.index], delta); break;
        case ElementRef::Kind::Pin: next.symbol.pins[element.index].at += delta; break;
        case ElementRef::Kind::Reference: next.symbol.referenceAt += delta; break;
        case ElementRef::Kind::Value: next.symbol.valueAt += delta; break;
        }
    }
    commit(next, tr("Move"));
}

QString SymbolCanvas::newPinName(const ComponentDefinition &definition)
{
    QStringList used = definition.pins;
    for (const SymbolPin &pin : definition.symbol.pins)
        used.append(pin.pin);
    bool numbered = !used.isEmpty();
    for (const QString &name : std::as_const(used)) {
        bool ok = false;
        name.toInt(&ok);
        numbered = numbered && ok;
    }
    for (int n = static_cast<int>(definition.pins.size()) + 1;; ++n) {
        const QString candidate = numbered ? QString::number(n) : QStringLiteral("p%1").arg(n);
        if (!used.contains(candidate))
            return candidate;
    }
}

QRectF SymbolCanvas::elementBounds(const ComponentDefinition &definition, const ElementRef &element)
{
    switch (element.kind) {
    case ElementRef::Kind::Graphic: {
        if (element.index < 0 || element.index >= definition.symbol.graphics.size())
            return {};
        Symbol single;
        single.graphics.append(definition.symbol.graphics.at(element.index));
        return QRectF(single.boundingRect());
    }
    case ElementRef::Kind::Pin: {
        if (element.index < 0 || element.index >= definition.symbol.pins.size())
            return {};
        const SymbolPin &pin = definition.symbol.pins.at(element.index);
        return QRectF(QPointF(pin.at), pinEnd(pin)).normalized().adjusted(-1, -1, 1, 1);
    }
    case ElementRef::Kind::Reference:
        return QRectF(QPointF(definition.symbol.referenceAt) - QPointF(8, 4), QSizeF(16, 8));
    case ElementRef::Kind::Value:
        return QRectF(QPointF(definition.symbol.valueAt) - QPointF(12, 4), QSizeF(24, 8));
    }
    return {};
}

double SymbolCanvas::zoom() const
{
    return transform().m11();
}

double SymbolCanvas::tolerance() const
{
    return 5.0 / std::max(zoom(), 0.01);
}

QPoint SymbolCanvas::snap(QPointF point, int grid) const
{
    return QPoint(qRound(point.x() / grid) * grid, qRound(point.y() / grid) * grid);
}

QPoint SymbolCanvas::mapFromSymbol(QPointF point) const
{
    return mapFromScene(point);
}

QPointF SymbolCanvas::mapToSymbol(QPoint viewportPosition) const
{
    return mapToScene(viewportPosition);
}

void SymbolCanvas::zoomToFit()
{
    if (viewport()->width() < 20 || viewport()->height() < 20) {
        m_fitPending = true;
        return;
    }
    QRectF bounds = SymbolPainter::boundingRect(m_definition.symbol);
    bounds = bounds.united(elementBounds(m_definition, ElementRef{ElementRef::Kind::Reference, 0}));
    bounds = bounds.united(elementBounds(m_definition, ElementRef{ElementRef::Kind::Value, 0}));
    bounds = bounds.united(QRectF(-20, -20, 40, 40));
    bounds.adjust(-20, -20, 20, 20);
    const double scaleX = viewport()->width() / bounds.width();
    const double scaleY = viewport()->height() / bounds.height();
    const double target = std::clamp(std::min(scaleX, scaleY), minimumZoom, 12.0);
    setTransform(QTransform::fromScale(target, target));
    centerOn(bounds.center());
    m_fitPending = true; // keep fitting on resize until the user zooms or pans
    Q_EMIT zoomChanged(zoom());
}

void SymbolCanvas::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    if (m_fitPending)
        zoomToFit();
}

// --- painting ---

void SymbolCanvas::drawBackground(QPainter *painter, const QRectF &rect)
{
    const SchematicStyle style = styleFor(this);
    painter->fillRect(rect, style.background);

    const double pixelsPerUnit = zoom();
    const auto drawGrid = [&](int spacing, const QColor &color) {
        if (spacing * pixelsPerUnit < 5)
            return;
        QPen pen(color, 0);
        painter->setPen(pen);
        const double left = std::floor(rect.left() / spacing) * spacing;
        const double top = std::floor(rect.top() / spacing) * spacing;
        QVector<QLineF> lines;
        for (double x = left; x <= rect.right(); x += spacing)
            lines.append(QLineF(x, rect.top(), x, rect.bottom()));
        for (double y = top; y <= rect.bottom(); y += spacing)
            lines.append(QLineF(rect.left(), y, rect.right(), y));
        painter->drawLines(lines);
    };
    QColor minor = style.grid;
    minor.setAlphaF(0.35f);
    QColor major = style.grid;
    major.setAlphaF(0.9f);
    painter->setRenderHint(QPainter::Antialiasing, false);
    if (m_graphicsGrid < 10)
        drawGrid(m_graphicsGrid, minor);
    drawGrid(10, major);

    // Origin cross.
    QPen originPen(style.reference, 0);
    painter->setPen(originPen);
    painter->drawLine(QPointF(-15, 0), QPointF(15, 0));
    painter->drawLine(QPointF(0, -15), QPointF(0, 15));
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->drawEllipse(QPointF(0, 0), 3, 3);
}

void SymbolCanvas::drawForeground(QPainter *painter, const QRectF &)
{
    const SchematicStyle style = styleFor(this);
    const SymbolPainter symbolPainter(style);
    painter->setRenderHint(QPainter::Antialiasing);

    SymbolPainter::Options options;
    options.showPinNames = true;
    symbolPainter.paint(*painter, m_definition.symbol, options);

    // Connection points.
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(style.pin, 0));
    for (const SymbolPin &pin : m_definition.symbol.pins)
        painter->drawEllipse(QPointF(pin.at), 1.8, 1.8);

    // Reference and value markers.
    const auto drawMarker = [&](QPoint at, const QString &text, const QColor &color) {
        painter->setPen(QPen(color, 0, Qt::DotLine));
        painter->drawLine(QPointF(at) - QPointF(3, 0), QPointF(at) + QPointF(3, 0));
        painter->drawLine(QPointF(at) - QPointF(0, 3), QPointF(at) + QPointF(0, 3));
        symbolPainter.paintText(*painter, at, text, 7, TextAnchor::Center, color, Orientation{});
    };
    const bool referenceSelected = m_selection.contains(ElementRef{ElementRef::Kind::Reference, 0});
    const bool valueSelected = m_selection.contains(ElementRef{ElementRef::Kind::Value, 0});
    drawMarker(m_definition.symbol.referenceAt,
               m_definition.referencePrefix.isEmpty() ? QStringLiteral("REF") : m_definition.referencePrefix + QStringLiteral("?"),
               referenceSelected ? style.selection : style.reference);
    drawMarker(m_definition.symbol.valueAt, tr("value"), valueSelected ? style.selection : style.value);

    // Selected elements drawn over the symbol in the selection colour.
    Symbol selected;
    for (const ElementRef &element : std::as_const(m_selection)) {
        if (element.kind == ElementRef::Kind::Graphic)
            selected.graphics.append(m_definition.symbol.graphics.at(element.index));
        else if (element.kind == ElementRef::Kind::Pin)
            selected.pins.append(m_definition.symbol.pins.at(element.index));
    }
    if (!selected.graphics.isEmpty() || !selected.pins.isEmpty()) {
        SymbolPainter::Options selectedOptions;
        selectedOptions.selected = true;
        symbolPainter.paint(*painter, selected, selectedOptions);
    }

    // Handles of a single selected element.
    const double handle = handleSize / std::max(zoom(), 0.01);
    painter->setPen(QPen(style.selection, 0));
    painter->setBrush(style.background);
    for (const QPointF &point : handles())
        painter->drawRect(QRectF(point - QPointF(handle / 2, handle / 2), QSizeF(handle, handle)));

    // Shape being created.
    if (const auto preview = creationPreview()) {
        Symbol shape;
        shape.graphics.append(*preview);
        SymbolPainter::Options previewOptions;
        previewOptions.selected = true;
        symbolPainter.paint(*painter, shape, previewOptions);
    }
    if (m_tool != Tool::Select && underMouse()) {
        // Snap cursor.
        const QPoint snapped = snap(m_cursor, m_tool == Tool::Pin ? 10 : m_graphicsGrid);
        painter->setPen(QPen(style.selection, 0));
        painter->setBrush(Qt::NoBrush);
        const double size = 4 / std::max(zoom(), 0.01) + 1;
        painter->drawLine(QPointF(snapped) - QPointF(size, 0), QPointF(snapped) + QPointF(size, 0));
        painter->drawLine(QPointF(snapped) - QPointF(0, size), QPointF(snapped) + QPointF(0, size));
        if (m_tool == Tool::Pin) {
            Symbol pinPreview;
            SymbolPin pin;
            pin.at = snapped;
            pin.pin = newPinName(m_definition);
            const QPointF body = m_definition.symbol.graphics.isEmpty()
                                     ? QPointF(0, 0)
                                     : QRectF(m_definition.symbol.boundingRect()).center();
            const QPointF toward = body - QPointF(snapped);
            if (std::abs(toward.x()) >= std::abs(toward.y()))
                pin.direction = toward.x() >= 0 ? PinDirection::Right : PinDirection::Left;
            else
                pin.direction = toward.y() >= 0 ? PinDirection::Down : PinDirection::Up;
            pinPreview.pins.append(pin);
            SymbolPainter::Options previewOptions;
            previewOptions.selected = true;
            previewOptions.showPinNames = true;
            painter->setOpacity(0.6);
            symbolPainter.paint(*painter, pinPreview, previewOptions);
            painter->setOpacity(1);
        }
    }

    // Rubber band.
    if (m_drag == Drag::Band) {
        QColor fill = style.selection;
        fill.setAlphaF(0.12f);
        painter->setBrush(fill);
        painter->setPen(QPen(style.selection, 0, Qt::DashLine));
        painter->drawRect(m_band.normalized());
    }
}

QList<QPointF> SymbolCanvas::handles() const
{
    if (m_selection.size() != 1 || m_tool != Tool::Select)
        return {};
    const ElementRef &element = m_selection.first();
    QList<QPointF> result;
    if (element.kind == ElementRef::Kind::Pin) {
        result.append(pinEnd(m_definition.symbol.pins.at(element.index)));
        return result;
    }
    if (element.kind != ElementRef::Kind::Graphic)
        return result;
    const SymbolGraphic &graphic = m_definition.symbol.graphics.at(element.index);
    switch (graphic.type) {
    case SymbolGraphic::Type::Line:
    case SymbolGraphic::Type::Polygon:
        for (const QPoint &point : graphic.points)
            result.append(point);
        break;
    case SymbolGraphic::Type::Rect:
        result << QPointF(graphic.from) << QPointF(graphic.to) << QPointF(graphic.to.x(), graphic.from.y())
               << QPointF(graphic.from.x(), graphic.to.y());
        break;
    case SymbolGraphic::Type::Circle:
        result << QPointF(graphic.center) + QPointF(graphic.radius, 0);
        break;
    case SymbolGraphic::Type::Arc:
        result << pointAtAngle(graphic.center, graphic.radius, graphic.startAngle)
               << pointAtAngle(graphic.center, graphic.radius, graphic.startAngle + graphic.spanAngle);
        break;
    case SymbolGraphic::Type::Text:
        break;
    }
    return result;
}

int SymbolCanvas::hitHandle(QPointF point) const
{
    const QList<QPointF> points = handles();
    const double reach = handleSize / std::max(zoom(), 0.01);
    for (int i = 0; i < points.size(); ++i) {
        if (std::abs(points.at(i).x() - point.x()) <= reach && std::abs(points.at(i).y() - point.y()) <= reach)
            return i;
    }
    return -1;
}

std::optional<ElementRef> SymbolCanvas::hitTest(QPointF point) const
{
    const double reach = tolerance();
    const Symbol &symbol = m_definition.symbol;
    for (const ElementRef::Kind kind : {ElementRef::Kind::Reference, ElementRef::Kind::Value}) {
        const ElementRef marker{kind, 0};
        if (elementBounds(m_definition, marker).adjusted(-reach / 2, -reach / 2, reach / 2, reach / 2).contains(point))
            return marker;
    }
    for (int i = static_cast<int>(symbol.pins.size()) - 1; i >= 0; --i) {
        const SymbolPin &pin = symbol.pins.at(i);
        if (distanceToSegment(point, pin.at, pinEnd(pin)) <= reach || QLineF(point, pin.at).length() <= reach * 1.5)
            return ElementRef{ElementRef::Kind::Pin, i};
    }
    for (int i = static_cast<int>(symbol.graphics.size()) - 1; i >= 0; --i) {
        const SymbolGraphic &graphic = symbol.graphics.at(i);
        bool hit = false;
        switch (graphic.type) {
        case SymbolGraphic::Type::Line:
        case SymbolGraphic::Type::Polygon: {
            const qsizetype count = graphic.points.size();
            for (qsizetype k = 0; k + 1 < count && !hit; ++k)
                hit = distanceToSegment(point, graphic.points.at(k), graphic.points.at(k + 1)) <= reach;
            if (graphic.type == SymbolGraphic::Type::Polygon && count > 2) {
                hit = hit || distanceToSegment(point, graphic.points.last(), graphic.points.first()) <= reach;
                if (graphic.fill != Fill::None) {
                    QPolygonF polygon;
                    for (const QPoint &p : graphic.points)
                        polygon.append(p);
                    hit = hit || polygon.containsPoint(point, Qt::OddEvenFill);
                }
            }
            break;
        }
        case SymbolGraphic::Type::Rect: {
            const QRectF rect = QRectF(QPointF(graphic.from), QPointF(graphic.to)).normalized();
            const QRectF outer = rect.adjusted(-reach, -reach, reach, reach);
            const QRectF inner = rect.adjusted(reach, reach, -reach, -reach);
            hit = outer.contains(point) && (graphic.fill != Fill::None || !inner.contains(point));
            break;
        }
        case SymbolGraphic::Type::Circle: {
            const double distance = QLineF(point, graphic.center).length();
            hit = std::abs(distance - graphic.radius) <= reach || (graphic.fill != Fill::None && distance <= graphic.radius);
            break;
        }
        case SymbolGraphic::Type::Arc: {
            const double distance = QLineF(point, graphic.center).length();
            hit = std::abs(distance - graphic.radius) <= reach
                  && angleInSpan(screenAngle(graphic.center, point), graphic.startAngle, graphic.spanAngle);
            break;
        }
        case SymbolGraphic::Type::Text:
            hit = elementBounds(m_definition, ElementRef{ElementRef::Kind::Graphic, i})
                      .adjusted(-reach / 2, -reach / 2, reach / 2, reach / 2)
                      .contains(point);
            break;
        }
        if (hit)
            return ElementRef{ElementRef::Kind::Graphic, i};
    }
    return std::nullopt;
}

// --- creation ---

void SymbolCanvas::cancelCreation()
{
    const bool had = !m_points.isEmpty() || m_drag == Drag::Create;
    m_points.clear();
    if (m_drag == Drag::Create)
        m_drag = Drag::None;
    if (had)
        viewport()->update();
}

std::optional<SymbolGraphic> SymbolCanvas::creationPreview() const
{
    if (m_points.isEmpty())
        return std::nullopt;
    const QPoint cursor = snap(m_cursor, m_graphicsGrid);
    SymbolGraphic graphic;
    switch (m_tool) {
    case Tool::Line:
    case Tool::Polygon:
        graphic.type = SymbolGraphic::Type::Line;
        graphic.points = m_points;
        graphic.points.append(cursor);
        if (m_tool == Tool::Polygon && graphic.points.size() >= 3)
            graphic.points.append(m_points.first());
        return graphic;
    case Tool::Rect:
        graphic.type = SymbolGraphic::Type::Rect;
        graphic.from = m_points.first();
        graphic.to = cursor;
        return graphic;
    case Tool::Circle:
        graphic.type = SymbolGraphic::Type::Circle;
        graphic.center = m_points.first();
        graphic.radius = std::max(1, qRound(QLineF(graphic.center, cursor).length()));
        return graphic;
    case Tool::Arc: {
        graphic.center = m_points.first();
        if (m_points.size() == 1) {
            graphic.type = SymbolGraphic::Type::Circle; // radius guide
            graphic.radius = std::max(1, qRound(QLineF(graphic.center, cursor).length()));
            return graphic;
        }
        graphic.type = SymbolGraphic::Type::Arc;
        graphic.radius = std::max(1, qRound(QLineF(graphic.center, m_points.at(1)).length()));
        graphic.startAngle = std::round(screenAngle(graphic.center, m_points.at(1)));
        graphic.spanAngle = std::round(positiveSpan(graphic.startAngle, screenAngle(graphic.center, cursor)));
        return graphic;
    }
    default:
        return std::nullopt;
    }
}

void SymbolCanvas::addCreationPoint(QPoint point)
{
    switch (m_tool) {
    case Tool::Line:
    case Tool::Polygon:
        if (m_tool == Tool::Polygon && m_points.size() >= 3 && point == m_points.first()) {
            finishCreation();
            return;
        }
        if (m_points.isEmpty() || m_points.last() != point)
            m_points.append(point);
        break;
    case Tool::Rect:
    case Tool::Circle:
        m_points.append(point);
        if (m_points.size() == 2)
            finishCreation();
        break;
    case Tool::Arc:
        if (m_points.size() == 1 && point == m_points.first())
            break; // the start point cannot be the centre
        m_points.append(point);
        if (m_points.size() == 3)
            finishCreation();
        break;
    default:
        break;
    }
    viewport()->update();
}

void SymbolCanvas::finishCreation()
{
    SymbolGraphic graphic;
    bool valid = false;
    switch (m_tool) {
    case Tool::Line:
        graphic.type = SymbolGraphic::Type::Line;
        graphic.points = m_points;
        valid = graphic.points.size() >= 2;
        break;
    case Tool::Polygon:
        graphic.type = SymbolGraphic::Type::Polygon;
        graphic.points = m_points;
        valid = graphic.points.size() >= 3;
        break;
    case Tool::Rect:
        graphic.type = SymbolGraphic::Type::Rect;
        if (m_points.size() == 2) {
            graphic.from = QPoint(std::min(m_points[0].x(), m_points[1].x()), std::min(m_points[0].y(), m_points[1].y()));
            graphic.to = QPoint(std::max(m_points[0].x(), m_points[1].x()), std::max(m_points[0].y(), m_points[1].y()));
            valid = graphic.from.x() != graphic.to.x() && graphic.from.y() != graphic.to.y();
        }
        break;
    case Tool::Circle:
        graphic.type = SymbolGraphic::Type::Circle;
        if (m_points.size() == 2) {
            graphic.center = m_points[0];
            graphic.radius = qRound(QLineF(m_points[0], m_points[1]).length());
            valid = graphic.radius > 0;
        }
        break;
    case Tool::Arc:
        graphic.type = SymbolGraphic::Type::Arc;
        if (m_points.size() == 3) {
            graphic.center = m_points[0];
            graphic.radius = qRound(QLineF(m_points[0], m_points[1]).length());
            graphic.startAngle = std::round(screenAngle(graphic.center, m_points[1]));
            graphic.spanAngle = std::round(positiveSpan(graphic.startAngle, screenAngle(graphic.center, m_points[2])));
            valid = graphic.radius > 0;
        }
        break;
    default:
        break;
    }
    m_points.clear();
    m_drag = Drag::None;
    if (!valid) {
        viewport()->update();
        return;
    }
    ComponentDefinition next = m_definition;
    next.symbol.graphics.append(graphic);
    const ElementRef created{ElementRef::Kind::Graphic, static_cast<int>(next.symbol.graphics.size() - 1)};
    commit(next, tr("Draw"));
    selectOnly(created);
    Q_EMIT elementCreated(created);
}

void SymbolCanvas::createPin(QPoint at)
{
    for (int i = 0; i < m_definition.symbol.pins.size(); ++i) {
        if (m_definition.symbol.pins.at(i).at == at) {
            selectOnly(ElementRef{ElementRef::Kind::Pin, i});
            return;
        }
    }
    ComponentDefinition next = m_definition;
    SymbolPin pin;
    pin.pin = newPinName(next);
    pin.at = at;
    pin.length = 10;
    // Point toward the body, so the pin line grows inward from the connection point.
    const QPointF body = next.symbol.graphics.isEmpty() ? QPointF(0, 0) : QRectF(next.symbol.boundingRect()).center();
    const QPointF toward = body - QPointF(at);
    if (std::abs(toward.x()) >= std::abs(toward.y()))
        pin.direction = toward.x() >= 0 ? PinDirection::Right : PinDirection::Left;
    else
        pin.direction = toward.y() >= 0 ? PinDirection::Down : PinDirection::Up;
    if (!next.pins.contains(pin.pin))
        next.pins.append(pin.pin);
    next.symbol.pins.append(pin);
    const ElementRef created{ElementRef::Kind::Pin, static_cast<int>(next.symbol.pins.size() - 1)};
    commit(next, tr("Add pin"));
    selectOnly(created);
    Q_EMIT elementCreated(created);
}

// --- mouse and keyboard ---

void SymbolCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    const QPointF point = mapToScene(event->position().toPoint());
    m_cursor = point;
    m_pressPoint = point;

    if (event->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        m_fitPending = false;
        m_lastPanPosition = event->position().toPoint();
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (m_tool != Tool::Select) {
        if (event->button() == Qt::RightButton) {
            // Right click finishes a polyline or polygon, otherwise cancels.
            if ((m_tool == Tool::Line && m_points.size() >= 2) || (m_tool == Tool::Polygon && m_points.size() >= 3))
                finishCreation();
            else if (!m_points.isEmpty())
                cancelCreation();
            else
                setTool(Tool::Select);
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        if (m_tool == Tool::Pin) {
            createPin(snap(point, 10));
            return;
        }
        const QPoint snapped = snap(point, m_graphicsGrid);
        if (m_tool == Tool::Text) {
            ComponentDefinition next = m_definition;
            SymbolGraphic text;
            text.type = SymbolGraphic::Type::Text;
            text.at = snapped;
            text.text = tr("Text");
            text.size = 8;
            next.symbol.graphics.append(text);
            const ElementRef created{ElementRef::Kind::Graphic, static_cast<int>(next.symbol.graphics.size() - 1)};
            commit(next, tr("Add text"));
            setTool(Tool::Select);
            selectOnly(created);
            Q_EMIT elementCreated(created);
            return;
        }
        addCreationPoint(snapped);
        if ((m_tool == Tool::Rect || m_tool == Tool::Circle) && m_points.size() == 1)
            m_drag = Drag::Create; // press-drag-release, or click twice
        return;
    }

    if (event->button() != Qt::LeftButton)
        return;

    const int handle = hitHandle(point);
    if (handle >= 0) {
        m_drag = Drag::Handle;
        m_handle = handle;
        m_dragStart = m_definition;
        m_moved = false;
        return;
    }

    const bool toggle = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    const auto hit = hitTest(point);
    if (hit) {
        if (toggle) {
            QList<ElementRef> selection = m_selection;
            if (selection.contains(*hit))
                selection.removeAll(*hit);
            else
                selection.append(*hit);
            setSelection(selection);
            return;
        }
        if (!m_selection.contains(*hit))
            selectOnly(*hit);
        m_drag = Drag::Move;
        m_dragStart = m_definition;
        m_moved = false;
        return;
    }
    if (!toggle && !m_selection.isEmpty())
        setSelection({});
    m_drag = Drag::Band;
    m_band = QRectF(point, point);
}

void SymbolCanvas::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF point = mapToScene(event->position().toPoint());
    m_cursor = point;
    Q_EMIT cursorMoved(point);

    switch (m_drag) {
    case Drag::Pan: {
        const QPoint delta = event->position().toPoint() - m_lastPanPosition;
        m_lastPanPosition = event->position().toPoint();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        return;
    }
    case Drag::Move: {
        const int grid = selectionHasPins() ? 10 : m_graphicsGrid;
        const QPoint delta = snap(point, grid) - snap(m_pressPoint, grid);
        ComponentDefinition next = m_dragStart;
        for (const ElementRef &element : std::as_const(m_selection)) {
            switch (element.kind) {
            case ElementRef::Kind::Graphic: translateGraphic(next.symbol.graphics[element.index], delta); break;
            case ElementRef::Kind::Pin: next.symbol.pins[element.index].at += delta; break;
            case ElementRef::Kind::Reference: next.symbol.referenceAt += delta; break;
            case ElementRef::Kind::Value: next.symbol.valueAt += delta; break;
            }
        }
        m_moved = !delta.isNull();
        m_definition = next;
        viewport()->update();
        return;
    }
    case Drag::Handle: {
        if (m_selection.size() != 1)
            return;
        const ElementRef element = m_selection.first();
        ComponentDefinition next = m_dragStart;
        const QPoint snapped = snap(point, m_graphicsGrid);
        if (element.kind == ElementRef::Kind::Pin) {
            SymbolPin &pin = next.symbol.pins[element.index];
            const QPoint offset = snapped - pin.at;
            if (std::abs(offset.x()) >= std::abs(offset.y())) {
                pin.direction = offset.x() >= 0 ? PinDirection::Right : PinDirection::Left;
                pin.length = std::abs(offset.x());
            } else {
                pin.direction = offset.y() >= 0 ? PinDirection::Down : PinDirection::Up;
                pin.length = std::abs(offset.y());
            }
        } else if (element.kind == ElementRef::Kind::Graphic) {
            SymbolGraphic &graphic = next.symbol.graphics[element.index];
            switch (graphic.type) {
            case SymbolGraphic::Type::Line:
            case SymbolGraphic::Type::Polygon:
                graphic.points[m_handle] = snapped;
                break;
            case SymbolGraphic::Type::Rect:
                if (m_handle == 0) {
                    graphic.from = snapped;
                } else if (m_handle == 1) {
                    graphic.to = snapped;
                } else if (m_handle == 2) {
                    graphic.to.setX(snapped.x());
                    graphic.from.setY(snapped.y());
                } else {
                    graphic.from.setX(snapped.x());
                    graphic.to.setY(snapped.y());
                }
                break;
            case SymbolGraphic::Type::Circle:
                graphic.radius = std::max(1, qRound(QLineF(graphic.center, snapped).length()));
                break;
            case SymbolGraphic::Type::Arc:
                if (m_handle == 0) {
                    const double end = graphic.startAngle + graphic.spanAngle;
                    graphic.radius = std::max(1, qRound(QLineF(graphic.center, snapped).length()));
                    graphic.startAngle = std::round(screenAngle(graphic.center, snapped));
                    graphic.spanAngle = std::round(positiveSpan(graphic.startAngle, end));
                } else {
                    graphic.spanAngle = std::round(positiveSpan(graphic.startAngle, screenAngle(graphic.center, snapped)));
                }
                break;
            case SymbolGraphic::Type::Text:
                break;
            }
        }
        m_moved = !(next == m_dragStart);
        m_definition = next;
        viewport()->update();
        return;
    }
    case Drag::Band:
        m_band.setBottomRight(point);
        viewport()->update();
        return;
    case Drag::Create:
    case Drag::None:
        break;
    }
    if (m_tool != Tool::Select)
        viewport()->update();
}

void SymbolCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    const QPointF point = mapToScene(event->position().toPoint());
    switch (m_drag) {
    case Drag::Pan:
        m_drag = Drag::None;
        viewport()->setCursor(m_tool == Tool::Select ? Qt::ArrowCursor : Qt::CrossCursor);
        return;
    case Drag::Move:
    case Drag::Handle: {
        const bool moved = m_moved;
        const ComponentDefinition result = m_definition;
        const QString description = m_drag == Drag::Move ? tr("Move") : tr("Reshape");
        m_definition = m_dragStart;
        m_drag = Drag::None;
        if (moved)
            commit(result, description);
        else
            viewport()->update();
        return;
    }
    case Drag::Band: {
        m_drag = Drag::None;
        const QRectF band = m_band.normalized();
        QList<ElementRef> selection = (event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) ? m_selection
                                                                                                         : QList<ElementRef>();
        if (band.width() > tolerance() || band.height() > tolerance()) {
            const auto consider = [&](const ElementRef &element) {
                if (band.contains(elementBounds(m_definition, element)) && !selection.contains(element))
                    selection.append(element);
            };
            for (int i = 0; i < m_definition.symbol.graphics.size(); ++i)
                consider(ElementRef{ElementRef::Kind::Graphic, i});
            for (int i = 0; i < m_definition.symbol.pins.size(); ++i)
                consider(ElementRef{ElementRef::Kind::Pin, i});
            consider(ElementRef{ElementRef::Kind::Reference, 0});
            consider(ElementRef{ElementRef::Kind::Value, 0});
        }
        setSelection(selection);
        viewport()->update();
        return;
    }
    case Drag::Create: {
        // Rect and circle: releasing away from the press point completes the shape.
        const QPoint snapped = snap(point, m_graphicsGrid);
        if (!m_points.isEmpty() && snapped != m_points.first())
            addCreationPoint(snapped);
        else
            m_drag = Drag::None; // a click: wait for the second click
        return;
    }
    case Drag::None:
        break;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void SymbolCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && (m_tool == Tool::Line || m_tool == Tool::Polygon)) {
        // The first click of the double click already added the point.
        finishCreation();
        return;
    }
    if (event->button() == Qt::LeftButton && m_tool == Tool::Select)
        return;
    mousePressEvent(event);
}

void SymbolCanvas::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        if (!m_points.isEmpty())
            cancelCreation();
        else if (m_tool != Tool::Select)
            setTool(Tool::Select);
        else
            setSelection({});
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_tool == Tool::Line || m_tool == Tool::Polygon) {
            finishCreation();
            return;
        }
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (m_tool == Tool::Line || m_tool == Tool::Polygon) {
            if (!m_points.isEmpty()) {
                m_points.removeLast();
                viewport()->update();
            }
            return;
        }
        deleteSelection();
        return;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down: {
        if (m_selection.isEmpty())
            break;
        const int step = selectionHasPins() ? 10 : m_graphicsGrid;
        QPoint delta;
        if (event->key() == Qt::Key_Left)
            delta = QPoint(-step, 0);
        else if (event->key() == Qt::Key_Right)
            delta = QPoint(step, 0);
        else if (event->key() == Qt::Key_Up)
            delta = QPoint(0, -step);
        else
            delta = QPoint(0, step);
        moveSelection(delta);
        return;
    }
    default:
        break;
    }
    QGraphicsView::keyPressEvent(event);
}

void SymbolCanvas::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    double factor = std::pow(1.2, steps);
    const double target = std::clamp(zoom() * factor, minimumZoom, maximumZoom);
    factor = target / zoom();
    scale(factor, factor);
    m_fitPending = false;
    Q_EMIT zoomChanged(zoom());
    event->accept();
}

} // namespace lvd

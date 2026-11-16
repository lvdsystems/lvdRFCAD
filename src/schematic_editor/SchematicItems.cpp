#include "schematic_editor/SchematicItems.h"

#include "core/Connectivity.h"
#include "core/Expression.h"
#include "schematic_editor/SchematicScene.h"

#include <QPainter>
#include <QPainterPathStroker>
#include <QStyleOptionGraphicsItem>

namespace lvd {

namespace {

constexpr double HitWidth = 6.0;   // wire hit area, drawing units
constexpr double HaloMargin = 3.0; // hover and rule-check halo

// Drawn in place of a symbol whose definition cannot be resolved.
const Symbol &placeholderSymbol()
{
    static const Symbol symbol = [] {
        Symbol s;
        SymbolGraphic box;
        box.type = SymbolGraphic::Type::Rect;
        box.from = QPoint(-20, -20);
        box.to = QPoint(20, 20);
        s.graphics.append(box);
        SymbolGraphic mark;
        mark.type = SymbolGraphic::Type::Text;
        mark.at = QPoint(0, 0);
        mark.text = QStringLiteral("?");
        mark.size = 16;
        mark.anchor = TextAnchor::Center;
        s.graphics.append(mark);
        s.referenceAt = QPoint(0, -30);
        s.valueAt = QPoint(0, 30);
        return s;
    }();
    return symbol;
}

QString displayUnit(const QString &unit)
{
    if (unit.compare(QStringLiteral("ohm"), Qt::CaseInsensitive) == 0)
        return QString(QChar(0x03A9)); // Ω
    return unit;
}

// The value text of a shown parameter: as typed, with the unit appended to a
// bare number ("15" in dB shows as "15 dB").
QString shownValue(const ParameterDefinition &parameter, const QString &text)
{
    if (!parameter.isExpression() || parameter.unit.isEmpty())
        return text;
    const bool bare = std::none_of(text.begin(), text.end(), [](QChar c) { return c.isLetter(); });
    if (bare && parseNumber(text))
        return text.trimmed() + QLatin1Char(' ') + displayUnit(parameter.unit);
    return text;
}

// Anchor for text placed at `at`, on the side of the body it lies on.
TextAnchor anchorFor(QPointF at, const QRectF &body)
{
    const QPointF offset = at - body.center();
    const bool beside = std::abs(offset.x()) > std::abs(offset.y()) || std::abs(offset.x()) > body.width() / 2;
    if (!beside)
        return TextAnchor::Center;
    return offset.x() > 0 ? TextAnchor::Left : TextAnchor::Right;
}

QRectF textRect(QPointF at, QSizeF size, TextAnchor anchor)
{
    double x = at.x();
    if (anchor == TextAnchor::Center)
        x -= size.width() / 2;
    else if (anchor == TextAnchor::Right)
        x -= size.width();
    return QRectF(x, at.y() - size.height() / 2, size.width(), size.height());
}

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

} // namespace

// SchematicItem

SchematicItem::SchematicItem(const QString &id, ItemKind kind) : m_id(id), m_kind(kind)
{
    setFlag(ItemIsSelectable, !id.isEmpty());
}

void SchematicItem::setGhost(bool ghost)
{
    m_ghost = ghost;
    setFlag(ItemIsSelectable, !ghost && !m_id.isEmpty());
    setOpacity(ghost ? 0.6 : 1.0);
    setZValue(ghost ? 100 : zValue());
}

void SchematicItem::setHovered(bool hovered)
{
    if (m_hovered != hovered) {
        m_hovered = hovered;
        update();
    }
}

void SchematicItem::setHighlighted(bool highlighted)
{
    if (m_highlighted != highlighted) {
        m_highlighted = highlighted;
        update();
    }
}

void SchematicItem::setMark(std::optional<RuleViolation::Severity> severity)
{
    if (m_mark != severity) {
        m_mark = severity;
        update();
    }
}

void SchematicItem::setPreviewOffset(QPoint offset)
{
    setPos(anchor() + offset);
}

void SchematicItem::resetPreview()
{
    setPos(anchor());
}

SchematicScene *SchematicItem::schematicScene() const
{
    return static_cast<SchematicScene *>(scene());
}

const SchematicStyle &SchematicItem::style() const
{
    return schematicScene()->style();
}

void SchematicItem::paintDecorations(QPainter *painter, const QRectF &rect) const
{
    if (!m_mark && !m_hovered)
        return;
    const SchematicStyle &s = style();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (m_mark) {
        const QColor color = *m_mark == RuleViolation::Severity::Error ? s.error : s.warning;
        painter->setPen(QPen(color, 1.0));
        painter->setBrush(withAlpha(color, 50));
        painter->drawRoundedRect(rect, 3, 3);
    }
    if (m_hovered) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(withAlpha(s.selection, 45));
        painter->drawRoundedRect(rect, 3, 3);
    }
    painter->restore();
}

// ComponentItem

ComponentItem::ComponentItem(const PlacedComponent &component, const Resolution &resolution, SchematicScene *scene)
    : SchematicItem(component.id, ItemKind::Component), m_owner(scene)
{
    setZValue(1);
    // Symbols are the most expensive items to draw; panning reuses the cache.
    setCacheMode(DeviceCoordinateCache);
    setComponent(component, resolution);
}

const Symbol &ComponentItem::symbol() const
{
    return m_resolution.definition ? m_resolution.definition->symbol : placeholderSymbol();
}

void ComponentItem::setComponent(const PlacedComponent &component, const Resolution &resolution)
{
    prepareGeometryChange();
    m_component = component;
    m_resolution = resolution;
    m_transform = symbolTransform(QPointF(), component.orientation());
    m_body = m_transform.mapRect(SymbolPainter::boundingRect(symbol()));
    if (!m_resolution.definition)
        m_unconnected.clear();
    layoutTexts();
    setPos(component.at);
    update();
}

void ComponentItem::layoutTexts()
{
    m_texts.clear();
    const SchematicStyle &s = m_owner->style();
    const Symbol &sym = symbol();
    const ComponentDefinition *definition = m_resolution.definition.get();

    // Power symbols such as ground carry no visible reference.
    if (!definition || definition->netName.isEmpty()) {
        const QPointF at = m_transform.map(QPointF(sym.referenceAt));
        m_texts.append(TextLine{at, m_component.reference, anchorFor(at, m_body), s.referenceTextSize, true});
    }

    QStringList values;
    if (!definition) {
        values.append(m_component.definition);
    } else {
        QList<const ParameterDefinition *> shown;
        for (const ParameterDefinition &parameter : definition->parameters) {
            if (parameter.shown)
                shown.append(&parameter);
        }
        for (const ParameterDefinition *parameter : shown) {
            const QString text = parameterText(m_component, *definition, parameter->name).trimmed();
            if (text.isEmpty())
                continue;
            QString line = shownValue(*parameter, text);
            if (shown.size() > 1 && parameter->isExpression())
                line = parameter->name + QLatin1Char('=') + line;
            values.append(line);
        }
    }
    if (!values.isEmpty()) {
        const QPointF at = m_transform.map(QPointF(sym.valueAt));
        const TextAnchor anchor = anchorFor(at, m_body);
        const double step = s.valueTextSize * 1.3;
        // Above the body the lines grow upward, elsewhere downward.
        const bool above = anchor == TextAnchor::Center && at.y() < m_body.center().y();
        for (qsizetype i = 0; i < values.size(); ++i) {
            const double offset = above ? -step * double(values.size() - 1 - i) : step * double(i);
            m_texts.append(TextLine{at + QPointF(0, offset), values.at(i), anchor, s.valueTextSize, false});
        }
    }

    // Some symbols put reference and value side by side, which collides once
    // rotated; then the value lines are stacked under the reference instead
    // (or the reference above them when the text is above the body).
    if (m_texts.size() > 1 && m_texts.first().reference) {
        const TextLine reference = m_texts.first();
        const auto rectOf = [this](const TextLine &line) {
            return textRect(line.at, m_owner->textSize(line.text, line.size), line.anchor);
        };
        QRectF valueRect;
        for (qsizetype i = 1; i < m_texts.size(); ++i)
            valueRect = valueRect.united(rectOf(m_texts.at(i)));
        if (rectOf(reference).adjusted(-1, -1, 1, 1).intersects(valueRect)) {
            const double step = s.valueTextSize * 1.3;
            const qsizetype count = m_texts.size() - 1;
            const bool above = reference.anchor == TextAnchor::Center && reference.at.y() < m_body.center().y();
            for (qsizetype i = 1; i < m_texts.size(); ++i) {
                TextLine &line = m_texts[i];
                line.anchor = reference.anchor;
                line.at = reference.at + QPointF(0, step * double(above ? i - count : i));
            }
            if (above)
                m_texts.first().at = reference.at - QPointF(0, step * double(count));
        }
    }

    m_bounds = m_body;
    for (const TextLine &line : m_texts)
        m_bounds = m_bounds.united(textRect(line.at, m_owner->textSize(line.text, line.size), line.anchor));
    m_bounds.adjust(-HaloMargin - 6, -HaloMargin - 6, HaloMargin + 6, HaloMargin + 6); // badge and halo
}

QList<QPair<QString, QPoint>> ComponentItem::pins() const
{
    QList<QPair<QString, QPoint>> result;
    if (m_resolution.definition) {
        for (const SymbolPin &pin : m_resolution.definition->symbol.pins)
            result.append({pin.pin, pinPosition(m_component, pin)});
    }
    return result;
}

QList<QPoint> ComponentItem::pinPositions() const
{
    if (!m_resolution.definition)
        return {};
    return lvd::pinPositions(m_component, *m_resolution.definition);
}

QString ComponentItem::pinAt(QPoint at) const
{
    if (m_resolution.definition) {
        for (const SymbolPin &pin : m_resolution.definition->symbol.pins) {
            if (pinPosition(m_component, pin) == at)
                return pin.pin;
        }
    }
    return {};
}

void ComponentItem::setUnconnectedPins(const QSet<QString> &pins)
{
    if (m_unconnected != pins) {
        m_unconnected = pins;
        update();
    }
}

void ComponentItem::setHighlightedPins(const QSet<QString> &pins)
{
    if (m_highlightedPins != pins) {
        m_highlightedPins = pins;
        update();
    }
}

void ComponentItem::resetPreview()
{
    setPos(m_component.at);
}

QRectF ComponentItem::boundingRect() const
{
    return m_bounds;
}

QPainterPath ComponentItem::shape() const
{
    QPainterPath path;
    path.addRect(m_body);
    for (const TextLine &line : m_texts) {
        if (line.reference)
            path.addRect(textRect(line.at, m_owner->textSize(line.text, line.size), line.anchor));
    }
    return path;
}

void ComponentItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *)
{
    const SchematicStyle &s = style();
    paintDecorations(painter, m_body.adjusted(-HaloMargin, -HaloMargin, HaloMargin, HaloMargin));

    const bool missing = !m_resolution.definition;
    painter->save();
    painter->setTransform(m_transform, true);
    SymbolPainter::Options options;
    options.orientation = m_component.orientation();
    options.selected = drawSelected();
    options.missing = missing;
    options.unconnectedPins = m_unconnected;
    m_owner->symbolPainter().paint(*painter, symbol(), options);
    painter->restore();

    if (!m_highlightedPins.isEmpty()) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(s.highlight);
        for (const auto &[name, at] : pins()) {
            if (m_highlightedPins.contains(name))
                painter->drawEllipse(QPointF(at - m_component.at), s.junctionRadius, s.junctionRadius);
        }
        painter->restore();
    }

    // Text is skipped when it would be too small to read.
    const double lod = option->levelOfDetailFromTransform(painter->worldTransform());
    if (lod * s.valueTextSize >= 3.0) {
        for (const TextLine &line : m_texts) {
            QColor color = line.reference ? s.reference : s.value;
            if (missing)
                color = s.missing;
            else if (drawSelected())
                color = s.selection;
            m_owner->symbolPainter().paintText(*painter, line.at, line.text, line.size, line.anchor, color,
                                               Orientation{});
        }
    }

    // Badge for parts drawn from the cache or changed in the library.
    if (m_resolution.source == Resolution::Source::Cache || m_resolution.changedInLibrary) {
        const QPointF centre = m_body.topRight() + QPointF(1, -1);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(s.background, 0.8));
        painter->setBrush(s.warning);
        painter->drawEllipse(centre, 4.5, 4.5);
        painter->restore();
        const QString letter = m_resolution.source == Resolution::Source::Cache ? QStringLiteral("C")
                                                                                : QStringLiteral("!");
        m_owner->symbolPainter().paintText(*painter, centre, letter, 6, TextAnchor::Center, s.background,
                                           Orientation{});
    }
}

// WireItem

WireItem::WireItem(const Wire &wire) : SchematicItem(wire.id, ItemKind::Wire)
{
    setZValue(2);
    setWire(wire);
}

void WireItem::setWire(const Wire &wire)
{
    m_wire = wire;
    setPos(0, 0);
    setGeometry(wire.points);
}

void WireItem::setPreviewPoints(const QList<QPoint> &points)
{
    setPos(0, 0);
    setGeometry(points);
}

void WireItem::resetPreview()
{
    setPos(0, 0);
    if (m_shown != m_wire.points)
        setGeometry(m_wire.points);
}

void WireItem::setGeometry(const QList<QPoint> &points)
{
    prepareGeometryChange();
    m_shown = points;
    m_path = QPainterPath();
    if (!points.isEmpty()) {
        m_path.moveTo(points.first());
        for (qsizetype i = 1; i < points.size(); ++i)
            m_path.lineTo(points.at(i));
    }
    QPainterPathStroker stroker;
    stroker.setWidth(HitWidth);
    stroker.setCapStyle(Qt::RoundCap);
    m_shape = stroker.createStroke(m_path);
    update();
}

QRectF WireItem::boundingRect() const
{
    return m_path.boundingRect().adjusted(-HitWidth, -HitWidth, HitWidth, HitWidth);
}

QPainterPath WireItem::shape() const
{
    return m_shape;
}

void WireItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    const SchematicStyle &s = style();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(Qt::NoBrush);
    if (m_mark || m_hovered) {
        QColor halo = m_mark ? (*m_mark == RuleViolation::Severity::Error ? s.error : s.warning)
                             : withAlpha(s.selection, 70);
        if (m_mark)
            halo.setAlpha(110);
        QPen pen(halo, s.wireWidth + 4);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter->setPen(pen);
        painter->drawPath(m_path);
    }
    QColor color = s.wire;
    if (drawSelected())
        color = s.selection;
    else if (m_highlighted)
        color = s.highlight;
    QPen pen(color, m_highlighted ? s.wireWidth * 1.6 : s.wireWidth);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->drawPath(m_path);
}

// JunctionItem

JunctionItem::JunctionItem(const Junction &junction) : SchematicItem(junction.id, ItemKind::Junction)
{
    setZValue(3);
    setJunction(junction);
}

void JunctionItem::setJunction(const Junction &junction)
{
    m_junction = junction;
    setPos(junction.at);
    update();
}

QRectF JunctionItem::boundingRect() const
{
    const double r = 3.0 + HaloMargin + 1;
    return QRectF(-r, -r, 2 * r, 2 * r);
}

void JunctionItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    const SchematicStyle &s = style();
    const double r = s.junctionRadius;
    paintDecorations(painter, QRectF(-r - 2, -r - 2, 2 * r + 4, 2 * r + 4));
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(drawSelected() ? s.selection : (m_highlighted ? s.highlight : s.junction));
    painter->drawEllipse(QPointF(0, 0), r, r);
}

// LabelItem

namespace {
constexpr double LabelTextSize = 10;
const QPointF LabelTextOffset(3, -7); // text sits just above the wire
} // namespace

LabelItem::LabelItem(const NetLabel &label, SchematicScene *scene)
    : SchematicItem(label.id, ItemKind::Label), m_owner(scene)
{
    setZValue(4);
    setLabel(label);
}

void LabelItem::setLabel(const NetLabel &label)
{
    prepareGeometryChange();
    m_label = label;
    m_textRect = textRect(LabelTextOffset, m_owner->textSize(label.net, LabelTextSize), TextAnchor::Left);
    setPos(label.at);
    update();
}

QRectF LabelItem::boundingRect() const
{
    return m_textRect.united(QRectF(-3, -3, 6, 6)).adjusted(-HaloMargin - 1, -HaloMargin - 1, HaloMargin + 1,
                                                            HaloMargin + 1);
}

QPainterPath LabelItem::shape() const
{
    QPainterPath path;
    path.addRect(m_textRect.united(QRectF(-3, -3, 6, 6)));
    return path;
}

void LabelItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    const SchematicStyle &s = style();
    paintDecorations(painter, m_textRect.united(QRectF(-3, -3, 6, 6)).adjusted(-1, -1, 1, 1));
    const QColor color = drawSelected() ? s.selection : (m_highlighted ? s.highlight : s.label);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    // A short flag from the connection point up to the text.
    painter->setPen(QPen(color, 1.0));
    painter->drawLine(QPointF(0, 0), QPointF(0, -3));
    painter->drawLine(QPointF(0, -3), QPointF(2, -3));
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawRect(QRectF(-1.5, -1.5, 3, 3));
    painter->restore();
    m_owner->symbolPainter().paintText(*painter, LabelTextOffset, m_label.net, LabelTextSize, TextAnchor::Left, color,
                                       Orientation{});
}

// TextItem

TextItem::TextItem(const TextNote &text, SchematicScene *scene) : SchematicItem(text.id, ItemKind::Text), m_owner(scene)
{
    setZValue(4);
    setText(text);
}

void TextItem::setText(const TextNote &text)
{
    prepareGeometryChange();
    m_text = text;
    m_rect = QRectF();
    const QStringList lines = text.text.split(QLatin1Char('\n'));
    const double step = text.size * 1.3;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QSizeF size = m_owner->textSize(lines.at(i).isEmpty() ? QStringLiteral(" ") : lines.at(i), text.size);
        m_rect = m_rect.united(textRect(QPointF(0, step * double(i)), size, TextAnchor::Left));
    }
    setPos(text.at);
    update();
}

QRectF TextItem::boundingRect() const
{
    return m_rect.adjusted(-HaloMargin - 1, -HaloMargin - 1, HaloMargin + 1, HaloMargin + 1);
}

void TextItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    const SchematicStyle &s = style();
    paintDecorations(painter, m_rect.adjusted(-1, -1, 1, 1));
    const QColor color = drawSelected() ? s.selection : s.text;
    const QStringList lines = m_text.text.split(QLatin1Char('\n'));
    const double step = m_text.size * 1.3;
    for (qsizetype i = 0; i < lines.size(); ++i)
        m_owner->symbolPainter().paintText(*painter, QPointF(0, step * double(i)), lines.at(i), m_text.size,
                                           TextAnchor::Left, color, Orientation{});
}

} // namespace lvd

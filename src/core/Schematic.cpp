#include "core/Schematic.h"

#include "core/JsonFile.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

namespace lvd {

namespace {

const QString VariablesId = QStringLiteral("$variables");
const QString SimulationId = QStringLiteral("$simulation");

QJsonArray pointJson(QPoint point)
{
    return QJsonArray{point.x(), point.y()};
}

std::optional<QPoint> readPoint(const QJsonValue &value)
{
    const QJsonArray array = value.toArray();
    if (!value.isArray() || array.size() != 2 || !array.at(0).isDouble() || !array.at(1).isDouble())
        return std::nullopt;
    return QPoint(array.at(0).toInt(), array.at(1).toInt());
}

QList<QPoint> cleanPoints(const QList<QPoint> &points)
{
    QList<QPoint> result;
    for (const QPoint &point : points) {
        if (result.isEmpty() || result.last() != point)
            result.append(point);
    }
    return result;
}

QPoint rotateAbout(QPoint point, QPoint center, int degrees)
{
    return center + Orientation{degrees, false}.map(point - center);
}

QPoint mirrorAbout(QPoint point, QPoint center)
{
    return QPoint(2 * center.x() - point.x(), point.y());
}

QJsonObject junctionToJson(const Junction &junction)
{
    return QJsonObject{{QStringLiteral("id"), junction.id}, {QStringLiteral("at"), pointJson(junction.at)}};
}

QJsonObject labelToJson(const NetLabel &label)
{
    return QJsonObject{{QStringLiteral("id"), label.id},
                       {QStringLiteral("net"), label.net},
                       {QStringLiteral("at"), pointJson(label.at)}};
}

QJsonObject textToJson(const TextNote &text)
{
    return QJsonObject{{QStringLiteral("id"), text.id},
                       {QStringLiteral("text"), text.text},
                       {QStringLiteral("at"), pointJson(text.at)},
                       {QStringLiteral("size"), text.size}};
}

QJsonArray variablesToJson(const QList<Variable> &variables)
{
    QJsonArray array;
    for (const Variable &variable : variables)
        array.append(QJsonObject{{QStringLiteral("name"), variable.name}, {QStringLiteral("value"), variable.value}});
    return array;
}

// Item parsers: return false with *error set on malformed input.

bool readComponent(const QJsonObject &object, PlacedComponent *component, QString *error)
{
    component->id = object.value(QStringLiteral("id")).toString();
    component->reference = object.value(QStringLiteral("reference")).toString();
    component->definition = object.value(QStringLiteral("definition")).toString();
    const auto at = readPoint(object.value(QStringLiteral("at")));
    component->rotation = normalizedRotation(object.value(QStringLiteral("rotation")).toInt(0));
    component->mirror = object.value(QStringLiteral("mirror")).toBool(false);
    component->parameters.clear();
    const QJsonObject parameters = object.value(QStringLiteral("parameters")).toObject();
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
        component->parameters.insert(it.key(), it.value().toString());
    if (component->definition.isEmpty() || !at) {
        setError(error, QStringLiteral("component \"%1\" needs \"definition\" and \"at\"").arg(component->id));
        return false;
    }
    component->at = *at;
    return true;
}

bool readWire(const QJsonObject &object, Wire *wire, QString *error)
{
    wire->id = object.value(QStringLiteral("id")).toString();
    wire->points.clear();
    for (const QJsonValue &value : object.value(QStringLiteral("points")).toArray()) {
        const auto point = readPoint(value);
        if (!point) {
            setError(error, QStringLiteral("wire \"%1\" has an invalid point").arg(wire->id));
            return false;
        }
        wire->points.append(*point);
    }
    wire->points = cleanPoints(wire->points);
    if (wire->points.size() < 2) {
        setError(error, QStringLiteral("wire \"%1\" needs at least two distinct points").arg(wire->id));
        return false;
    }
    return true;
}

bool readJunction(const QJsonObject &object, Junction *junction, QString *error)
{
    junction->id = object.value(QStringLiteral("id")).toString();
    const auto at = readPoint(object.value(QStringLiteral("at")));
    if (!at) {
        setError(error, QStringLiteral("junction \"%1\" needs \"at\"").arg(junction->id));
        return false;
    }
    junction->at = *at;
    return true;
}

bool readLabel(const QJsonObject &object, NetLabel *label, QString *error)
{
    label->id = object.value(QStringLiteral("id")).toString();
    label->net = object.value(QStringLiteral("net")).toString();
    const auto at = readPoint(object.value(QStringLiteral("at")));
    if (!at || label->net.isEmpty()) {
        setError(error, QStringLiteral("label \"%1\" needs \"net\" and \"at\"").arg(label->id));
        return false;
    }
    label->at = *at;
    return true;
}

bool readText(const QJsonObject &object, TextNote *text, QString *error)
{
    text->id = object.value(QStringLiteral("id")).toString();
    text->text = object.value(QStringLiteral("text")).toString();
    text->size = object.value(QStringLiteral("size")).toInt(12);
    const auto at = readPoint(object.value(QStringLiteral("at")));
    if (!at) {
        setError(error, QStringLiteral("text \"%1\" needs \"at\"").arg(text->id));
        return false;
    }
    text->at = *at;
    return true;
}

QList<Variable> readVariables(const QJsonArray &array)
{
    QList<Variable> variables;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        variables.append(Variable{object.value(QStringLiteral("name")).toString(),
                                  object.value(QStringLiteral("value")).toString()});
    }
    return variables;
}

int idNumber(const QString &id)
{
    bool ok = false;
    const int number = id.mid(1).toInt(&ok);
    return ok ? number : 0;
}

// Sorts ids of one kind by their number, so that c2 comes before c10.
QStringList sortedIds(QStringList ids)
{
    std::sort(ids.begin(), ids.end(), [](const QString &a, const QString &b) {
        const int na = idNumber(a), nb = idNumber(b);
        return na != nb ? na < nb : a < b;
    });
    return ids;
}

} // namespace

QJsonObject componentToJson(const PlacedComponent &component)
{
    QJsonObject object{{QStringLiteral("id"), component.id},
                       {QStringLiteral("reference"), component.reference},
                       {QStringLiteral("definition"), component.definition},
                       {QStringLiteral("at"), pointJson(component.at)},
                       {QStringLiteral("rotation"), component.rotation},
                       {QStringLiteral("mirror"), component.mirror}};
    QJsonObject parameters;
    for (auto it = component.parameters.begin(); it != component.parameters.end(); ++it)
        parameters.insert(it.key(), it.value());
    object.insert(QStringLiteral("parameters"), parameters);
    return object;
}

QJsonObject wireToJson(const Wire &wire)
{
    QJsonArray points;
    for (const QPoint &point : wire.points)
        points.append(pointJson(point));
    return QJsonObject{{QStringLiteral("id"), wire.id}, {QStringLiteral("points"), points}};
}

Schematic::Schematic(QObject *parent) : QObject(parent), m_undo(new UndoStack(this))
{
}

ItemKind Schematic::kindOf(const QString &id)
{
    if (id.size() < 2)
        return ItemKind::None;
    switch (id.at(0).unicode()) {
    case u'c': return ItemKind::Component;
    case u'w': return ItemKind::Wire;
    case u'j': return ItemKind::Junction;
    case u'l': return ItemKind::Label;
    case u't': return ItemKind::Text;
    default: return ItemKind::None;
    }
}

std::unique_ptr<Schematic> Schematic::fromJson(const QJsonObject &content, QString *error)
{
    auto schematic = std::make_unique<Schematic>();
    int highestId = 0;
    QSet<QString> ids;

    const auto accept = [&](const QString &id, ItemKind kind) {
        if (kindOf(id) != kind || ids.contains(id)) {
            setError(error, QStringLiteral("invalid or duplicate item id \"%1\"").arg(id));
            return false;
        }
        ids.insert(id);
        highestId = std::max(highestId, idNumber(id));
        return true;
    };

    for (const QJsonValue &value : content.value(QStringLiteral("components")).toArray()) {
        PlacedComponent component;
        if (!readComponent(value.toObject(), &component, error) || !accept(component.id, ItemKind::Component))
            return nullptr;
        schematic->m_components.insert(component.id, component);
    }
    for (const QJsonValue &value : content.value(QStringLiteral("wires")).toArray()) {
        Wire wire;
        if (!readWire(value.toObject(), &wire, error) || !accept(wire.id, ItemKind::Wire))
            return nullptr;
        schematic->m_wires.insert(wire.id, wire);
    }
    for (const QJsonValue &value : content.value(QStringLiteral("junctions")).toArray()) {
        Junction junction;
        if (!readJunction(value.toObject(), &junction, error) || !accept(junction.id, ItemKind::Junction))
            return nullptr;
        schematic->m_junctions.insert(junction.id, junction);
    }
    for (const QJsonValue &value : content.value(QStringLiteral("labels")).toArray()) {
        NetLabel label;
        if (!readLabel(value.toObject(), &label, error) || !accept(label.id, ItemKind::Label))
            return nullptr;
        schematic->m_labels.insert(label.id, label);
    }
    for (const QJsonValue &value : content.value(QStringLiteral("texts")).toArray()) {
        TextNote text;
        if (!readText(value.toObject(), &text, error) || !accept(text.id, ItemKind::Text))
            return nullptr;
        schematic->m_texts.insert(text.id, text);
    }
    schematic->m_variables = readVariables(content.value(QStringLiteral("variables")).toArray());
    schematic->m_simulation = content.value(QStringLiteral("simulation")).toObject();

    const QJsonObject definitions = content.value(QStringLiteral("definitions")).toObject();
    for (auto it = definitions.begin(); it != definitions.end(); ++it) {
        // An unreadable cache entry is dropped; the library copy may still resolve.
        if (auto definition = ComponentDefinition::fromJson(it.value().toObject()))
            schematic->m_cache.insert(it.key(), std::make_shared<const ComponentDefinition>(std::move(*definition)));
    }

    schematic->m_nextId = std::max(content.value(QStringLiteral("nextId")).toInt(1), highestId + 1);
    return schematic;
}

QJsonObject Schematic::toJson() const
{
    QJsonObject content;
    content.insert(QStringLiteral("nextId"), m_nextId);

    QJsonObject definitions;
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
        definitions.insert(it.key(), it.value()->toJson());
    content.insert(QStringLiteral("definitions"), definitions);
    content.insert(QStringLiteral("variables"), variablesToJson(m_variables));
    if (!m_simulation.isEmpty())
        content.insert(QStringLiteral("simulation"), m_simulation);

    QJsonArray components, wires, junctions, labels, texts;
    for (const QString &id : sortedIds(m_components.keys()))
        components.append(componentToJson(m_components.value(id)));
    for (const QString &id : sortedIds(m_wires.keys()))
        wires.append(wireToJson(m_wires.value(id)));
    for (const QString &id : sortedIds(m_junctions.keys()))
        junctions.append(junctionToJson(m_junctions.value(id)));
    for (const QString &id : sortedIds(m_labels.keys()))
        labels.append(labelToJson(m_labels.value(id)));
    for (const QString &id : sortedIds(m_texts.keys()))
        texts.append(textToJson(m_texts.value(id)));
    content.insert(QStringLiteral("components"), components);
    content.insert(QStringLiteral("wires"), wires);
    content.insert(QStringLiteral("junctions"), junctions);
    content.insert(QStringLiteral("labels"), labels);
    content.insert(QStringLiteral("texts"), texts);
    return content;
}

const PlacedComponent *Schematic::component(const QString &id) const
{
    const auto it = m_components.constFind(id);
    return it == m_components.cend() ? nullptr : &it.value();
}

const PlacedComponent *Schematic::componentByReference(const QString &reference) const
{
    for (const PlacedComponent &component : m_components) {
        if (component.reference == reference)
            return &component;
    }
    return nullptr;
}

bool Schematic::contains(const QString &id) const
{
    return snapshot(id).has_value();
}

QStringList Schematic::itemIds() const
{
    return sortedIds(m_components.keys()) + sortedIds(m_wires.keys()) + sortedIds(m_junctions.keys())
           + sortedIds(m_labels.keys()) + sortedIds(m_texts.keys());
}

void Schematic::setCachedDefinition(const QString &key, DefinitionPtr definition)
{
    if (definition)
        m_cache.insert(key, std::move(definition));
}

void Schematic::pruneCache()
{
    QSet<QString> used;
    for (const PlacedComponent &component : m_components)
        used.insert(component.definition);
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (used.contains(it.key()))
            ++it;
        else
            it = m_cache.erase(it);
    }
}

void Schematic::beginEdit(const QString &description)
{
    if (m_editDepth++ == 0) {
        m_editDescription = description;
        m_before.clear();
    }
}

void Schematic::endEdit()
{
    Q_ASSERT(m_editDepth > 0);
    if (m_editDepth <= 0 || --m_editDepth > 0)
        return;

    QMap<QString, Snapshot> before, after;
    for (auto it = m_before.begin(); it != m_before.end(); ++it) {
        const Snapshot now = snapshot(it.key());
        if (now != it.value()) {
            before.insert(it.key(), it.value());
            after.insert(it.key(), now);
        }
    }
    m_before.clear();
    if (before.isEmpty())
        return;

    const auto restore = [this](const QMap<QString, Snapshot> &states) {
        for (auto it = states.begin(); it != states.end(); ++it)
            apply(it.key(), it.value());
        Q_EMIT changed();
    };
    m_undo->push(UndoStack::Command{m_editDescription, [restore, before] { restore(before); },
                                    [restore, after] { restore(after); }});
    Q_EMIT changed();
}

QString Schematic::newId(QChar prefix)
{
    return QString(prefix) + QString::number(m_nextId++);
}

void Schematic::touch(const QString &id)
{
    Q_ASSERT(m_editDepth > 0);
    if (!m_before.contains(id))
        m_before.insert(id, snapshot(id));
}

Schematic::Snapshot Schematic::snapshot(const QString &id) const
{
    if (id == VariablesId)
        return QJsonObject{{QStringLiteral("variables"), variablesToJson(m_variables)}};
    if (id == SimulationId)
        return QJsonObject{{QStringLiteral("simulation"), m_simulation}};
    switch (kindOf(id)) {
    case ItemKind::Component:
        if (const auto it = m_components.constFind(id); it != m_components.cend())
            return componentToJson(*it);
        break;
    case ItemKind::Wire:
        if (const auto it = m_wires.constFind(id); it != m_wires.cend())
            return wireToJson(*it);
        break;
    case ItemKind::Junction:
        if (const auto it = m_junctions.constFind(id); it != m_junctions.cend())
            return junctionToJson(*it);
        break;
    case ItemKind::Label:
        if (const auto it = m_labels.constFind(id); it != m_labels.cend())
            return labelToJson(*it);
        break;
    case ItemKind::Text:
        if (const auto it = m_texts.constFind(id); it != m_texts.cend())
            return textToJson(*it);
        break;
    case ItemKind::None:
        break;
    }
    return std::nullopt;
}

void Schematic::apply(const QString &id, const Snapshot &state)
{
    if (id == VariablesId) {
        m_variables = readVariables(state ? state->value(QStringLiteral("variables")).toArray() : QJsonArray());
        Q_EMIT variablesChanged();
        return;
    }
    if (id == SimulationId) {
        m_simulation = state ? state->value(QStringLiteral("simulation")).toObject() : QJsonObject();
        Q_EMIT simulationChanged();
        return;
    }
    const bool existed = contains(id);
    if (!state) {
        m_components.remove(id);
        m_wires.remove(id);
        m_junctions.remove(id);
        m_labels.remove(id);
        m_texts.remove(id);
        if (existed)
            Q_EMIT itemRemoved(id);
        return;
    }
    // Snapshots were produced by this class, so parsing cannot fail.
    switch (kindOf(id)) {
    case ItemKind::Component: {
        PlacedComponent component;
        readComponent(*state, &component, nullptr);
        m_components.insert(id, component);
        break;
    }
    case ItemKind::Wire: {
        Wire wire;
        readWire(*state, &wire, nullptr);
        m_wires.insert(id, wire);
        break;
    }
    case ItemKind::Junction: {
        Junction junction;
        readJunction(*state, &junction, nullptr);
        m_junctions.insert(id, junction);
        break;
    }
    case ItemKind::Label: {
        NetLabel label;
        readLabel(*state, &label, nullptr);
        m_labels.insert(id, label);
        break;
    }
    case ItemKind::Text: {
        TextNote text;
        readText(*state, &text, nullptr);
        m_texts.insert(id, text);
        break;
    }
    case ItemKind::None:
        return;
    }
    if (existed)
        Q_EMIT itemChanged(id);
    else
        Q_EMIT itemAdded(id);
}

QString Schematic::nextReference(const QString &prefix) const
{
    static const QRegularExpression digits(QStringLiteral("^\\d+$"));
    int highest = 0;
    for (const PlacedComponent &component : m_components) {
        if (!component.reference.startsWith(prefix))
            continue;
        const QString rest = component.reference.mid(prefix.size());
        if (digits.match(rest).hasMatch())
            highest = std::max(highest, rest.toInt());
    }
    return prefix + QString::number(highest + 1);
}

QString Schematic::addComponent(const QString &definitionKey, const ComponentDefinition *definition, QPoint at,
                                int rotation, bool mirror)
{
    Edit edit(this, tr("Place component"));
    PlacedComponent component;
    component.id = newId(QLatin1Char('c'));
    component.definition = definitionKey;
    component.reference = nextReference(definition ? definition->referencePrefix : QStringLiteral("U"));
    component.at = at;
    component.rotation = normalizedRotation(rotation);
    component.mirror = mirror;
    touch(component.id);
    apply(component.id, componentToJson(component));
    return component.id;
}

bool Schematic::updateComponent(const PlacedComponent &component)
{
    if (!m_components.contains(component.id))
        return false;
    Edit edit(this, tr("Edit component"));
    touch(component.id);
    PlacedComponent normalized = component;
    normalized.rotation = normalizedRotation(component.rotation);
    apply(component.id, componentToJson(normalized));
    return true;
}

bool Schematic::setParameter(const QString &componentId, const QString &name, const QString &value)
{
    const PlacedComponent *existing = component(componentId);
    if (!existing)
        return false;
    PlacedComponent changed = *existing;
    changed.parameters.insert(name, value);
    Edit edit(this, tr("Set %1").arg(name));
    return updateComponent(changed);
}

bool Schematic::resetParameter(const QString &componentId, const QString &name)
{
    const PlacedComponent *existing = component(componentId);
    if (!existing)
        return false;
    PlacedComponent changed = *existing;
    changed.parameters.remove(name);
    Edit edit(this, tr("Reset %1").arg(name));
    return updateComponent(changed);
}

bool Schematic::setReference(const QString &componentId, const QString &reference)
{
    const PlacedComponent *existing = component(componentId);
    if (!existing || reference.trimmed().isEmpty())
        return false;
    PlacedComponent changed = *existing;
    changed.reference = reference.trimmed();
    Edit edit(this, tr("Rename %1").arg(existing->reference));
    return updateComponent(changed);
}

QString Schematic::addWire(const QList<QPoint> &points)
{
    const QList<QPoint> cleaned = cleanPoints(points);
    if (cleaned.size() < 2)
        return {};
    Edit edit(this, tr("Add wire"));
    const Wire wire{newId(QLatin1Char('w')), cleaned};
    touch(wire.id);
    apply(wire.id, wireToJson(wire));
    return wire.id;
}

bool Schematic::updateWire(const Wire &wire)
{
    const QList<QPoint> cleaned = cleanPoints(wire.points);
    if (!m_wires.contains(wire.id) || cleaned.size() < 2)
        return false;
    Edit edit(this, tr("Edit wire"));
    touch(wire.id);
    apply(wire.id, wireToJson(Wire{wire.id, cleaned}));
    return true;
}

QString Schematic::addJunction(QPoint at)
{
    Edit edit(this, tr("Add junction"));
    const Junction junction{newId(QLatin1Char('j')), at};
    touch(junction.id);
    apply(junction.id, junctionToJson(junction));
    return junction.id;
}

QString Schematic::addLabel(const QString &net, QPoint at)
{
    if (net.trimmed().isEmpty())
        return {};
    Edit edit(this, tr("Add net label"));
    const NetLabel label{newId(QLatin1Char('l')), net.trimmed(), at};
    touch(label.id);
    apply(label.id, labelToJson(label));
    return label.id;
}

bool Schematic::updateLabel(const NetLabel &label)
{
    if (!m_labels.contains(label.id) || label.net.trimmed().isEmpty())
        return false;
    Edit edit(this, tr("Edit net label"));
    touch(label.id);
    apply(label.id, labelToJson(NetLabel{label.id, label.net.trimmed(), label.at}));
    return true;
}

QString Schematic::addText(const QString &text, QPoint at, int size)
{
    Edit edit(this, tr("Add text"));
    const TextNote note{newId(QLatin1Char('t')), text, at, size};
    touch(note.id);
    apply(note.id, textToJson(note));
    return note.id;
}

bool Schematic::updateText(const TextNote &text)
{
    if (!m_texts.contains(text.id))
        return false;
    Edit edit(this, tr("Edit text"));
    touch(text.id);
    apply(text.id, textToJson(text));
    return true;
}

bool Schematic::removeItem(const QString &id)
{
    if (!contains(id))
        return false;
    Edit edit(this, tr("Delete"));
    touch(id);
    apply(id, std::nullopt);
    return true;
}

void Schematic::removeItems(const QStringList &ids)
{
    Edit edit(this, tr("Delete"));
    for (const QString &id : ids)
        removeItem(id);
}

void Schematic::setVariables(const QList<Variable> &variables)
{
    Edit edit(this, tr("Edit variables"));
    touch(VariablesId);
    m_variables = variables;
    Q_EMIT variablesChanged();
}

void Schematic::setSimulation(const QJsonObject &simulation)
{
    Edit edit(this, tr("Edit simulation setup"));
    touch(SimulationId);
    m_simulation = simulation;
    Q_EMIT simulationChanged();
}

void Schematic::moveItems(const QStringList &ids, QPoint delta, const QHash<QString, QList<QPoint>> &pinPositions,
                          bool dragWires)
{
    if (delta.isNull() || ids.isEmpty())
        return;
    Edit edit(this, tr("Move"));
    const QSet<QString> moved(ids.begin(), ids.end());

    // Points that move; attached wire ends follow them.
    QSet<QPoint> movingPoints;
    if (dragWires) {
        for (const QString &id : ids) {
            for (const QPoint &point : pinPositions.value(id))
                movingPoints.insert(point);
            if (const auto it = m_wires.constFind(id); it != m_wires.cend()) {
                movingPoints.insert(it->points.first());
                movingPoints.insert(it->points.last());
            }
        }
    }

    for (const QString &id : ids) {
        switch (kindOf(id)) {
        case ItemKind::Component:
            if (const PlacedComponent *existing = component(id)) {
                PlacedComponent changed = *existing;
                changed.at += delta;
                updateComponent(changed);
            }
            break;
        case ItemKind::Wire:
            if (m_wires.contains(id)) {
                Wire wire = m_wires.value(id);
                for (QPoint &point : wire.points)
                    point += delta;
                updateWire(wire);
            }
            break;
        case ItemKind::Junction:
            if (m_junctions.contains(id)) {
                touch(id);
                Junction junction = m_junctions.value(id);
                junction.at += delta;
                apply(id, junctionToJson(junction));
            }
            break;
        case ItemKind::Label:
            if (m_labels.contains(id)) {
                NetLabel label = m_labels.value(id);
                label.at += delta;
                updateLabel(label);
            }
            break;
        case ItemKind::Text:
            if (m_texts.contains(id)) {
                TextNote text = m_texts.value(id);
                text.at += delta;
                updateText(text);
            }
            break;
        case ItemKind::None:
            break;
        }
    }

    if (!dragWires || movingPoints.isEmpty())
        return;

    // Stretch wires that are not moved themselves but end on a moving point.
    const QList<QString> wireIds = m_wires.keys();
    for (const QString &id : wireIds) {
        if (moved.contains(id))
            continue;
        Wire wire = m_wires.value(id);
        const bool moveFirst = movingPoints.contains(wire.points.first());
        const bool moveLast = movingPoints.contains(wire.points.last());
        if (!moveFirst && !moveLast)
            continue;
        if (moveFirst && moveLast) {
            for (QPoint &point : wire.points)
                point += delta;
            updateWire(wire);
            continue;
        }
        if (moveLast)
            std::reverse(wire.points.begin(), wire.points.end());

        // wire.points.first() moves by delta; keep the first segment orthogonal.
        const QPoint start = wire.points.at(0);
        const QPoint next = wire.points.at(1);
        const QPoint newStart = start + delta;
        if (wire.points.size() == 2) {
            if (start.y() == next.y() && delta.y() != 0)
                wire.points = {newStart, QPoint(newStart.x(), next.y()), next};
            else if (start.x() == next.x() && delta.x() != 0)
                wire.points = {newStart, QPoint(next.x(), newStart.y()), next};
            else
                wire.points[0] = newStart;
        } else {
            wire.points[0] = newStart;
            if (start.y() == next.y())
                wire.points[1].setY(next.y() + delta.y());
            else if (start.x() == next.x())
                wire.points[1].setX(next.x() + delta.x());
        }
        if (moveLast)
            std::reverse(wire.points.begin(), wire.points.end());
        if (cleanPoints(wire.points).size() >= 2)
            updateWire(wire);
        else
            removeItem(id);
    }
}

void Schematic::rotateItems(const QStringList &ids, QPoint center, int degrees)
{
    const int rotation = normalizedRotation(degrees);
    if (rotation == 0 || ids.isEmpty())
        return;
    Edit edit(this, tr("Rotate"));
    for (const QString &id : ids) {
        switch (kindOf(id)) {
        case ItemKind::Component:
            if (const PlacedComponent *existing = component(id)) {
                PlacedComponent changed = *existing;
                changed.at = rotateAbout(changed.at, center, rotation);
                changed.rotation = normalizedRotation(changed.rotation + rotation);
                updateComponent(changed);
            }
            break;
        case ItemKind::Wire:
            if (m_wires.contains(id)) {
                Wire wire = m_wires.value(id);
                for (QPoint &point : wire.points)
                    point = rotateAbout(point, center, rotation);
                updateWire(wire);
            }
            break;
        case ItemKind::Junction:
            if (m_junctions.contains(id)) {
                touch(id);
                Junction junction = m_junctions.value(id);
                junction.at = rotateAbout(junction.at, center, rotation);
                apply(id, junctionToJson(junction));
            }
            break;
        case ItemKind::Label:
            if (m_labels.contains(id)) {
                NetLabel label = m_labels.value(id);
                label.at = rotateAbout(label.at, center, rotation);
                updateLabel(label);
            }
            break;
        case ItemKind::Text:
            if (m_texts.contains(id)) {
                TextNote text = m_texts.value(id);
                text.at = rotateAbout(text.at, center, rotation);
                updateText(text);
            }
            break;
        case ItemKind::None:
            break;
        }
    }
}

void Schematic::mirrorItems(const QStringList &ids, QPoint center)
{
    if (ids.isEmpty())
        return;
    Edit edit(this, tr("Mirror"));
    for (const QString &id : ids) {
        switch (kindOf(id)) {
        case ItemKind::Component:
            if (const PlacedComponent *existing = component(id)) {
                // Mirror after rotation r equals rotation -r after the mirror.
                PlacedComponent changed = *existing;
                changed.at = mirrorAbout(changed.at, center);
                changed.rotation = normalizedRotation(360 - changed.rotation);
                changed.mirror = !changed.mirror;
                updateComponent(changed);
            }
            break;
        case ItemKind::Wire:
            if (m_wires.contains(id)) {
                Wire wire = m_wires.value(id);
                for (QPoint &point : wire.points)
                    point = mirrorAbout(point, center);
                updateWire(wire);
            }
            break;
        case ItemKind::Junction:
            if (m_junctions.contains(id)) {
                touch(id);
                Junction junction = m_junctions.value(id);
                junction.at = mirrorAbout(junction.at, center);
                apply(id, junctionToJson(junction));
            }
            break;
        case ItemKind::Label:
            if (m_labels.contains(id)) {
                NetLabel label = m_labels.value(id);
                label.at = mirrorAbout(label.at, center);
                updateLabel(label);
            }
            break;
        case ItemKind::Text:
            if (m_texts.contains(id)) {
                TextNote text = m_texts.value(id);
                text.at = mirrorAbout(text.at, center);
                updateText(text);
            }
            break;
        case ItemKind::None:
            break;
        }
    }
}

} // namespace lvd

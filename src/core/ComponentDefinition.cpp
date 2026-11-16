#include "core/ComponentDefinition.h"

#include "core/JsonFile.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace lvd {

namespace {

// Collects the first error while parsing; later reads become no-ops.
class Reader
{
public:
    explicit Reader(QString context) : m_context(std::move(context)) {}

    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }

    void fail(const QString &message)
    {
        if (m_error.isEmpty())
            m_error = m_context.isEmpty() ? message : m_context + QStringLiteral(": ") + message;
    }

    QString string(const QJsonObject &object, const char *key, bool required)
    {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull()) {
            if (required)
                fail(QStringLiteral("missing \"%1\"").arg(QLatin1String(key)));
            return {};
        }
        if (!value.isString()) {
            fail(QStringLiteral("\"%1\" must be text").arg(QLatin1String(key)));
            return {};
        }
        return value.toString();
    }

    int integer(const QJsonObject &object, const char *key, int defaultValue)
    {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined())
            return defaultValue;
        if (!value.isDouble() || value.toDouble() != std::floor(value.toDouble())) {
            fail(QStringLiteral("\"%1\" must be an integer").arg(QLatin1String(key)));
            return defaultValue;
        }
        return value.toInt();
    }

    double number(const QJsonObject &object, const char *key, double defaultValue)
    {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined())
            return defaultValue;
        if (!value.isDouble()) {
            fail(QStringLiteral("\"%1\" must be a number").arg(QLatin1String(key)));
            return defaultValue;
        }
        return value.toDouble();
    }

    bool boolean(const QJsonObject &object, const char *key, bool defaultValue)
    {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined())
            return defaultValue;
        if (!value.isBool()) {
            fail(QStringLiteral("\"%1\" must be true or false").arg(QLatin1String(key)));
            return defaultValue;
        }
        return value.toBool();
    }

    QPoint point(const QJsonValue &value, const QString &what)
    {
        const QJsonArray array = value.toArray();
        if (!value.isArray() || array.size() != 2 || !array.at(0).isDouble() || !array.at(1).isDouble()) {
            fail(QStringLiteral("%1 must be a point [x, y]").arg(what));
            return {};
        }
        return QPoint(array.at(0).toInt(), array.at(1).toInt());
    }

    QPoint point(const QJsonObject &object, const char *key, bool required)
    {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined()) {
            if (required)
                fail(QStringLiteral("missing \"%1\"").arg(QLatin1String(key)));
            return {};
        }
        return point(value, QStringLiteral("\"%1\"").arg(QLatin1String(key)));
    }

    QList<QPoint> points(const QJsonObject &object, const char *key, int minimum)
    {
        QList<QPoint> result;
        const QJsonValue value = object.value(QLatin1String(key));
        if (!value.isArray()) {
            fail(QStringLiteral("\"%1\" must be a list of points").arg(QLatin1String(key)));
            return result;
        }
        for (const QJsonValue &item : value.toArray())
            result.append(point(item, QStringLiteral("each entry of \"%1\"").arg(QLatin1String(key))));
        if (result.size() < minimum)
            fail(QStringLiteral("\"%1\" needs at least %2 points").arg(QLatin1String(key)).arg(minimum));
        return result;
    }

    QJsonArray array(const QJsonObject &object, const char *key, bool required)
    {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined()) {
            if (required)
                fail(QStringLiteral("missing \"%1\"").arg(QLatin1String(key)));
            return {};
        }
        if (!value.isArray()) {
            fail(QStringLiteral("\"%1\" must be a list").arg(QLatin1String(key)));
            return {};
        }
        return value.toArray();
    }

    template<typename Enum>
    Enum choice(const QJsonObject &object, const char *key, const QList<std::pair<const char *, Enum>> &options,
                Enum defaultValue, bool required = false)
    {
        const QString text = string(object, key, required);
        if (text.isEmpty())
            return defaultValue;
        for (const auto &[name, value] : options) {
            if (text == QLatin1String(name))
                return value;
        }
        QStringList names;
        for (const auto &option : options)
            names.append(QLatin1String(option.first));
        fail(QStringLiteral("\"%1\" must be one of %2").arg(QLatin1String(key), names.join(QStringLiteral(", "))));
        return defaultValue;
    }

private:
    QString m_context;
    QString m_error;
};

const QList<std::pair<const char *, PinDirection>> directionNames = {
    {"left", PinDirection::Left}, {"right", PinDirection::Right}, {"up", PinDirection::Up}, {"down", PinDirection::Down}};
const QList<std::pair<const char *, Fill>> fillNames = {
    {"none", Fill::None}, {"outline", Fill::Outline}, {"background", Fill::Background}};
const QList<std::pair<const char *, TextAnchor>> anchorNames = {
    {"left", TextAnchor::Left}, {"center", TextAnchor::Center}, {"right", TextAnchor::Right}};
const QList<std::pair<const char *, SymbolGraphic::Type>> graphicTypeNames = {
    {"line", SymbolGraphic::Type::Line},     {"polygon", SymbolGraphic::Type::Polygon},
    {"rect", SymbolGraphic::Type::Rect},     {"circle", SymbolGraphic::Type::Circle},
    {"arc", SymbolGraphic::Type::Arc},       {"text", SymbolGraphic::Type::Text}};

template<typename Enum>
QString enumName(const QList<std::pair<const char *, Enum>> &names, Enum value)
{
    for (const auto &[name, candidate] : names) {
        if (candidate == value)
            return QLatin1String(name);
    }
    return {};
}

QJsonArray pointJson(QPoint point)
{
    return QJsonArray{point.x(), point.y()};
}

QJsonArray pointsJson(const QList<QPoint> &points)
{
    QJsonArray array;
    for (const QPoint &point : points)
        array.append(pointJson(point));
    return array;
}

bool isIdentifier(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return pattern.match(text).hasMatch();
}

SymbolGraphic readGraphic(Reader &reader, const QJsonObject &object)
{
    SymbolGraphic graphic;
    graphic.type = reader.choice(object, "type", graphicTypeNames, SymbolGraphic::Type::Line, true);
    switch (graphic.type) {
    case SymbolGraphic::Type::Line:
        graphic.points = reader.points(object, "points", 2);
        break;
    case SymbolGraphic::Type::Polygon:
        graphic.points = reader.points(object, "points", 3);
        graphic.fill = reader.choice(object, "fill", fillNames, Fill::None);
        break;
    case SymbolGraphic::Type::Rect:
        graphic.from = reader.point(object, "from", true);
        graphic.to = reader.point(object, "to", true);
        graphic.fill = reader.choice(object, "fill", fillNames, Fill::None);
        break;
    case SymbolGraphic::Type::Circle:
        graphic.center = reader.point(object, "center", true);
        graphic.radius = reader.integer(object, "radius", 0);
        graphic.fill = reader.choice(object, "fill", fillNames, Fill::None);
        if (graphic.radius <= 0)
            reader.fail(QStringLiteral("a circle needs a positive \"radius\""));
        break;
    case SymbolGraphic::Type::Arc:
        graphic.center = reader.point(object, "center", true);
        graphic.radius = reader.integer(object, "radius", 0);
        graphic.startAngle = reader.number(object, "startAngle", 0);
        graphic.spanAngle = reader.number(object, "spanAngle", 0);
        if (graphic.radius <= 0)
            reader.fail(QStringLiteral("an arc needs a positive \"radius\""));
        break;
    case SymbolGraphic::Type::Text:
        graphic.at = reader.point(object, "at", true);
        graphic.text = reader.string(object, "text", true);
        graphic.size = reader.integer(object, "size", 10);
        graphic.anchor = reader.choice(object, "anchor", anchorNames, TextAnchor::Left);
        break;
    }
    return graphic;
}

QJsonObject graphicJson(const SymbolGraphic &graphic)
{
    QJsonObject object;
    object.insert(QStringLiteral("type"), enumName(graphicTypeNames, graphic.type));
    const auto insertFill = [&] {
        if (graphic.fill != Fill::None)
            object.insert(QStringLiteral("fill"), enumName(fillNames, graphic.fill));
    };
    switch (graphic.type) {
    case SymbolGraphic::Type::Line:
        object.insert(QStringLiteral("points"), pointsJson(graphic.points));
        break;
    case SymbolGraphic::Type::Polygon:
        object.insert(QStringLiteral("points"), pointsJson(graphic.points));
        insertFill();
        break;
    case SymbolGraphic::Type::Rect:
        object.insert(QStringLiteral("from"), pointJson(graphic.from));
        object.insert(QStringLiteral("to"), pointJson(graphic.to));
        insertFill();
        break;
    case SymbolGraphic::Type::Circle:
        object.insert(QStringLiteral("center"), pointJson(graphic.center));
        object.insert(QStringLiteral("radius"), graphic.radius);
        insertFill();
        break;
    case SymbolGraphic::Type::Arc:
        object.insert(QStringLiteral("center"), pointJson(graphic.center));
        object.insert(QStringLiteral("radius"), graphic.radius);
        object.insert(QStringLiteral("startAngle"), graphic.startAngle);
        object.insert(QStringLiteral("spanAngle"), graphic.spanAngle);
        break;
    case SymbolGraphic::Type::Text:
        object.insert(QStringLiteral("at"), pointJson(graphic.at));
        object.insert(QStringLiteral("text"), graphic.text);
        object.insert(QStringLiteral("size"), graphic.size);
        object.insert(QStringLiteral("anchor"), enumName(anchorNames, graphic.anchor));
        break;
    }
    return object;
}

} // namespace

const SymbolPin *Symbol::pin(const QString &name) const
{
    for (const SymbolPin &candidate : pins) {
        if (candidate.pin == name)
            return &candidate;
    }
    return nullptr;
}

QRect Symbol::boundingRect() const
{
    QRect rect;
    const auto add = [&rect](const QRect &part) { rect = rect.isNull() ? part : rect.united(part); };
    const auto addPoint = [&add](QPoint point) { add(QRect(point, QSize(1, 1))); };
    for (const SymbolGraphic &graphic : graphics) {
        switch (graphic.type) {
        case SymbolGraphic::Type::Line:
        case SymbolGraphic::Type::Polygon:
            for (const QPoint &point : graphic.points)
                addPoint(point);
            break;
        case SymbolGraphic::Type::Rect:
            add(QRect(graphic.from, graphic.to).normalized());
            break;
        case SymbolGraphic::Type::Circle:
        case SymbolGraphic::Type::Arc:
            add(QRect(graphic.center - QPoint(graphic.radius, graphic.radius),
                      QSize(2 * graphic.radius, 2 * graphic.radius)));
            break;
        case SymbolGraphic::Type::Text: {
            const int width = static_cast<int>(graphic.text.size()) * graphic.size * 6 / 10;
            int left = graphic.at.x();
            if (graphic.anchor == TextAnchor::Center)
                left -= width / 2;
            else if (graphic.anchor == TextAnchor::Right)
                left -= width;
            add(QRect(left, graphic.at.y() - graphic.size / 2, width, graphic.size));
            break;
        }
        }
    }
    for (const SymbolPin &pin : pins)
        addPoint(pin.at);
    return rect;
}

std::optional<ComponentDefinition> ComponentDefinition::fromJson(const QJsonObject &object, QString *error)
{
    const QString idForContext = object.value(QStringLiteral("id")).toString();
    Reader reader(idForContext.isEmpty() ? QStringLiteral("component") : QStringLiteral("component \"%1\"").arg(idForContext));

    ComponentDefinition definition;
    definition.id = reader.string(object, "id", true);
    definition.name = reader.string(object, "name", true);
    definition.category = reader.string(object, "category", true);
    definition.description = reader.string(object, "description", false);
    definition.referencePrefix = reader.string(object, "referencePrefix", true);
    definition.netName = reader.string(object, "netName", false);
    definition.role = reader.string(object, "role", false);

    for (const QJsonValue &value : reader.array(object, "pins", true))
        definition.pins.append(reader.string(value.toObject(), "name", true));

    for (const QJsonValue &value : reader.array(object, "parameters", false)) {
        const QJsonObject item = value.toObject();
        ParameterDefinition parameter;
        parameter.name = reader.string(item, "name", true);
        parameter.label = reader.string(item, "label", false);
        parameter.unit = reader.string(item, "unit", false);
        parameter.defaultValue = reader.string(item, "default", false);
        parameter.shown = reader.boolean(item, "shown", false);
        parameter.isFile = reader.boolean(item, "file", false);
        parameter.isText = reader.boolean(item, "text", false);
        for (const QJsonValue &choice : reader.array(item, "choices", false))
            parameter.choices.append(choice.toString());
        definition.parameters.append(parameter);
    }

    const QJsonValue symbolValue = object.value(QStringLiteral("symbol"));
    if (!symbolValue.isObject()) {
        reader.fail(QStringLiteral("missing \"symbol\""));
    } else {
        const QJsonObject symbol = symbolValue.toObject();
        for (const QJsonValue &value : reader.array(symbol, "graphics", false))
            definition.symbol.graphics.append(readGraphic(reader, value.toObject()));
        for (const QJsonValue &value : reader.array(symbol, "pins", true)) {
            const QJsonObject item = value.toObject();
            SymbolPin pin;
            pin.pin = reader.string(item, "pin", true);
            pin.at = reader.point(item, "at", true);
            pin.length = reader.integer(item, "length", 10);
            pin.direction = reader.choice(item, "direction", directionNames, PinDirection::Right, true);
            definition.symbol.pins.append(pin);
        }
        definition.symbol.referenceAt = reader.point(symbol, "referenceAt", false);
        definition.symbol.valueAt = reader.point(symbol, "valueAt", false);
    }

    for (const QJsonValue &value : reader.array(object, "models", false)) {
        ModelDefinition model;
        model.data = value.toObject();
        model.kind = reader.string(model.data, "kind", true);
        definition.models.append(model);
    }

    if (reader.ok()) {
        const QStringList problems = definition.validate();
        if (!problems.isEmpty())
            reader.fail(problems.first());
    }
    if (!reader.ok()) {
        setError(error, reader.error());
        return std::nullopt;
    }
    return definition;
}

QStringList ComponentDefinition::validate() const
{
    QStringList problems;
    const QString context = QStringLiteral("component \"%1\": ").arg(id);
    static const QRegularExpression idPattern(QStringLiteral("^[a-z0-9_]+$"));
    if (!idPattern.match(id).hasMatch())
        problems.append(context + QStringLiteral("the id may contain only a-z, 0-9 and _"));
    static const QRegularExpression prefixPattern(QStringLiteral("^[A-Za-z]+$"));
    if (!prefixPattern.match(referencePrefix).hasMatch())
        problems.append(context + QStringLiteral("the reference prefix must be letters only"));

    QSet<QString> pinNames;
    for (const QString &pin : pins) {
        if (pin.isEmpty())
            problems.append(context + QStringLiteral("a pin has no name"));
        else if (pinNames.contains(pin))
            problems.append(context + QStringLiteral("pin \"%1\" is defined twice").arg(pin));
        pinNames.insert(pin);
        if (!symbol.pin(pin))
            problems.append(context + QStringLiteral("pin \"%1\" is not placed on the symbol").arg(pin));
    }
    QSet<QPoint> pinPoints;
    QSet<QString> symbolPins;
    for (const SymbolPin &pin : symbol.pins) {
        if (!pinNames.contains(pin.pin))
            problems.append(context + QStringLiteral("the symbol places unknown pin \"%1\"").arg(pin.pin));
        if (symbolPins.contains(pin.pin))
            problems.append(context + QStringLiteral("the symbol places pin \"%1\" twice").arg(pin.pin));
        symbolPins.insert(pin.pin);
        if (pin.at.x() % 10 != 0 || pin.at.y() % 10 != 0)
            problems.append(context + QStringLiteral("pin \"%1\" is not on the 10-unit grid").arg(pin.pin));
        if (pinPoints.contains(pin.at))
            problems.append(context + QStringLiteral("two pins share the point (%1, %2)").arg(pin.at.x()).arg(pin.at.y()));
        pinPoints.insert(pin.at);
        if (pin.length < 0)
            problems.append(context + QStringLiteral("pin \"%1\" has a negative length").arg(pin.pin));
    }

    QSet<QString> parameterNames;
    for (const ParameterDefinition &parameter : parameters) {
        if (!isIdentifier(parameter.name))
            problems.append(context + QStringLiteral("\"%1\" is not a valid parameter name").arg(parameter.name));
        if (parameterNames.contains(parameter.name))
            problems.append(context + QStringLiteral("parameter \"%1\" is defined twice").arg(parameter.name));
        parameterNames.insert(parameter.name);
        if (!parameter.choices.isEmpty() && !parameter.choices.contains(parameter.defaultValue))
            problems.append(context + QStringLiteral("the default of \"%1\" is not one of its choices").arg(parameter.name));
    }

    if (!netName.isEmpty() && pins.size() != 1)
        problems.append(context + QStringLiteral("a component with a net name must have exactly one pin"));
    if (!role.isEmpty() && role != sheetPortRole())
        problems.append(context + QStringLiteral("unknown role \"%1\"").arg(role));
    if (isSheetPort()) {
        if (pins.size() != 1)
            problems.append(context + QStringLiteral("a sheet port must have exactly one pin"));
        if (!parameter(QStringLiteral("name")))
            problems.append(context + QStringLiteral("a sheet port needs a \"name\" parameter"));
    }
    for (const ModelDefinition &model : models) {
        if (model.kind.isEmpty())
            problems.append(context + QStringLiteral("a model has no kind"));
    }
    return problems;
}

QJsonObject ComponentDefinition::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("name"), name);
    object.insert(QStringLiteral("category"), category);
    if (!description.isEmpty())
        object.insert(QStringLiteral("description"), description);
    object.insert(QStringLiteral("referencePrefix"), referencePrefix);
    if (!netName.isEmpty())
        object.insert(QStringLiteral("netName"), netName);
    if (!role.isEmpty())
        object.insert(QStringLiteral("role"), role);

    QJsonArray pinArray;
    for (const QString &pin : pins)
        pinArray.append(QJsonObject{{QStringLiteral("name"), pin}});
    object.insert(QStringLiteral("pins"), pinArray);

    if (!parameters.isEmpty()) {
        QJsonArray parameterArray;
        for (const ParameterDefinition &parameter : parameters) {
            QJsonObject item;
            item.insert(QStringLiteral("name"), parameter.name);
            if (!parameter.label.isEmpty())
                item.insert(QStringLiteral("label"), parameter.label);
            if (!parameter.unit.isEmpty())
                item.insert(QStringLiteral("unit"), parameter.unit);
            item.insert(QStringLiteral("default"), parameter.defaultValue);
            if (parameter.shown)
                item.insert(QStringLiteral("shown"), true);
            if (!parameter.choices.isEmpty())
                item.insert(QStringLiteral("choices"), QJsonArray::fromStringList(parameter.choices));
            if (parameter.isFile)
                item.insert(QStringLiteral("file"), true);
            if (parameter.isText)
                item.insert(QStringLiteral("text"), true);
            parameterArray.append(item);
        }
        object.insert(QStringLiteral("parameters"), parameterArray);
    }

    QJsonObject symbolObject;
    QJsonArray graphicArray;
    for (const SymbolGraphic &graphic : symbol.graphics)
        graphicArray.append(graphicJson(graphic));
    symbolObject.insert(QStringLiteral("graphics"), graphicArray);
    QJsonArray symbolPinArray;
    for (const SymbolPin &pin : symbol.pins) {
        symbolPinArray.append(QJsonObject{{QStringLiteral("pin"), pin.pin},
                                          {QStringLiteral("at"), pointJson(pin.at)},
                                          {QStringLiteral("length"), pin.length},
                                          {QStringLiteral("direction"), pinDirectionName(pin.direction)}});
    }
    symbolObject.insert(QStringLiteral("pins"), symbolPinArray);
    symbolObject.insert(QStringLiteral("referenceAt"), pointJson(symbol.referenceAt));
    symbolObject.insert(QStringLiteral("valueAt"), pointJson(symbol.valueAt));
    object.insert(QStringLiteral("symbol"), symbolObject);

    if (!models.isEmpty()) {
        QJsonArray modelArray;
        for (const ModelDefinition &model : models) {
            QJsonObject data = model.data;
            data.insert(QStringLiteral("kind"), model.kind);
            modelArray.append(data);
        }
        object.insert(QStringLiteral("models"), modelArray);
    }
    return object;
}

const ParameterDefinition *ComponentDefinition::parameter(const QString &parameterName) const
{
    for (const ParameterDefinition &candidate : parameters) {
        if (candidate.name == parameterName)
            return &candidate;
    }
    return nullptr;
}

const ModelDefinition *ComponentDefinition::model(const QString &kind) const
{
    for (const ModelDefinition &candidate : models) {
        if (candidate.kind == kind)
            return &candidate;
    }
    return nullptr;
}

int normalizedRotation(int degrees)
{
    const int rounded = static_cast<int>(std::lround(degrees / 90.0)) * 90;
    return ((rounded % 360) + 360) % 360;
}

QPoint Orientation::map(QPoint point) const
{
    if (mirror)
        point.setX(-point.x());
    switch (normalizedRotation(rotation)) {
    case 90:
        return QPoint(point.y(), -point.x());
    case 180:
        return QPoint(-point.x(), -point.y());
    case 270:
        return QPoint(-point.y(), point.x());
    default:
        return point;
    }
}

PinDirection Orientation::map(PinDirection direction) const
{
    QPoint vector;
    switch (direction) {
    case PinDirection::Left: vector = QPoint(-1, 0); break;
    case PinDirection::Right: vector = QPoint(1, 0); break;
    case PinDirection::Up: vector = QPoint(0, -1); break;
    case PinDirection::Down: vector = QPoint(0, 1); break;
    }
    const QPoint mapped = map(vector);
    if (mapped.x() < 0)
        return PinDirection::Left;
    if (mapped.x() > 0)
        return PinDirection::Right;
    return mapped.y() < 0 ? PinDirection::Up : PinDirection::Down;
}

QString pinDirectionName(PinDirection direction)
{
    return enumName(directionNames, direction);
}

} // namespace lvd

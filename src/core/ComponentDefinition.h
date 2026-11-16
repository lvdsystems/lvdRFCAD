#pragma once

#include <QJsonObject>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>

#include <optional>

namespace lvd {

// Component definition and symbol: docs/file-format.md, section 4.

enum class PinDirection { Left, Right, Up, Down };
enum class Fill { None, Outline, Background };
enum class TextAnchor { Left, Center, Right };

struct SymbolGraphic {
    enum class Type { Line, Polygon, Rect, Circle, Arc, Text };

    Type type = Type::Line;
    QList<QPoint> points;    // line, polygon
    QPoint from, to;         // rect
    QPoint center;           // circle, arc
    int radius = 0;          // circle, arc
    double startAngle = 0;   // arc, degrees counter-clockwise on screen from +X
    double spanAngle = 0;    // arc
    QPoint at;               // text
    QString text;            // text
    int size = 10;           // text
    TextAnchor anchor = TextAnchor::Left;
    Fill fill = Fill::None;

    bool operator==(const SymbolGraphic &) const = default;
};

struct SymbolPin {
    QString pin;
    QPoint at; // connection point
    int length = 10;
    PinDirection direction = PinDirection::Right; // from the connection point toward the body

    bool operator==(const SymbolPin &) const = default;
};

struct Symbol {
    QList<SymbolGraphic> graphics;
    QList<SymbolPin> pins;
    QPoint referenceAt;
    QPoint valueAt;

    const SymbolPin *pin(const QString &name) const;
    // Bounding rectangle of graphics and pins, in symbol coordinates. Text is
    // approximated from its size.
    QRect boundingRect() const;

    bool operator==(const Symbol &) const = default;
};

struct ParameterDefinition {
    QString name;
    QString label;
    QString unit;
    QString defaultValue;
    bool shown = false;
    QStringList choices; // non-empty: an enumeration
    bool isFile = false; // a file path relative to the project folder
    bool isText = false; // plain text, not an expression

    // Values of enumeration, file and text parameters are not evaluated.
    bool isExpression() const { return choices.isEmpty() && !isFile && !isText; }

    bool operator==(const ParameterDefinition &) const = default;
};

struct ModelDefinition {
    QString kind;
    QJsonObject data; // the whole model object, including "kind"

    bool operator==(const ModelDefinition &) const = default;
};

struct ComponentDefinition {
    QString id;
    QString name;
    QString category;
    QString description;
    QString referencePrefix;
    QStringList pins;
    QList<ParameterDefinition> parameters;
    Symbol symbol;
    QList<ModelDefinition> models;
    QString netName; // global net of a single-pin component, such as ground
    QString role;    // "sheet-port" or empty

    static QString sheetPortRole() { return QStringLiteral("sheet-port"); }

    // Parses and validates. Returns nullopt with *error set on any problem.
    static std::optional<ComponentDefinition> fromJson(const QJsonObject &object, QString *error = nullptr);
    QJsonObject toJson() const;

    // Problems that make the definition unusable; empty when valid.
    QStringList validate() const;

    const ParameterDefinition *parameter(const QString &parameterName) const;
    const ModelDefinition *model(const QString &kind) const;
    bool isSheetPort() const { return role == sheetPortRole(); }

    bool operator==(const ComponentDefinition &) const = default;
};

// Orientation of a placed symbol: mirror about the Y axis first, then rotate
// counter-clockwise on screen (Y points down).
struct Orientation {
    int rotation = 0; // 0, 90, 180 or 270
    bool mirror = false;

    QPoint map(QPoint point) const;
    PinDirection map(PinDirection direction) const;
};

int normalizedRotation(int degrees); // to 0, 90, 180 or 270; other angles round to the nearest

QString pinDirectionName(PinDirection direction);

} // namespace lvd

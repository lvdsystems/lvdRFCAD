#pragma once

#include "core/Library.h"
#include "core/UndoStack.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPoint>
#include <QSet>
#include <QString>

#include <memory>
#include <optional>

namespace lvd {

// Schematic content of RF and SPICE documents: docs/file-format.md, section 5.

struct PlacedComponent {
    QString id;
    QString reference;
    QString definition; // "library:component" or "sheet:<document>"
    QPoint at;
    int rotation = 0;
    bool mirror = false;
    QMap<QString, QString> parameters; // values that differ from the defaults

    Orientation orientation() const { return Orientation{rotation, mirror}; }
    bool operator==(const PlacedComponent &) const = default;
};

struct Wire {
    QString id;
    QList<QPoint> points; // at least two

    bool operator==(const Wire &) const = default;
};

struct Junction {
    QString id;
    QPoint at;

    bool operator==(const Junction &) const = default;
};

struct NetLabel {
    QString id;
    QString net;
    QPoint at;

    bool operator==(const NetLabel &) const = default;
};

struct TextNote {
    QString id;
    QString text;
    QPoint at;
    int size = 12;

    bool operator==(const TextNote &) const = default;
};

struct Variable {
    QString name;
    QString value;

    bool operator==(const Variable &) const = default;
};

enum class ItemKind { None, Component, Wire, Junction, Label, Text };

// Every change goes through an edit, which records one undo step. Mutators
// called outside beginEdit()/endEdit() form an edit of their own.
class Schematic : public QObject
{
    Q_OBJECT
public:
    explicit Schematic(QObject *parent = nullptr);

    static std::unique_ptr<Schematic> fromJson(const QJsonObject &content, QString *error = nullptr);
    QJsonObject toJson() const;

    static ItemKind kindOf(const QString &id);

    // Read access
    const QMap<QString, PlacedComponent> &components() const { return m_components; }
    const QMap<QString, Wire> &wires() const { return m_wires; }
    const QMap<QString, Junction> &junctions() const { return m_junctions; }
    const QMap<QString, NetLabel> &labels() const { return m_labels; }
    const QMap<QString, TextNote> &texts() const { return m_texts; }
    const QList<Variable> &variables() const { return m_variables; }
    // Analysis setups and plot definitions (docs/file-format.md, section 5.1).
    // Stored as JSON; the simulators interpret it.
    const QJsonObject &simulation() const { return m_simulation; }
    const PlacedComponent *component(const QString &id) const;
    const PlacedComponent *componentByReference(const QString &reference) const;
    bool contains(const QString &id) const;
    QStringList itemIds() const; // all items, sorted by kind then id

    // Definition cache (fallback copies of library definitions)
    const QMap<QString, DefinitionPtr> &cachedDefinitions() const { return m_cache; }
    DefinitionPtr cachedDefinition(const QString &key) const { return m_cache.value(key); }
    void setCachedDefinition(const QString &key, DefinitionPtr definition);
    // Removes cache entries no placed component refers to.
    void pruneCache();

    // Edits
    void beginEdit(const QString &description);
    void endEdit();
    bool isEditing() const { return m_editDepth > 0; }

    class Edit
    {
    public:
        Edit(Schematic *schematic, const QString &description) : m_schematic(schematic)
        {
            m_schematic->beginEdit(description);
        }
        ~Edit() { m_schematic->endEdit(); }
        Edit(const Edit &) = delete;
        Edit &operator=(const Edit &) = delete;

    private:
        Schematic *m_schematic;
    };

    // `definition` supplies the reference prefix; it may be null ("U" is used).
    QString addComponent(const QString &definitionKey, const ComponentDefinition *definition, QPoint at,
                         int rotation = 0, bool mirror = false);
    // Replaces the component with the same id. Returns false if there is none.
    bool updateComponent(const PlacedComponent &component);
    bool setParameter(const QString &componentId, const QString &name, const QString &value);
    bool resetParameter(const QString &componentId, const QString &name);
    bool setReference(const QString &componentId, const QString &reference);

    // Consecutive duplicate points are dropped; returns an empty id if fewer
    // than two distinct points remain.
    QString addWire(const QList<QPoint> &points);
    bool updateWire(const Wire &wire);
    QString addJunction(QPoint at);
    QString addLabel(const QString &net, QPoint at);
    bool updateLabel(const NetLabel &label);
    QString addText(const QString &text, QPoint at, int size = 12);
    bool updateText(const TextNote &text);
    bool removeItem(const QString &id);
    void removeItems(const QStringList &ids);
    void setVariables(const QList<Variable> &variables);
    void setSimulation(const QJsonObject &simulation);

    // Moves items. With dragWires, wire end points that touch a moved
    // component pin or a moved wire end follow the move, keeping the attached
    // segment horizontal or vertical. `pinPositions` gives the connection
    // points of each moved component before the move.
    void moveItems(const QStringList &ids, QPoint delta,
                   const QHash<QString, QList<QPoint>> &pinPositions = {}, bool dragWires = false);
    // Rotates items counter-clockwise on screen about a centre, in steps of 90 degrees.
    void rotateItems(const QStringList &ids, QPoint center, int degrees = 90);
    // Mirrors items about the vertical line through center.x().
    void mirrorItems(const QStringList &ids, QPoint center);

    // Next free reference for a prefix, such as "R3".
    QString nextReference(const QString &prefix) const;

    UndoStack *undoStack() const { return m_undo; }

Q_SIGNALS:
    void itemAdded(const QString &id);
    void itemChanged(const QString &id);
    void itemRemoved(const QString &id);
    void variablesChanged();
    void simulationChanged();
    // After every completed edit, undo and redo.
    void changed();

private:
    using Snapshot = std::optional<QJsonObject>;

    QString newId(QChar prefix);
    void touch(const QString &id);
    Snapshot snapshot(const QString &id) const;
    void apply(const QString &id, const Snapshot &state);

    QMap<QString, PlacedComponent> m_components;
    QMap<QString, Wire> m_wires;
    QMap<QString, Junction> m_junctions;
    QMap<QString, NetLabel> m_labels;
    QMap<QString, TextNote> m_texts;
    QList<Variable> m_variables;
    QJsonObject m_simulation;
    QMap<QString, DefinitionPtr> m_cache;
    int m_nextId = 1;

    int m_editDepth = 0;
    QString m_editDescription;
    QMap<QString, Snapshot> m_before;
    UndoStack *m_undo;
};

// JSON of single items, as stored in the file.
QJsonObject componentToJson(const PlacedComponent &component);
QJsonObject wireToJson(const Wire &wire);

} // namespace lvd

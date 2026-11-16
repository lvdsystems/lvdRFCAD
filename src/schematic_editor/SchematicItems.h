#pragma once

#include "core/Project.h"
#include "core/RuleCheck.h"
#include "core/Schematic.h"

#include <QGraphicsItem>
#include <QPainterPath>
#include <QSet>

#include <optional>

namespace lvd {

class SchematicScene;
struct SchematicStyle;

// Graphics items, one per schematic item. Each item keeps a copy of its data,
// so the same classes also draw ghosts (placement, paste) that are not part
// of the schematic. Coordinates are sheet units; the item position is the
// item's anchor point (wires use position 0 and absolute points).
class SchematicItem : public QGraphicsItem
{
public:
    SchematicItem(const QString &id, ItemKind kind);

    QString id() const { return m_id; }
    ItemKind kind() const { return m_kind; }

    // Ghosts are drawn translucent and never selected.
    bool isGhost() const { return m_ghost; }
    void setGhost(bool ghost);

    void setHovered(bool hovered);
    void setHighlighted(bool highlighted);
    bool isHighlighted() const { return m_highlighted; }
    void setMark(std::optional<RuleViolation::Severity> severity);

    // Anchor in sheet coordinates (the stored position).
    virtual QPoint anchor() const = 0;
    // Temporary offset while dragging or floating; cleared by resetPreview().
    virtual void setPreviewOffset(QPoint offset);
    virtual void resetPreview();

protected:
    SchematicScene *schematicScene() const;
    const SchematicStyle &style() const;
    // Hover and rule-check halo around rect, in item coordinates.
    void paintDecorations(QPainter *painter, const QRectF &rect) const;
    bool drawSelected() const { return isSelected() && !m_ghost; }
    void setAnchorPosition(QPoint at) { setPos(at); }

    bool m_hovered = false;
    bool m_highlighted = false;
    std::optional<RuleViolation::Severity> m_mark;

private:
    QString m_id;
    ItemKind m_kind;
    bool m_ghost = false;
};

class ComponentItem : public SchematicItem
{
public:
    enum { Type = UserType + 1 };
    int type() const override { return Type; }

    ComponentItem(const PlacedComponent &component, const Resolution &resolution, SchematicScene *scene);

    void setComponent(const PlacedComponent &component, const Resolution &resolution);
    const PlacedComponent &component() const { return m_component; }
    const Resolution &resolution() const { return m_resolution; }

    // Connection points in sheet coordinates, keyed by pin name.
    QList<QPair<QString, QPoint>> pins() const;
    QList<QPoint> pinPositions() const;
    // The pin whose connection point is at `at`, or empty.
    QString pinAt(QPoint at) const;

    void setUnconnectedPins(const QSet<QString> &pins);
    const QSet<QString> &unconnectedPins() const { return m_unconnected; }
    void setHighlightedPins(const QSet<QString> &pins);

    QPoint anchor() const override { return m_component.at; }
    void resetPreview() override;

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    struct TextLine {
        QPointF at; // item coordinates
        QString text;
        TextAnchor anchor;
        double size;
        bool reference;
    };
    void layoutTexts();
    const Symbol &symbol() const;

    SchematicScene *m_owner;
    PlacedComponent m_component;
    Resolution m_resolution;
    QTransform m_transform; // symbol to item coordinates
    QList<TextLine> m_texts;
    QRectF m_body;  // symbol area, item coordinates
    QRectF m_bounds;
    QSet<QString> m_unconnected;
    QSet<QString> m_highlightedPins;
};

class WireItem : public SchematicItem
{
public:
    enum { Type = UserType + 2 };
    int type() const override { return Type; }

    explicit WireItem(const Wire &wire);

    void setWire(const Wire &wire);
    const Wire &wire() const { return m_wire; }
    // Shows other points while dragging, without changing the stored wire.
    void setPreviewPoints(const QList<QPoint> &points);

    QPoint anchor() const override { return m_wire.points.value(0); }
    // Wire geometry is absolute: the offset is the item position.
    void setPreviewOffset(QPoint offset) override { setPos(offset); }
    void resetPreview() override;

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    void setGeometry(const QList<QPoint> &points);

    Wire m_wire;
    QList<QPoint> m_shown;
    QPainterPath m_path;
    QPainterPath m_shape;
};

class JunctionItem : public SchematicItem
{
public:
    enum { Type = UserType + 3 };
    int type() const override { return Type; }

    explicit JunctionItem(const Junction &junction);
    void setJunction(const Junction &junction);
    const Junction &junction() const { return m_junction; }

    QPoint anchor() const override { return m_junction.at; }
    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    Junction m_junction;
};

class LabelItem : public SchematicItem
{
public:
    enum { Type = UserType + 4 };
    int type() const override { return Type; }

    LabelItem(const NetLabel &label, SchematicScene *scene);
    void setLabel(const NetLabel &label);
    const NetLabel &label() const { return m_label; }

    QPoint anchor() const override { return m_label.at; }
    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    SchematicScene *m_owner;
    NetLabel m_label;
    QRectF m_textRect;
};

class TextItem : public SchematicItem
{
public:
    enum { Type = UserType + 5 };
    int type() const override { return Type; }

    TextItem(const TextNote &text, SchematicScene *scene);
    void setText(const TextNote &text);
    const TextNote &text() const { return m_text; }

    QPoint anchor() const override { return m_text.at; }
    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    SchematicScene *m_owner;
    TextNote m_text;
    QRectF m_rect;
};

} // namespace lvd

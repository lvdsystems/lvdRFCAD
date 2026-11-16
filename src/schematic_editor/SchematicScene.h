#pragma once

#include "core/Connectivity.h"
#include "core/Project.h"
#include "core/RuleCheck.h"
#include "widgets/SchematicStyle.h"
#include "widgets/SymbolPainter.h"

#include <QFontMetricsF>
#include <QGraphicsScene>
#include <QHash>
#include <QPointer>
#include <QTimer>

namespace lvd {

class ComponentItem;
class Document;
class SchematicItem;

// Keeps one graphics item per schematic item, updated incrementally from the
// schematic's signals, and caches what is expensive to derive: resolved
// definitions per key and the sheet connectivity (recomputed after edits,
// debounced).
class SchematicScene : public QGraphicsScene
{
    Q_OBJECT
public:
    SchematicScene(Project *project, Document *document, QObject *parent = nullptr);
    ~SchematicScene() override;

    Project *project() const { return m_project; }
    Document *document() const { return m_document; }
    Schematic *schematic() const { return m_schematic; }

    const SchematicStyle &style() const { return m_style; }
    void setStyle(const SchematicStyle &style);
    const SymbolPainter &symbolPainter() const { return m_painter; }
    // Size of text drawn by SymbolPainter::paintText at `size` drawing units.
    QSizeF textSize(const QString &text, double size) const;

    // Cached resolution of a definition key.
    Resolution resolution(const QString &key);
    DefinitionLookup lookup();
    // Forgets cached resolutions (all when key is empty) and refreshes the
    // components that use them.
    void invalidateDefinitions(const QString &key = QString());

    SchematicItem *item(const QString &id) const { return m_items.value(id); }
    ComponentItem *componentItem(const QString &id) const;
    const QHash<QString, SchematicItem *> &schematicItems() const { return m_items; }

    // Up to date: recomputes now if an edit is pending.
    const Connectivity &connectivity();
    bool isConnectivityPending() const { return m_connectivityDirty; }
    void scheduleConnectivity();

    // Highlights the net that contains a seed: an item id or "componentId/pin".
    void setHighlightSeed(const QString &seed);
    QString highlightSeed() const { return m_highlightSeed; }
    // Index into connectivity().nets of the highlighted net, or -1.
    int highlightedNet();

    void setViolations(const QList<RuleViolation> &violations);
    const QList<RuleViolation> &violations() const { return m_violations; }

    QStringList selectedIds() const;
    void selectIds(const QStringList &ids, bool clearFirst = true);

    // Sheet-space bounds of all schematic items.
    QRectF contentBounds() const;

    void setGridVisible(bool visible);

Q_SIGNALS:
    void connectivityUpdated();

protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;

private:
    void addItemFor(const QString &id);
    void updateItemFor(const QString &id);
    void removeItemFor(const QString &id);
    void recomputeConnectivity();
    void applyHighlight();
    void applyMarks();
    void watchSheet(Document *document);

    Project *m_project;
    Document *m_document;
    Schematic *m_schematic;
    SchematicStyle m_style;
    SymbolPainter m_painter;
    QFontMetricsF m_metrics;
    bool m_gridVisible = true;

    QHash<QString, Resolution> m_resolutions;
    QHash<QString, SchematicItem *> m_items;

    Connectivity m_connectivity;
    bool m_connectivityDirty = true;
    QTimer m_connectivityTimer;
    QString m_highlightSeed;
    QSet<QString> m_highlightedIds; // items currently drawn highlighted

    QList<RuleViolation> m_violations;
};

} // namespace lvd

#include "matching/MatchingInsert.h"

#include "core/Connectivity.h"
#include "core/Document.h"
#include "core/Expression.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QCoreApplication>
#include <QSet>

#include <cmath>

namespace lvd {

namespace {

using namespace lvd::rf;

QString tr(const char *text)
{
    return QCoreApplication::translate("lvd::MatchingInsert", text);
}

QString definitionKey(const MatchElement &e)
{
    switch (e.kind) {
    case MatchElementKind::Capacitor: return QStringLiteral("basic:capacitor");
    case MatchElementKind::Inductor: return QStringLiteral("basic:inductor");
    case MatchElementKind::Line: return QStringLiteral("rf-circuit:tline");
    case MatchElementKind::OpenStub: return QStringLiteral("rf-circuit:open_stub");
    case MatchElementKind::ShortStub: return QStringLiteral("rf-circuit:short_stub");
    }
    return {};
}

QPoint snapToGrid(QPoint p)
{
    const auto snap = [](int v) { return static_cast<int>(std::lround(v / 10.0)) * 10; };
    return QPoint(snap(p.x()), snap(p.y()));
}

// Offset of a pin from the component position for an orientation.
QPoint pinOffset(const ComponentDefinition &definition, const QString &pin, int rotation)
{
    PlacedComponent probe;
    probe.rotation = rotation;
    const SymbolPin *symbolPin = definition.symbol.pin(pin);
    return symbolPin ? pinPosition(probe, *symbolPin) : QPoint();
}

QString number(double value, int digits)
{
    return formatEngineering(value, QString(), digits);
}

// Base names with the same numeric suffix, both unused as net labels.
std::pair<QString, QString> uniqueLabels(const Schematic &sheet, const QString &in, const QString &out)
{
    QSet<QString> used;
    for (const NetLabel &label : sheet.labels())
        used.insert(label.net);
    for (const PlacedComponent &component : sheet.components())
        used.insert(component.reference);
    for (int k = 1;; ++k) {
        const QString suffix = k == 1 ? QString() : QStringLiteral("_%1").arg(k);
        const QString a = in + suffix, b = out + suffix;
        if (!used.contains(a) && !used.contains(b))
            return {a, b};
    }
}

} // namespace

std::optional<InsertedMatch> insertMatchingNetwork(Project &project, Document &document, const MatchSolution &solution,
                                                   QPoint origin, QString *error, const MatchInsertOptions &options)
{
    const auto fail = [error](const QString &message) -> std::optional<InsertedMatch> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    Schematic *sheet = document.schematic();
    if (!sheet)
        return fail(tr("The document %1 is not a schematic.").arg(document.name()));
    if (options.inputLabel.trimmed().isEmpty() || options.outputLabel.trimmed().isEmpty())
        return fail(tr("The net label names must not be empty."));

    // Resolve every definition first so that a failure changes nothing.
    QHash<QString, DefinitionPtr> definitions;
    QStringList keys{QStringLiteral("basic:ground")};
    for (const MatchElement &e : solution.elements)
        keys << definitionKey(e);
    for (const QString &key : keys) {
        if (definitions.contains(key))
            continue;
        const DefinitionPtr definition = project.resolve(*sheet, key).definition;
        if (!definition)
            return fail(tr("The library part %1 is not available.").arg(key));
        definitions.insert(key, definition);
    }

    const int digits = std::max(4, options.significantDigits);
    const auto [inName, outName] = uniqueLabels(*sheet, options.inputLabel.trimmed(), options.outputLabel.trimmed());

    InsertedMatch inserted;
    inserted.inputLabel = inName;
    inserted.outputLabel = outName;

    Schematic::Edit edit(sheet, tr("Insert matching network"));
    const auto wire = [&](QPoint a, QPoint b) {
        if (a == b)
            return;
        const QString id = sheet->addWire({a, b});
        if (!id.isEmpty())
            inserted.itemIds << id;
    };
    const auto place = [&](const QString &key, QPoint at, int rotation) {
        const DefinitionPtr &definition = definitions[key];
        const QString id = sheet->addComponent(key, definition.get(), at, rotation);
        inserted.itemIds << id;
        return id;
    };
    const auto ground = [&](QPoint pin) {
        const QPoint at = pin + QPoint(0, 20);
        place(QStringLiteral("basic:ground"), at, 0);
        wire(pin, at);
    };

    QPoint cursor = snapToGrid(origin);
    inserted.input = cursor;
    inserted.itemIds << sheet->addLabel(inName, cursor);
    const QString f0Text = formatEngineering(solution.frequency, QStringLiteral("Hz"), 12);

    for (const MatchElement &e : solution.elements) {
        const QString key = definitionKey(e);
        const ComponentDefinition &definition = *definitions[key];
        QString id;
        if (e.placement == MatchPlacement::Series) {
            const QString left = e.kind == MatchElementKind::Line ? QStringLiteral("in") : QStringLiteral("p");
            const QString right = e.kind == MatchElementKind::Line ? QStringLiteral("out") : QStringLiteral("n");
            const QPoint leftPin = cursor + QPoint(30, 0);
            const QPoint at = leftPin - pinOffset(definition, left, 0);
            id = place(key, at, 0);
            wire(cursor, leftPin);
            cursor = at + pinOffset(definition, right, 0);
        } else {
            const QPoint node = cursor + QPoint(40, 0);
            wire(cursor, node);
            inserted.itemIds << sheet->addJunction(node);
            const QPoint top = node + QPoint(0, 30);
            if (e.isLumped()) {
                int rotation = 90;
                if (pinOffset(definition, QStringLiteral("p"), 90).y() > pinOffset(definition, QStringLiteral("n"), 90).y())
                    rotation = 270;
                const QPoint at = top - pinOffset(definition, QStringLiteral("p"), rotation);
                id = place(key, at, rotation);
                ground(at + pinOffset(definition, QStringLiteral("n"), rotation));
            } else {
                const QPoint at = top - pinOffset(definition, QStringLiteral("p"), 0);
                id = place(key, at, 0);
            }
            wire(node, top);
            cursor = node;
        }
        switch (e.kind) {
        case MatchElementKind::Capacitor:
            sheet->setParameter(id, QStringLiteral("c"), formatEngineering(e.value, QStringLiteral("F"), digits));
            break;
        case MatchElementKind::Inductor:
            sheet->setParameter(id, QStringLiteral("l"), formatEngineering(e.value, QStringLiteral("H"), digits));
            break;
        case MatchElementKind::Line:
        case MatchElementKind::OpenStub:
        case MatchElementKind::ShortStub:
            sheet->setParameter(id, QStringLiteral("z0"), number(e.z0, digits));
            sheet->setParameter(id, QStringLiteral("elen"), number(e.lengthDeg, digits));
            sheet->setParameter(id, QStringLiteral("freq"), f0Text);
            break;
        }
    }
    const QPoint end = cursor + QPoint(40, 0);
    wire(cursor, end);
    inserted.output = end;
    inserted.itemIds << sheet->addLabel(outName, end);
    return inserted;
}

} // namespace lvd

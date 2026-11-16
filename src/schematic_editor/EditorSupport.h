#pragma once

#include "core/Connectivity.h"
#include "core/Schematic.h"

#include <QJsonObject>
#include <QPointF>
#include <QRect>

#include <memory>

namespace lvd {

constexpr int GridStep = 10;

// Nearest grid point.
QPoint snapToGrid(QPointF point);

// MIME types: items on the clipboard, and component keys dragged from the
// library browser (UTF-8 definition key).
QString itemsMimeType();
QString componentMimeType();

// Items in the layout of schematic content (docs/file-format.md, section 5),
// with the definitions of their components so they paste anywhere.
QJsonObject itemsToJson(const Schematic &schematic, const QStringList &ids, const DefinitionLookup &definitionOf);
// A detached schematic holding the items, or nullptr if the JSON is unusable.
std::unique_ptr<Schematic> itemsFromJson(const QJsonObject &content);

// Bounds of the anchor points of all items (component positions, wire
// points, junctions, labels and texts).
QRect anchorBounds(const Schematic &schematic, const QStringList &ids = {});
// Grid point nearest the centre of a rectangle from anchorBounds.
QPoint gridCenter(const QRect &bounds);

// Adds missing junctions and removes redundant ones. Call inside an edit.
void fixJunctions(Schematic &schematic, const DefinitionLookup &definitionOf);

// Drops repeated points and the middle point of three collinear ones.
QList<QPoint> simplifyPolyline(const QList<QPoint> &points);

// Orthogonal route from `from` to `to`: one or two points after `from`.
QList<QPoint> orthogonalRoute(QPoint from, QPoint to, bool horizontalFirst);

} // namespace lvd

#pragma once

// Helpers shared by the library browser, the symbol editor and the library
// manager dialog. Not part of the public interface of the module.

#include "core/ComponentDefinition.h"
#include "core/Library.h"
#include "widgets/SchematicStyle.h"

#include <QPixmap>
#include <QRectF>
#include <QString>

class QPainter;
class QWidget;

namespace lvd {

// Translated scope name for display: "project", "user", "built-in".
QString scopeDisplayName(LibraryScope scope);

// User and project libraries can be edited; built-in libraries cannot.
inline bool isWritableScope(LibraryScope scope)
{
    return scope != LibraryScope::BuiltIn;
}

// The entry of the effective (not shadowed) library of this name, or nullptr.
const LibraryManager::Entry *effectiveEntry(const LibraryManager &libraries, const QString &name);
// The entry of the library with this name in this scope, or nullptr.
const LibraryManager::Entry *entryInScope(const LibraryManager &libraries, const QString &name, LibraryScope scope);
// The entry that hides a shadowed library, or nullptr.
const LibraryManager::Entry *shadowingEntry(const LibraryManager &libraries, const LibraryManager::Entry &entry);

// Checks a library name: it becomes a file name and the first part of
// definition keys. Returns false with *error set.
bool validateLibraryName(const QString &name, QString *error);

// Bounds of a symbol including pin names, for fitting it into a rectangle.
QRectF symbolBoundsWithPinNames(const Symbol &symbol, const Orientation &orientation = {});

// Draws a symbol scaled to fit target, in the given orientation, optionally
// with pin names. Uses SymbolPainter, so it matches the schematic editor.
void paintSymbolFitted(QPainter &painter, const Symbol &symbol, const QRectF &target, const SchematicStyle &style,
                       const Orientation &orientation = {}, bool showPinNames = false, double maximumScale = 0);

// A symbol drawn into a transparent pixmap of the given size (device independent pixels).
QPixmap symbolPixmap(const Symbol &symbol, QSize size, const SchematicStyle &style, bool showPinNames,
                     double devicePixelRatio, double maximumScale = 0);

// The style for a widget's current palette.
SchematicStyle styleFor(const QWidget *widget);

} // namespace lvd

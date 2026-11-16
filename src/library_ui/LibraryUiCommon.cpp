#include "library_ui/LibraryUiCommon.h"

#include "core/Project.h"
#include "widgets/SymbolPainter.h"

#include <QCoreApplication>
#include <QPainter>
#include <QTransform>
#include <QWidget>

#include <algorithm>

namespace lvd {

QString scopeDisplayName(LibraryScope scope)
{
    switch (scope) {
    case LibraryScope::Project: return QCoreApplication::translate("lvd::LibraryUi", "project");
    case LibraryScope::User: return QCoreApplication::translate("lvd::LibraryUi", "user");
    case LibraryScope::BuiltIn: return QCoreApplication::translate("lvd::LibraryUi", "built-in");
    }
    return {};
}

const LibraryManager::Entry *effectiveEntry(const LibraryManager &libraries, const QString &name)
{
    for (const LibraryManager::Entry &entry : libraries.entries()) {
        if (!entry.shadowed && entry.library.name == name)
            return &entry;
    }
    return nullptr;
}

const LibraryManager::Entry *entryInScope(const LibraryManager &libraries, const QString &name, LibraryScope scope)
{
    for (const LibraryManager::Entry &entry : libraries.entries()) {
        if (entry.scope == scope && entry.library.name == name)
            return &entry;
    }
    return nullptr;
}

const LibraryManager::Entry *shadowingEntry(const LibraryManager &libraries, const LibraryManager::Entry &entry)
{
    if (!entry.shadowed)
        return nullptr;
    const LibraryManager::Entry *winner = effectiveEntry(libraries, entry.library.name);
    return winner != &entry ? winner : nullptr;
}

bool validateLibraryName(const QString &name, QString *error)
{
    if (name.trimmed().isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("lvd::LibraryUi", "Enter a library name.");
        return false;
    }
    if (name.contains(QLatin1Char(':'))) {
        if (error)
            *error = QCoreApplication::translate("lvd::LibraryUi", "A library name cannot contain \":\".");
        return false;
    }
    if (name == QLatin1String("sheet")) {
        if (error)
            *error = QCoreApplication::translate("lvd::LibraryUi", "\"sheet\" is reserved for schematic sheets.");
        return false;
    }
    // The name becomes the file name, so the project naming rules apply.
    return Project::validateName(name, error);
}

namespace {

QRectF pinLabelRect(const SymbolPin &pin, const QTransform &transform, const Orientation &orientation)
{
    QPointF vector;
    switch (pin.direction) {
    case PinDirection::Left: vector = QPointF(-1, 0); break;
    case PinDirection::Right: vector = QPointF(1, 0); break;
    case PinDirection::Up: vector = QPointF(0, -1); break;
    case PinDirection::Down: vector = QPointF(0, 1); break;
    }
    const QPointF at = transform.map(QPointF(pin.at) + vector * (pin.length + 3));
    const double width = pin.pin.size() * 6 * 0.62;
    const double height = 7;
    const bool horizontal = pin.direction == PinDirection::Left || pin.direction == PinDirection::Right;
    if (horizontal) {
        const PinDirection mapped = orientation.map(pin.direction);
        if (mapped == PinDirection::Right)
            return QRectF(at.x(), at.y() - height / 2, width, height);
        if (mapped == PinDirection::Left)
            return QRectF(at.x() - width, at.y() - height / 2, width, height);
    }
    return QRectF(at.x() - width / 2, at.y() - height / 2, width, height);
}

} // namespace

QRectF symbolBoundsWithPinNames(const Symbol &symbol, const Orientation &orientation)
{
    const QTransform transform = symbolTransform(QPointF(), orientation);
    QRectF bounds = transform.mapRect(SymbolPainter::boundingRect(symbol));
    for (const SymbolPin &pin : symbol.pins)
        bounds = bounds.united(pinLabelRect(pin, transform, orientation));
    return bounds;
}

void paintSymbolFitted(QPainter &painter, const Symbol &symbol, const QRectF &target, const SchematicStyle &style,
                       const Orientation &orientation, bool showPinNames, double maximumScale)
{
    const QRectF bounds = showPinNames ? symbolBoundsWithPinNames(symbol, orientation)
                                       : symbolTransform(QPointF(), orientation).mapRect(SymbolPainter::boundingRect(symbol));
    if (bounds.isEmpty() || target.isEmpty())
        return;
    double scale = std::min(target.width() / bounds.width(), target.height() / bounds.height()) * 0.92;
    if (maximumScale > 0)
        scale = std::min(scale, maximumScale);
    painter.save();
    painter.translate(target.center());
    painter.scale(scale, scale);
    painter.translate(-bounds.center());
    painter.setTransform(symbolTransform(QPointF(), orientation), true);
    SymbolPainter::Options options;
    options.orientation = orientation;
    options.showPinNames = showPinNames;
    SymbolPainter(style).paint(painter, symbol, options);
    painter.restore();
}

QPixmap symbolPixmap(const Symbol &symbol, QSize size, const SchematicStyle &style, bool showPinNames,
                     double devicePixelRatio, double maximumScale)
{
    QPixmap pixmap(size * devicePixelRatio);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paintSymbolFitted(painter, symbol, QRectF(QPointF(0, 0), QSizeF(size)), style, {}, showPinNames, maximumScale);
    return pixmap;
}

SchematicStyle styleFor(const QWidget *widget)
{
    return SchematicStyle::fromPalette(widget->palette());
}

} // namespace lvd

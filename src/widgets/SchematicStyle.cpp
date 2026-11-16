#include "widgets/SchematicStyle.h"

#include <QPalette>

namespace lvd {

QFont SchematicStyle::font(double size) const
{
    QFont font(QStringLiteral("Segoe UI"));
    font.setStyleHint(QFont::SansSerif);
    font.setPointSizeF(size);
    return font;
}

SchematicStyle SchematicStyle::light()
{
    SchematicStyle style;
    style.background = QColor(0xFC, 0xFC, 0xFA);
    style.grid = QColor(0xC8, 0xC8, 0xC4);
    style.symbol = QColor(0x8B, 0x1A, 0x1A);
    style.symbolFill = QColor(0xFF, 0xF8, 0xE7);
    style.pin = QColor(0x8B, 0x1A, 0x1A);
    style.pinName = QColor(0x3C, 0x3C, 0x3C);
    style.wire = QColor(0x1F, 0x6F, 0x3A);
    style.junction = QColor(0x1F, 0x6F, 0x3A);
    style.label = QColor(0x1A, 0x4E, 0x9A);
    style.text = QColor(0x2B, 0x2B, 0x2B);
    style.reference = QColor(0x1A, 0x4E, 0x9A);
    style.value = QColor(0x50, 0x50, 0x50);
    style.selection = QColor(0xE0, 0x8A, 0x00);
    style.highlight = QColor(0xD0, 0x3A, 0xC0);
    style.error = QColor(0xD0, 0x2C, 0x2C);
    style.warning = QColor(0xD8, 0x8C, 0x00);
    style.missing = QColor(0xD0, 0x2C, 0x2C);
    return style;
}

SchematicStyle SchematicStyle::dark()
{
    SchematicStyle style;
    style.background = QColor(0x1E, 0x1F, 0x22);
    style.grid = QColor(0x48, 0x4A, 0x4F);
    style.symbol = QColor(0xE8, 0x8A, 0x7A);
    style.symbolFill = QColor(0x33, 0x2C, 0x26);
    style.pin = QColor(0xE8, 0x8A, 0x7A);
    style.pinName = QColor(0xC8, 0xC8, 0xC8);
    style.wire = QColor(0x6F, 0xCF, 0x8A);
    style.junction = QColor(0x6F, 0xCF, 0x8A);
    style.label = QColor(0x7F, 0xB2, 0xF0);
    style.text = QColor(0xDD, 0xDD, 0xDD);
    style.reference = QColor(0x7F, 0xB2, 0xF0);
    style.value = QColor(0xB4, 0xB4, 0xB4);
    style.selection = QColor(0xFF, 0xB0, 0x3B);
    style.highlight = QColor(0xF0, 0x7A, 0xE6);
    style.error = QColor(0xFF, 0x6B, 0x6B);
    style.warning = QColor(0xFF, 0xC1, 0x4D);
    style.missing = QColor(0xFF, 0x6B, 0x6B);
    return style;
}

SchematicStyle SchematicStyle::fromPalette(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128 ? dark() : light();
}

} // namespace lvd

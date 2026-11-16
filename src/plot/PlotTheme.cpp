#include "plot/PlotTheme.h"

#include <QPalette>

namespace lvd {

namespace {

QColor blend(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(static_cast<float>(a.redF() + (b.redF() - a.redF()) * t),
                            static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * t),
                            static_cast<float>(a.blueF() + (b.blueF() - a.blueF()) * t));
}

} // namespace

PlotTheme PlotTheme::fromPalette(const QPalette &palette)
{
    PlotTheme theme;
    const QColor window = palette.color(QPalette::Window);
    theme.dark = window.lightness() < 128;
    theme.surround = window;
    theme.background = palette.color(QPalette::Base);
    // Keep the plot area distinct from the surround but never harsh.
    if (theme.dark && theme.background.lightness() < 20)
        theme.background = QColor(0x1A, 0x1A, 0x19);
    theme.text = palette.color(QPalette::WindowText);
    theme.mutedText = blend(theme.text, window, 0.35);
    theme.axis = blend(theme.background, theme.text, 0.55);
    theme.grid = blend(theme.background, theme.text, theme.dark ? 0.20 : 0.16);
    theme.minorGrid = blend(theme.background, theme.text, theme.dark ? 0.09 : 0.07);
    theme.smithGrid = blend(theme.background, theme.text, theme.dark ? 0.30 : 0.26);
    theme.admittanceGrid = theme.dark ? QColor(0x7A, 0x6A, 0x50) : QColor(0xD9, 0xB8, 0x8C);
    theme.crosshair = blend(theme.background, theme.text, 0.45);
    theme.rubberBand = palette.color(QPalette::Highlight);
    theme.auxiliary = theme.dark ? QColor(0xC3, 0xC2, 0xB7) : QColor(0x52, 0x51, 0x4E);
    theme.tableBackground = theme.background;
    theme.tableBackground.setAlpha(225);

    if (theme.dark) {
        theme.series = {QColor(0x39, 0x87, 0xE5), QColor(0xD9, 0x59, 0x26), QColor(0x19, 0x9E, 0x70),
                        QColor(0xC9, 0x85, 0x00), QColor(0xD5, 0x51, 0x81), QColor(0x0C, 0xA3, 0x0C),
                        QColor(0x90, 0x85, 0xE9), QColor(0xE6, 0x67, 0x67)};
    } else {
        theme.series = {QColor(0x2A, 0x78, 0xD6), QColor(0xEB, 0x68, 0x34), QColor(0x1B, 0xAF, 0x7A),
                        QColor(0xED, 0xA1, 0x00), QColor(0xE8, 0x7B, 0xA4), QColor(0x00, 0x83, 0x00),
                        QColor(0x62, 0x50, 0xD6), QColor(0xE3, 0x49, 0x48)};
    }
    return theme;
}

} // namespace lvd

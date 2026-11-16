#include "schematic_editor/EditorIcons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>

#include <cmath>
#include <optional>

namespace lvd {

namespace {

std::optional<QIcon::ThemeIcon> themeIcon(EditorIcon icon)
{
    switch (icon) {
    case EditorIcon::Undo: return QIcon::ThemeIcon::EditUndo;
    case EditorIcon::Redo: return QIcon::ThemeIcon::EditRedo;
    case EditorIcon::Cut: return QIcon::ThemeIcon::EditCut;
    case EditorIcon::Copy: return QIcon::ThemeIcon::EditCopy;
    case EditorIcon::Paste: return QIcon::ThemeIcon::EditPaste;
    case EditorIcon::Delete: return QIcon::ThemeIcon::EditDelete;
    case EditorIcon::SelectAll: return QIcon::ThemeIcon::EditSelectAll;
    case EditorIcon::ZoomFit: return QIcon::ThemeIcon::ZoomFitBest;
    case EditorIcon::Add: return QIcon::ThemeIcon::ListAdd;
    case EditorIcon::Remove: return QIcon::ThemeIcon::ListRemove;
    case EditorIcon::Up: return QIcon::ThemeIcon::GoUp;
    case EditorIcon::Down: return QIcon::ThemeIcon::GoDown;
    case EditorIcon::Browse: return QIcon::ThemeIcon::DocumentOpen;
    default: return std::nullopt;
    }
}

void arrowHead(QPainter &p, QPointF tip, QPointF direction)
{
    const QPointF d = direction / std::hypot(direction.x(), direction.y());
    const QPointF n(-d.y(), d.x());
    QPolygonF head{tip, tip - d * 7 + n * 5, tip - d * 7 - n * 5};
    p.save();
    p.setBrush(p.pen().color());
    p.drawPolygon(head);
    p.restore();
}

void draw(QPainter &p, EditorIcon icon, const QColor &color, const QColor &accent)
{
    QPen pen(color, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (icon) {
    case EditorIcon::Undo:
    case EditorIcon::Redo: {
        const bool undo = icon == EditorIcon::Undo;
        QPainterPath path;
        path.moveTo(undo ? 8 : 24, 12);
        path.lineTo(undo ? 20 : 12, 12);
        path.cubicTo(undo ? 28 : 4, 12, undo ? 28 : 4, 26, undo ? 18 : 14, 26);
        p.drawPath(path);
        arrowHead(p, QPointF(undo ? 5 : 27, 12), QPointF(undo ? -1 : 1, 0));
        break;
    }
    case EditorIcon::Cut:
        p.drawEllipse(QPointF(10, 24), 4, 4);
        p.drawEllipse(QPointF(22, 24), 4, 4);
        p.drawLine(QPointF(12, 21), QPointF(22, 5));
        p.drawLine(QPointF(20, 21), QPointF(10, 5));
        break;
    case EditorIcon::Copy:
        p.drawRect(QRectF(5, 5, 15, 17));
        p.setBrush(QColor(0, 0, 0, 0));
        p.drawRect(QRectF(12, 11, 15, 17));
        break;
    case EditorIcon::Paste:
        p.drawRoundedRect(QRectF(6, 6, 20, 22), 2, 2);
        p.drawRect(QRectF(11, 3, 10, 6));
        p.drawLine(QPointF(11, 16), QPointF(21, 16));
        p.drawLine(QPointF(11, 21), QPointF(18, 21));
        break;
    case EditorIcon::Duplicate:
        p.drawRect(QRectF(4, 4, 15, 15));
        p.drawRect(QRectF(13, 13, 15, 15));
        p.drawLine(QPointF(20.5, 17), QPointF(20.5, 24));
        p.drawLine(QPointF(17, 20.5), QPointF(24, 20.5));
        break;
    case EditorIcon::Delete:
        p.setPen(QPen(accent, 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(8, 8), QPointF(24, 24));
        p.drawLine(QPointF(24, 8), QPointF(8, 24));
        break;
    case EditorIcon::Rotate: {
        p.drawArc(QRectF(6, 6, 20, 20), 90 * 16, 270 * 16);
        arrowHead(p, QPointF(16, 6), QPointF(-1, 0));
        break;
    }
    case EditorIcon::Mirror:
        p.setPen(QPen(color, 1.6, Qt::DashLine));
        p.drawLine(QPointF(16, 3), QPointF(16, 29));
        p.setPen(pen);
        p.drawPolygon(QPolygonF{QPointF(13, 8), QPointF(13, 24), QPointF(4, 24)});
        p.setBrush(accent);
        p.drawPolygon(QPolygonF{QPointF(19, 8), QPointF(19, 24), QPointF(28, 24)});
        break;
    case EditorIcon::SelectAll:
        p.setPen(QPen(color, 2, Qt::DashLine));
        p.drawRect(QRectF(4, 4, 24, 24));
        p.setPen(pen);
        p.setBrush(accent);
        p.drawRect(QRectF(10, 10, 12, 12));
        break;
    case EditorIcon::Select:
        p.setBrush(color);
        p.drawPolygon(QPolygonF{QPointF(9, 4), QPointF(9, 25), QPointF(14, 20), QPointF(18, 28), QPointF(21, 27),
                                QPointF(17, 19), QPointF(24, 19)});
        break;
    case EditorIcon::Wire:
        p.setPen(QPen(accent, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF{QPointF(5, 8), QPointF(18, 8), QPointF(18, 24), QPointF(27, 24)});
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(5, 8), 3, 3);
        p.drawEllipse(QPointF(27, 24), 3, 3);
        break;
    case EditorIcon::Label:
        p.setPen(QPen(accent, 2.4));
        p.drawLine(QPointF(3, 26), QPointF(29, 26));
        p.setPen(pen);
        p.drawPolygon(QPolygonF{QPointF(6, 21), QPointF(6, 9), QPointF(22, 9), QPointF(28, 15), QPointF(22, 21)});
        p.drawLine(QPointF(6, 21), QPointF(6, 26));
        break;
    case EditorIcon::Text:
        p.setPen(QPen(color, 3, Qt::SolidLine, Qt::SquareCap));
        p.drawLine(QPointF(7, 7), QPointF(25, 7));
        p.drawLine(QPointF(16, 7), QPointF(16, 26));
        p.drawLine(QPointF(12, 26), QPointF(20, 26));
        break;
    case EditorIcon::Highlight:
        p.setPen(QPen(accent, 3.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF{QPointF(4, 10), QPointF(16, 10), QPointF(16, 24), QPointF(28, 24)});
        p.drawLine(QPointF(16, 10), QPointF(28, 10));
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(QPointF(16, 10), 3.4, 3.4);
        break;
    case EditorIcon::ZoomFit:
        p.drawEllipse(QPointF(13, 13), 8, 8);
        p.setPen(QPen(color, 3.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(19, 19), QPointF(27, 27));
        p.setPen(QPen(accent, 1.6));
        p.drawRect(QRectF(9.5, 9.5, 7, 7));
        break;
    case EditorIcon::RuleCheck:
        p.drawRoundedRect(QRectF(4, 4, 24, 24), 3, 3);
        p.setPen(QPen(accent, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF{QPointF(9, 16), QPointF(14, 21), QPointF(23, 10)});
        break;
    case EditorIcon::Reset:
        p.drawArc(QRectF(7, 7, 18, 18), 120 * 16, 280 * 16);
        arrowHead(p, QPointF(9, 10), QPointF(-0.5, 1));
        break;
    case EditorIcon::Browse:
        p.drawPolyline(QPolygonF{QPointF(4, 26), QPointF(4, 8), QPointF(12, 8), QPointF(15, 11), QPointF(27, 11),
                                 QPointF(27, 26), QPointF(4, 26)});
        break;
    case EditorIcon::Add:
        p.drawLine(QPointF(16, 6), QPointF(16, 26));
        p.drawLine(QPointF(6, 16), QPointF(26, 16));
        break;
    case EditorIcon::Remove: p.drawLine(QPointF(6, 16), QPointF(26, 16)); break;
    case EditorIcon::Up:
    case EditorIcon::Down: {
        const bool up = icon == EditorIcon::Up;
        p.drawLine(QPointF(16, up ? 26 : 6), QPointF(16, up ? 10 : 22));
        arrowHead(p, QPointF(16, up ? 5 : 27), QPointF(0, up ? -1 : 1));
        break;
    }
    }
}

} // namespace

QIcon editorIcon(EditorIcon icon, const QPalette &palette)
{
    if (const auto name = themeIcon(icon)) {
        const QIcon themed = QIcon::fromTheme(*name);
        if (!themed.isNull())
            return themed;
    }
    const QColor color = palette.color(QPalette::WindowText);
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    const QColor accent = icon == EditorIcon::Delete ? QColor(dark ? 0xFF6B6B : 0xD02C2C)
                                                     : QColor(dark ? 0x6FCF8A : 0x1F6F3A);
    QIcon result;
    for (const int size : {16, 24, 32, 64}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 32.0, size / 32.0);
        draw(painter, icon, color, accent);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

} // namespace lvd

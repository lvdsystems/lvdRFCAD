#include "matching/MatchLadderView.h"

#include "core/Expression.h"
#include "widgets/SchematicStyle.h"

#include <QCoreApplication>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace lvd {

using namespace lvd::rf;

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("lvd::MatchLadderView", text);
}

QString valueText(const MatchElement &e)
{
    switch (e.kind) {
    case MatchElementKind::Capacitor: return formatEngineering(e.value, QStringLiteral("F"), 4);
    case MatchElementKind::Inductor: return formatEngineering(e.value, QStringLiteral("H"), 4);
    case MatchElementKind::Line:
    case MatchElementKind::OpenStub:
    case MatchElementKind::ShortStub:
        return QStringLiteral("%1\n%2°").arg(formatEngineering(e.z0, QStringLiteral("Ω"), 4),
                                            QString::number(e.lengthDeg, 'f', 2));
    }
    return {};
}

QString nameText(const MatchElement &e, int index)
{
    switch (e.kind) {
    case MatchElementKind::Capacitor: return QStringLiteral("C%1").arg(index);
    case MatchElementKind::Inductor: return QStringLiteral("L%1").arg(index);
    case MatchElementKind::Line: return QStringLiteral("TL%1").arg(index);
    case MatchElementKind::OpenStub: return QStringLiteral("TL%1 (%2)").arg(index).arg(tr("open"));
    case MatchElementKind::ShortStub: return QStringLiteral("TL%1 (%2)").arg(index).arg(tr("short"));
    }
    return {};
}

// Glyphs in local coordinates: horizontal from (-20, 0) to (20, 0).
void capacitorGlyph(QPainter &p)
{
    p.drawLine(QPointF(-20, 0), QPointF(-4, 0));
    p.drawLine(QPointF(4, 0), QPointF(20, 0));
    p.drawLine(QPointF(-4, -11), QPointF(-4, 11));
    p.drawLine(QPointF(4, -11), QPointF(4, 11));
}

void inductorGlyph(QPainter &p)
{
    QPainterPath path(QPointF(-20, 0));
    for (int i = 0; i < 4; ++i)
        path.arcTo(QRectF(-20 + 10 * i, -5, 10, 10), 180, -180);
    p.drawPath(path);
}

void lineGlyph(QPainter &p)
{
    p.drawLine(QPointF(-20, 0), QPointF(-16, 0));
    p.drawLine(QPointF(16, 0), QPointF(20, 0));
    p.drawRect(QRectF(-16, -6, 32, 12));
}

void groundGlyph(QPainter &p, QPointF at)
{
    p.drawLine(at + QPointF(-10, 0), at + QPointF(10, 0));
    p.drawLine(at + QPointF(-6, 4), at + QPointF(6, 4));
    p.drawLine(at + QPointF(-2, 8), at + QPointF(2, 8));
}

} // namespace

QString formatImpedance(std::complex<double> z, int digits)
{
    const double x = z.imag();
    const QString re = formatEngineering(z.real(), QString(), digits);
    if (x == 0)
        return re + QStringLiteral(" Ω");
    const QString im = formatEngineering(std::abs(x), QString(), digits);
    return QStringLiteral("%1 %2 j%3 Ω").arg(re, x < 0 ? QStringLiteral("−") : QStringLiteral("+"), im);
}

MatchLadderView::MatchLadderView(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void MatchLadderView::setSolution(const std::optional<MatchSolution> &solution)
{
    m_solution = solution;
    update();
}

QSize MatchLadderView::sizeHint() const { return QSize(640, 190); }
QSize MatchLadderView::minimumSizeHint() const { return QSize(320, 150); }

void MatchLadderView::paintEvent(QPaintEvent *)
{
    const SchematicStyle style = SchematicStyle::fromPalette(palette());
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), style.background);

    if (!m_solution) {
        p.setPen(style.text);
        p.drawText(rect(), Qt::AlignCenter, tr("No network selected"));
        return;
    }
    const MatchSolution &s = *m_solution;

    QFont font = p.font();
    QFont small = font;
    small.setPointSizeF(std::max(7.0, font.pointSizeF() * 0.9));
    const QFontMetricsF fm(small);

    const double w = width(), h = height();
    const double scale = std::clamp(h / 190.0, 0.8, 1.6);
    const double railY = std::round(std::max(h * 0.34, 16 * scale + fm.lineSpacing() * 3 + 6));
    const double blockW = 96 * scale;
    const double left = 12 + blockW, right = w - 12 - blockW;
    const int n = static_cast<int>(s.elements.size());
    const double slot = (right - left) / std::max(1, n);

    QPen symbolPen(style.symbol, 1.6);
    QPen wirePen(style.wire, 1.6);
    symbolPen.setCosmetic(true);
    wirePen.setCosmetic(true);

    // Source and load blocks.
    const auto block = [&](double x0, const QString &title, const QString &value, bool source) {
        const QRectF box(x0, railY - 18 * scale, blockW - 14 * scale, 36 * scale);
        p.setPen(symbolPen);
        p.setBrush(style.symbolFill);
        if (source) {
            const QPointF c = box.center();
            const double r = 14 * scale;
            p.drawEllipse(c, r, r);
            QPainterPath sine;
            sine.moveTo(c + QPointF(-8 * scale, 0));
            sine.cubicTo(c + QPointF(-4 * scale, -10 * scale), c + QPointF(0, -10 * scale), c);
            sine.cubicTo(c + QPointF(0, 10 * scale), c + QPointF(4 * scale, 10 * scale), c + QPointF(8 * scale, 0));
            p.setBrush(Qt::NoBrush);
            p.drawPath(sine);
            p.setPen(wirePen);
            p.drawLine(QPointF(c.x() + r, railY), QPointF(box.right() + 14 * scale, railY));
        } else {
            p.drawRect(box);
            p.setPen(wirePen);
            p.drawLine(QPointF(x0 - 2, railY), box.topLeft() + QPointF(0, box.height() / 2));
        }
        p.setFont(small);
        p.setPen(style.text);
        const QRectF textBox(x0 - 6, box.bottom() + 6, blockW + 4, 40);
        p.drawText(textBox, Qt::AlignHCenter | Qt::AlignTop, title + QLatin1Char('\n') + value);
    };
    block(12, tr("Source"), QStringLiteral("Zs = %1").arg(formatImpedance(s.zs, 3)), true);
    block(right + 14 * scale, tr("Load"), QStringLiteral("ZL = %1").arg(formatImpedance(s.zl, 3)), false);

    if (n == 0) {
        p.setPen(wirePen);
        p.drawLine(QPointF(left, railY), QPointF(right, railY));
        return;
    }

    int counter = 1;
    for (int i = 0; i < n; ++i) {
        const MatchElement &e = s.elements[static_cast<size_t>(i)];
        const double x0 = left + slot * i, x1 = x0 + slot, xc = (x0 + x1) / 2;
        const QString label = nameText(e, counter++) + QLatin1Char('\n') + valueText(e);
        p.setFont(small);
        if (e.placement == MatchPlacement::Series) {
            const double half = 20 * scale;
            p.setPen(wirePen);
            p.drawLine(QPointF(x0, railY), QPointF(xc - half, railY));
            p.drawLine(QPointF(xc + half, railY), QPointF(x1, railY));
            p.save();
            p.translate(xc, railY);
            p.scale(scale, scale);
            p.setPen(symbolPen);
            p.setBrush(Qt::NoBrush);
            if (e.kind == MatchElementKind::Capacitor)
                capacitorGlyph(p);
            else if (e.kind == MatchElementKind::Inductor)
                inductorGlyph(p);
            else
                lineGlyph(p);
            p.restore();
            p.setPen(style.value);
            const QRectF textBox(x0 + 2, railY - 16 * scale - fm.lineSpacing() * 3, slot - 4, fm.lineSpacing() * 3);
            p.drawText(textBox, Qt::AlignHCenter | Qt::AlignBottom, label);
        } else {
            p.setPen(wirePen);
            p.drawLine(QPointF(x0, railY), QPointF(x1, railY));
            p.setBrush(style.junction);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(xc, railY), 3.2, 3.2);
            const double top = railY + 14 * scale;
            p.setPen(wirePen);
            p.drawLine(QPointF(xc, railY), QPointF(xc, top));
            p.save();
            p.translate(xc, top + 20 * scale);
            p.scale(scale, scale);
            p.setPen(symbolPen);
            p.setBrush(Qt::NoBrush);
            if (e.isLumped()) {
                p.rotate(90);
                if (e.kind == MatchElementKind::Capacitor)
                    capacitorGlyph(p);
                else
                    inductorGlyph(p);
                p.rotate(-90);
                p.drawLine(QPointF(0, 20), QPointF(0, 26));
                groundGlyph(p, QPointF(0, 26));
            } else {
                p.drawRect(QRectF(-5, -20, 10, 36));
                if (e.kind == MatchElementKind::ShortStub) {
                    p.drawLine(QPointF(0, 16), QPointF(0, 22));
                    groundGlyph(p, QPointF(0, 22));
                }
            }
            p.restore();
            p.setPen(style.value);
            const QRectF textBox(xc + 14 * scale, top + 2 * scale, std::max(40.0, slot / 2 - 14 * scale), fm.lineSpacing() * 3);
            p.drawText(textBox, Qt::AlignLeft | Qt::AlignTop, label);
        }
    }
}

} // namespace lvd

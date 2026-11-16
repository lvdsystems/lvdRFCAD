#include "antenna_ui/AntennaUiCommon.h"

#include "plot/PlotWidget.h"
#include "plot3d/Pattern3DView.h"

#include <QEvent>
#include <QLabel>
#include <QWheelEvent>

namespace lvd::antui {

namespace {

// Ignores wheel events on unfocused spin boxes and combo boxes.
class WheelGuard : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Wheel) {
            auto *w = qobject_cast<QWidget *>(watched);
            if (w && !w->hasFocus()) {
                event->ignore();
                return true;
            }
        }
        return false;
    }
};

void guard(QWidget *w)
{
    w->setFocusPolicy(Qt::StrongFocus);
    w->installEventFilter(new WheelGuard(w));
}

} // namespace

QDoubleSpinBox *doubleSpin(double min, double max, int decimals, const QString &suffix, double step)
{
    auto *s = new QDoubleSpinBox;
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setSingleStep(step);
    s->setKeyboardTracking(false);
    if (!suffix.isEmpty())
        s->setSuffix(QLatin1Char(' ') + suffix);
    guard(s);
    return s;
}

QSpinBox *intSpin(int min, int max, const QString &suffix)
{
    auto *s = new QSpinBox;
    s->setRange(min, max);
    s->setKeyboardTracking(false);
    if (!suffix.isEmpty())
        s->setSuffix(QLatin1Char(' ') + suffix);
    guard(s);
    return s;
}

QComboBox *idCombo(const std::vector<std::pair<QString, QString>> &items)
{
    auto *c = new QComboBox;
    for (const auto &[id, label] : items)
        c->addItem(label, id);
    guard(c);
    return c;
}

void setComboId(QComboBox *combo, const QString &id)
{
    const int i = combo->findData(id);
    if (i >= 0)
        combo->setCurrentIndex(i);
}

QString comboId(const QComboBox *combo)
{
    return combo->currentData().toString();
}

QLabel *noteLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

ContentCommand::ContentCommand(const QString &text, QJsonObject before, QJsonObject after, int mergeKey,
                               std::function<void(const QJsonObject &)> apply)
    : QUndoCommand(text), m_before(std::move(before)), m_after(std::move(after)), m_mergeKey(mergeKey), m_apply(std::move(apply))
{
}

void ContentCommand::undo()
{
    m_apply(m_before);
}

void ContentCommand::redo()
{
    // The first redo happens when pushed; the content is already applied.
    if (m_first) {
        m_first = false;
        return;
    }
    m_apply(m_after);
}

bool ContentCommand::mergeWith(const QUndoCommand *other)
{
    const auto *o = static_cast<const ContentCommand *>(other);
    if (m_mergeKey < 0 || o->m_mergeKey != m_mergeKey)
        return false;
    m_after = o->m_after;
    return true;
}

std::vector<double> cutValuesDb(const ant::PatternCut &cut, ant::PatternComponent component, ant::CoPolarisation co)
{
    std::vector<double> v(cut.fields.size());
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = ant::powerDb(ant::componentPower(cut.fields[i], ant::toRadians(cut.phi[i]), component, co));
    return v;
}

double plotCuts(PlotWidget *plot, const std::vector<CutTrace> &cuts, ant::PatternComponent component, ant::CoPolarisation co,
                bool polar, double dynamicRangeDb, const QString &unit, bool compass)
{
    plot->clear();
    plot->setKind(polar ? PlotKind::Polar : PlotKind::Rectangular);
    double peak = -400;
    std::vector<std::pair<int, std::vector<double>>> data;
    int slot = 0;
    std::vector<int> colours;
    for (const CutTrace &c : cuts) {
        if (!c.cut || c.cut->fields.empty())
            continue;
        std::vector<double> y = cutValuesDb(*c.cut, component, co);
        for (double v : y)
            peak = std::max(peak, v);
        TraceStyle style;
        style.lineWidth = c.dashed ? 1.2 : 2.0;
        if (c.dashed)
            style.lineStyle = Qt::DashLine;
        const int id = plot->addTrace(c.name, unit, style);
        if (!c.dashed)
            ++slot;
        data.emplace_back(id, std::move(y));
        colours.push_back(id);
    }
    const double floor = peak - dynamicRangeDb;
    std::size_t k = 0;
    for (const CutTrace &c : cuts) {
        if (!c.cut || c.cut->fields.empty())
            continue;
        std::vector<double> y = data[k].second;
        for (double &v : y)
            v = std::max(v, floor);
        plot->setTraceData(data[k].first, c.cut->angle, y);
        ++k;
    }
    if (polar) {
        plot->setPolarOrientation(compass ? PolarOrientation::Compass : PolarOrientation::Mathematical);
        const double top = std::ceil(peak / 5.0) * 5.0;
        plot->setPolarScale(PolarScale::Decibel, top - dynamicRangeDb); // the floor is absolute
        plot->setPolarMaximum(top);
    } else {
        plot->setXAxis(QObject::tr("Angle"), QStringLiteral("deg"));
        plot->setYAxis(QObject::tr("Level"), unit);
        double lo = 180, hi = -180;
        for (const CutTrace &c : cuts)
            if (c.cut && !c.cut->angle.empty()) {
                lo = std::min(lo, c.cut->angle.front());
                hi = std::max(hi, c.cut->angle.back());
            }
        plot->setXRange(lo < hi ? lo : -180, lo < hi ? hi : 180);
        plot->setYRange(std::floor(floor / 5.0) * 5.0, std::ceil((peak + 1) / 5.0) * 5.0);
    }
    return peak;
}

void show3d(Pattern3DView *view, const ant::SphericalGrid &grid, ant::PatternComponent component, ant::CoPolarisation co,
            double dynamicRangeDb, const QString &label, const QString &unit)
{
    SphericalPattern p;
    p.theta = grid.theta;
    p.phi = grid.phi;
    p.valuesDb.resize(grid.fields.size());
    for (std::size_t i = 0; i < grid.theta.size(); ++i)
        for (std::size_t j = 0; j < grid.phi.size(); ++j) {
            const std::size_t k = i * grid.phi.size() + j;
            p.valuesDb[k] = ant::powerDb(ant::componentPower(grid.fields[k], ant::toRadians(grid.phi[j]), component, co));
        }
    view->setColorBar(label, unit);
    view->setPattern(p, dynamicRangeDb);
}

QString formatDb(double value, const QString &unit, int decimals)
{
    if (!std::isfinite(value))
        return QStringLiteral("-");
    return QStringLiteral("%1 %2").arg(value, 0, 'f', decimals).arg(unit);
}

QString formatFrequency(double hz)
{
    if (hz >= 1e9)
        return QStringLiteral("%1 GHz").arg(hz / 1e9, 0, 'g', 6);
    if (hz >= 1e6)
        return QStringLiteral("%1 MHz").arg(hz / 1e6, 0, 'g', 6);
    return QStringLiteral("%1 kHz").arg(hz / 1e3, 0, 'g', 6);
}

} // namespace lvd::antui

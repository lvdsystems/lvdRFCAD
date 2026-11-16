#include "spice/SpiceData.h"

namespace lvd::spice {

QString vectorKindUnit(VectorKind kind)
{
    switch (kind) {
    case VectorKind::Time:
        return QStringLiteral("s");
    case VectorKind::Frequency:
        return QStringLiteral("Hz");
    case VectorKind::Voltage:
        return QStringLiteral("V");
    case VectorKind::Current:
        return QStringLiteral("A");
    case VectorKind::VoltageDensity:
        return QStringLiteral("V/√Hz");
    case VectorKind::CurrentDensity:
        return QStringLiteral("A/√Hz");
    case VectorKind::Temperature:
        return QStringLiteral("°C");
    case VectorKind::Impedance:
        return QStringLiteral("Ω");
    case VectorKind::Admittance:
        return QStringLiteral("S");
    case VectorKind::Power:
        return QStringLiteral("W");
    case VectorKind::Pole:
    case VectorKind::Zero:
        return QStringLiteral("rad/s"); // complex frequency s
    case VectorKind::Other:
        break;
    }
    return {};
}

QString normalisedVectorName(const QString &name, VectorKind kind, const QString &plotType)
{
    QString lower = name.toLower();
    const bool nodeVectors = plotType == QLatin1String("op") || plotType == QLatin1String("dc")
                             || plotType == QLatin1String("tran") || plotType == QLatin1String("ac");
    // Raw files put every voltage-type vector in v(), also the sweep scale
    // and results such as "onoise_total" or "pole(1)" that are not nodes.
    if (lower.startsWith(QLatin1String("v(")) && lower.endsWith(QLatin1Char(')'))) {
        const QString inner = lower.mid(2, lower.size() - 3);
        if (!nodeVectors || inner.endsWith(QLatin1String("-sweep")) || inner.contains(QLatin1Char('#')))
            lower = inner;
    }
    if (plotType == QLatin1String("tf") && lower.endsWith(QLatin1String("#input_impedance")))
        return QStringLiteral("input_impedance");
    if (plotType == QLatin1String("tf") && lower.startsWith(QLatin1String("output_impedance")))
        return QStringLiteral("output_impedance");
    if (lower.endsWith(QLatin1String("#branch")))
        return QStringLiteral("i(%1)").arg(lower.chopped(7));
    // XSPICE devices: "a1#branch_1_0" for their first port.
    if (lower.endsWith(QLatin1String("#branch_1_0")))
        return QStringLiteral("i(%1)").arg(lower.chopped(11));
    if (nodeVectors && kind == VectorKind::Voltage && !lower.contains(QLatin1Char('(')) && !lower.startsWith(QLatin1Char('@'))
        && !lower.endsWith(QLatin1String("-sweep")) && !lower.contains(QLatin1Char('#')))
        return QStringLiteral("v(%1)").arg(lower);
    return lower;
}

const SpiceVector *SpicePlot::vector(const QString &vectorName) const
{
    for (const SpiceVector &v : vectors)
        if (v.name.compare(vectorName, Qt::CaseInsensitive) == 0)
            return &v;
    return nullptr;
}

const SpiceVector *SpicePlot::scale() const
{
    return scaleName.isEmpty() ? nullptr : vector(scaleName);
}

QStringList SpicePlot::vectorNames() const
{
    QStringList names;
    names.reserve(int(vectors.size()));
    for (const SpiceVector &v : vectors)
        names << v.name;
    return names;
}

const SpicePlot *SpiceRunResult::plot(const QString &plotType) const
{
    for (const SpicePlot &p : plots)
        if (p.type == plotType)
            return &p;
    return nullptr;
}

} // namespace lvd::spice

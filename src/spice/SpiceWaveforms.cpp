#include "spice/SpiceWaveforms.h"

#include "spice/SpiceNetlister.h"

#include <QRegularExpression>

namespace lvd::spice {

Waveform waveformOf(const SpicePlot &plot, const SpiceVector &vector)
{
    Waveform w;
    const SpiceVector *scale = plot.scale();
    const std::size_t n = vector.size();
    w.x.resize(n);
    if (scale && scale->size() == n && !scale->isComplex) {
        w.x = scale->real;
    } else {
        for (std::size_t i = 0; i < n; ++i)
            w.x[i] = double(i);
    }
    w.isComplex = vector.isComplex;
    w.y.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        w.y[i] = vector.at(i);
    w.unit = vectorKindUnit(vector.kind);
    return w;
}

VectorExpression::Resolver plotResolver(const SpicePlot &plot, const SpiceDeck *deck)
{
    return [&plot, deck](const QString &name) -> std::optional<Waveform> {
        if (const SpiceVector *v = plot.vector(name))
            return waveformOf(plot, *v);
        if (!deck)
            return std::nullopt;
        static const QRegularExpression call(QStringLiteral("^([vi])\\((.*)\\)$"));
        const QRegularExpressionMatch match = call.match(name);
        if (!match.hasMatch())
            return std::nullopt;
        const QString argument = match.captured(2).trimmed();
        if (match.captured(1) == QLatin1String("v")) {
            const QString node = deck->node(argument);
            if (node == QLatin1String("0")) {
                // Ground: a zero trace over the scale.
                if (const SpiceVector *scale = plot.scale()) {
                    Waveform w = waveformOf(plot, *scale);
                    std::fill(w.y.begin(), w.y.end(), Complex(0.0, 0.0));
                    w.isComplex = false;
                    w.unit = QStringLiteral("V");
                    return w;
                }
                return std::nullopt;
            }
            if (!node.isEmpty())
                if (const SpiceVector *v = plot.vector(QStringLiteral("v(%1)").arg(node)))
                    return waveformOf(plot, *v);
            return std::nullopt;
        }
        if (const SpiceElement *element = deck->element(argument)) {
            for (const QString &elementName : element->elementNames)
                if (const SpiceVector *v = plot.vector(QStringLiteral("i(%1)").arg(elementName)))
                    return waveformOf(plot, *v);
        }
        return std::nullopt;
    };
}

std::optional<Waveform> evaluateOnPlot(const QString &expression, const SpicePlot &plot, const SpiceDeck *deck, QString *error)
{
    const auto parsed = VectorExpression::parse(expression, error);
    if (!parsed)
        return std::nullopt;
    return parsed->evaluate(plotResolver(plot, deck), error);
}

} // namespace lvd::spice

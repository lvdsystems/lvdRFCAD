#include "sparam_ui/SParamFormat.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>

#include <cmath>

namespace lvd::sparam {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("lvd::TouchstoneEditor", text);
}

struct Prefix {
    const char *unit;
    double scale;
};

constexpr Prefix kUnits[] = {{"GHz", 1e9}, {"MHz", 1e6}, {"kHz", 1e3}, {"Hz", 1.0}};

std::optional<double> toDouble(const QString &text)
{
    bool ok = false;
    double value = text.toDouble(&ok);
    if (!ok)
        value = QLocale().toDouble(text, &ok);
    if (!ok || !std::isfinite(value))
        return std::nullopt;
    return value;
}

} // namespace

QString frequencyUnitFor(double maximumHz, double *scale)
{
    const double m = std::abs(maximumHz);
    for (const Prefix &p : kUnits) {
        if (m >= p.scale || p.scale == 1.0) {
            if (scale)
                *scale = p.scale;
            return QString::fromLatin1(p.unit);
        }
    }
    if (scale)
        *scale = 1.0;
    return QStringLiteral("Hz");
}

double frequencyUnitScale(const QString &unit)
{
    for (const Prefix &p : kUnits) {
        if (unit.compare(QLatin1String(p.unit), Qt::CaseInsensitive) == 0)
            return p.scale;
    }
    return 0;
}

QString formatFrequency(double hz, int digits)
{
    double scale = 1;
    const QString unit = frequencyUnitFor(hz, &scale);
    return QStringLiteral("%1 %2").arg(QLocale().toString(hz / scale, 'g', digits), unit);
}

std::optional<double> parseFrequency(const QString &text, double defaultScale)
{
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*([-+]?[0-9]*[.,]?[0-9]+(?:[eE][-+]?[0-9]+)?)\\s*([kKmMgGtT]?)\\s*(?:[hH][zZ])?\\s*$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    const std::optional<double> number = toDouble(match.captured(1));
    if (!number)
        return std::nullopt;
    const QString prefix = match.captured(2).toLower();
    const bool hasHz = text.contains(QLatin1String("hz"), Qt::CaseInsensitive);
    double scale = hasHz ? 1.0 : defaultScale;
    if (prefix == QLatin1String("k"))
        scale = 1e3;
    else if (prefix == QLatin1String("m"))
        scale = 1e6; // "m" means mega for frequencies (no millihertz in RF)
    else if (prefix == QLatin1String("g"))
        scale = 1e9;
    else if (prefix == QLatin1String("t"))
        scale = 1e12;
    return *number * scale;
}

std::optional<std::vector<double>> parseFrequencyList(const QString &text, double defaultScale, QString *error)
{
    static const QRegularExpression separators(QStringLiteral("[\\s;]+"));
    // Commas separate entries unless the locale uses them as decimal point.
    QString normalised = text;
    if (QLocale().decimalPoint() != QLatin1String(","))
        normalised.replace(QLatin1Char(','), QLatin1Char(' '));
    std::vector<double> result;
    // Keep "1.5 GHz" together: join a number with a following unit token.
    const QStringList tokens = normalised.split(separators, Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        QString token = tokens[i];
        if (i + 1 < tokens.size() && !tokens[i + 1].isEmpty() && tokens[i + 1].at(0).isLetter()) {
            token += tokens[i + 1];
            ++i;
        }
        const std::optional<double> value = parseFrequency(token, defaultScale);
        if (!value) {
            if (error)
                *error = tr("Not a frequency: \"%1\".").arg(token);
            return std::nullopt;
        }
        result.push_back(*value);
    }
    if (result.empty()) {
        if (error)
            *error = tr("The frequency list is empty.");
        return std::nullopt;
    }
    return result;
}

std::vector<double> linearGrid(double start, double stop, int points)
{
    std::vector<double> grid;
    if (points <= 0)
        return grid;
    if (points == 1)
        return {start};
    grid.reserve(static_cast<std::size_t>(points));
    for (int i = 0; i < points; ++i)
        grid.push_back(start + (stop - start) * i / (points - 1));
    grid.back() = stop;
    return grid;
}

std::vector<double> logGrid(double start, double stop, int points)
{
    std::vector<double> grid;
    if (points <= 0 || !(start > 0) || !(stop > 0))
        return grid;
    if (points == 1)
        return {start};
    grid.reserve(static_cast<std::size_t>(points));
    const double a = std::log10(start);
    const double b = std::log10(stop);
    for (int i = 0; i < points; ++i)
        grid.push_back(std::pow(10.0, a + (b - a) * i / (points - 1)));
    grid.front() = start;
    grid.back() = stop;
    return grid;
}

QString parameterName(const QString &letter, int row, int column, int portCount)
{
    if (portCount >= 10)
        return QStringLiteral("%1%2,%3").arg(letter).arg(row + 1).arg(column + 1);
    return QStringLiteral("%1%2%3").arg(letter).arg(row + 1).arg(column + 1);
}

bool allEqual(const std::vector<double> &values)
{
    for (const double v : values) {
        if (v != values.front())
            return false;
    }
    return true;
}

QString referenceImpedanceText(const std::vector<double> &z0)
{
    if (z0.empty())
        return QString();
    const QLocale locale;
    if (allEqual(z0)) {
        if (z0.size() == 1)
            return tr("%1 Ω").arg(locale.toString(z0.front(), 'g', 8));
        return tr("%1 Ω (all ports)").arg(locale.toString(z0.front(), 'g', 8));
    }
    QStringList parts;
    for (std::size_t i = 0; i < z0.size(); ++i)
        parts << tr("P%1 %2 Ω").arg(i + 1).arg(locale.toString(z0[i], 'g', 8));
    return parts.join(QStringLiteral(", "));
}

QString touchstoneExtension(int portCount)
{
    return QStringLiteral(".s%1p").arg(portCount);
}

bool extensionMatches(const QString &path, int portCount, int version)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (version >= 2 && suffix == QLatin1String("ts"))
        return true;
    return suffix == QStringLiteral("s%1p").arg(portCount);
}

QString withMatchingExtension(const QString &path, int portCount)
{
    const QFileInfo info(path);
    const QString suffix = info.suffix();
    QString base = path;
    if (!suffix.isEmpty())
        base.chop(suffix.size() + 1);
    return base + touchstoneExtension(portCount);
}

QString touchstoneFormatName(rf::TouchstoneFormat format)
{
    switch (format) {
    case rf::TouchstoneFormat::RealImaginary:
        return tr("Real / imaginary (RI)");
    case rf::TouchstoneFormat::MagnitudeAngle:
        return tr("Magnitude / angle (MA)");
    case rf::TouchstoneFormat::DecibelAngle:
        return tr("dB / angle (DB)");
    }
    return QString();
}

} // namespace lvd::sparam

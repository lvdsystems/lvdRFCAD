#include "rfsim/PhaseNoise.h"

#include "rfdata/RfTypes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <numeric>

namespace lvd::rf {

namespace {

double fromDb(double db)
{
    return std::pow(10.0, db / 10.0);
}

double toDb(double linear)
{
    return linear > 0 ? 10.0 * std::log10(linear) : -INFINITY;
}

// ∫ from a to b of l0 (f / x0)^s df, with l0 linear.
double powerLawIntegral(double l0, double x0, double s, double a, double b)
{
    if (!(b > a) || !(l0 > 0))
        return 0;
    if (std::abs(s + 1.0) < 1e-9)
        return l0 * x0 * std::log(b / a);
    return l0 * x0 * (std::pow(b / x0, s + 1.0) - std::pow(a / x0, s + 1.0)) / (s + 1.0);
}

} // namespace

double PhaseNoiseProfile::level(double offset) const
{
    if (offsets.empty())
        return -INFINITY;
    if (offset <= offsets.front())
        return levels.front();
    if (offset >= offsets.back())
        return levels.back();
    const auto it = std::upper_bound(offsets.begin(), offsets.end(), offset);
    const size_t k = static_cast<size_t>(it - offsets.begin());
    const double x0 = offsets[k - 1], x1 = offsets[k];
    const double t = std::log10(offset / x0) / std::log10(x1 / x0);
    return levels[k - 1] + t * (levels[k] - levels[k - 1]);
}

PhaseNoiseProfile PhaseNoiseProfile::shifted(double db) const
{
    PhaseNoiseProfile result = *this;
    for (double &level : result.levels)
        level += db;
    return result;
}

std::optional<PhaseNoiseProfile> parsePhaseNoise(const QByteArray &text, QString *error)
{
    const auto fail = [&](int line, const QString &message) -> std::optional<PhaseNoiseProfile> {
        if (error)
            *error = line > 0 ? QStringLiteral("line %1: %2").arg(line).arg(message) : message;
        return std::nullopt;
    };
    static const QRegularExpression separators(QStringLiteral("[\\s,;]+"));
    static const QRegularExpression commentStart(QStringLiteral("[#!]"));
    std::vector<std::pair<double, double>> points;
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        const qsizetype comment = line.indexOf(commentStart);
        if (comment >= 0)
            line.truncate(comment);
        const QStringList tokens = line.split(separators, Qt::SkipEmptyParts);
        if (tokens.isEmpty())
            continue;
        if (tokens.size() != 2)
            return fail(i + 1, QStringLiteral("expected \"offset_Hz level_dBc/Hz\""));
        bool okOffset = false, okLevel = false;
        const double offset = tokens.at(0).toDouble(&okOffset);
        const double level = tokens.at(1).toDouble(&okLevel);
        if (!okOffset || !okLevel || !std::isfinite(offset) || !std::isfinite(level))
            return fail(i + 1, QStringLiteral("\"%1\" is not a pair of numbers").arg(line.trimmed()));
        if (!(offset > 0))
            return fail(i + 1, QStringLiteral("the offset must be positive"));
        points.emplace_back(offset, level);
    }
    if (points.empty())
        return fail(0, QStringLiteral("the phase-noise file has no points"));
    std::stable_sort(points.begin(), points.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    PhaseNoiseProfile profile;
    for (const auto &[offset, level] : points) {
        if (!profile.offsets.empty() && offset <= profile.offsets.back())
            return fail(0, QStringLiteral("the offset %1 Hz appears twice").arg(offset));
        profile.offsets.push_back(offset);
        profile.levels.push_back(level);
    }
    return profile;
}

std::optional<PhaseNoiseProfile> readPhaseNoise(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return std::nullopt;
    }
    QString parseError;
    auto profile = parsePhaseNoise(file.readAll(), &parseError);
    if (!profile && error)
        *error = QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), parseError);
    return profile;
}

double integratePhaseNoiseSsb(const PhaseNoiseProfile &profile, double f1, double f2)
{
    if (profile.empty() || !(f2 > f1) || !(f1 >= 0))
        return -INFINITY;
    const auto &x = profile.offsets;
    const auto &l = profile.levels;
    double sum = 0;
    // Below the first point and above the last: flat.
    if (f1 < x.front())
        sum += fromDb(l.front()) * (std::min(f2, x.front()) - f1);
    if (f2 > x.back())
        sum += fromDb(l.back()) * (f2 - std::max(f1, x.back()));
    for (size_t k = 0; k + 1 < x.size(); ++k) {
        const double a = std::max(f1, x[k]), b = std::min(f2, x[k + 1]);
        if (!(b > a))
            continue;
        const double s = (l[k + 1] - l[k]) / (10.0 * std::log10(x[k + 1] / x[k]));
        sum += powerLawIntegral(fromDb(l[k]), x[k], s, a, b);
    }
    return toDb(sum);
}

IntegratedPhaseNoise integratePhaseNoise(const PhaseNoiseProfile &profile, double f1, double f2, double carrier)
{
    IntegratedPhaseNoise result;
    const double ssb = integratePhaseNoiseSsb(profile, f1, f2);
    if (!std::isfinite(ssb))
        return result;
    const double variance = 2.0 * fromDb(ssb); // rad^2, both sidebands
    result.dbc = toDb(variance);
    const double rms = std::sqrt(variance);
    result.rmsPhaseDegrees = rms * 180.0 / Pi;
    result.rmsJitterSeconds = carrier > 0 ? rms / (2.0 * Pi * carrier) : 0.0;
    return result;
}

double vcoLevel(double offset, double pn, double pnOffset, double floor)
{
    const double slope = pn - 20.0 * std::log10(offset / pnOffset);
    return toDb(fromDb(slope) + fromDb(floor));
}

double pllLevel(double offset, const PllSettings &pll)
{
    const double n = pll.frequency / pll.reference;
    const double multiplication = 20.0 * std::log10(n);
    const double inBand = std::max(pll.referencePn + multiplication, pll.fom + 10.0 * std::log10(pll.reference) + multiplication);
    const double x = offset / pll.loopBandwidth;
    const double lowpass = 1.0 / (1.0 + x * x);
    const double highpass = x * x / (1.0 + x * x);
    const double vco = vcoLevel(offset, pll.vcoPn, pll.vcoPnOffset, pll.vcoFloor);
    return toDb(fromDb(inBand) * lowpass + fromDb(vco) * highpass);
}

PhaseNoiseProfile tabulate(const std::function<double(double)> &level)
{
    PhaseNoiseProfile profile;
    // 0.1 Hz to 10 GHz, 20 points per decade.
    for (int i = -20; i <= 200; ++i) {
        const double f = std::pow(10.0, i / 20.0);
        profile.offsets.push_back(f);
        profile.levels.push_back(level(f));
    }
    return profile;
}

std::optional<PhaseNoiseProfile> oscillatorPhaseNoise(const FlatComponent &component, const QString &projectDirectory,
                                                      QString *error)
{
    const ModelDefinition *model = component.definition ? component.definition->model(QStringLiteral("rf-behavioural")) : nullptr;
    if (!model)
        return std::nullopt;
    const QString block = model->data.value(QStringLiteral("block")).toString();
    const auto value = [&](const char *name, double fallback) {
        return component.parameterValues.value(QString::fromLatin1(name), fallback);
    };
    if (block == QLatin1String("lo")) {
        QString file = component.parameterText.value(QStringLiteral("phase_noise")).trimmed();
        if (file.size() >= 2 && file.startsWith(QLatin1Char('"')) && file.endsWith(QLatin1Char('"')))
            file = file.mid(1, file.size() - 2).trimmed();
        if (file.isEmpty())
            return std::nullopt;
        const QString path = QFileInfo(file).isAbsolute() || projectDirectory.isEmpty()
                                 ? file
                                 : QDir(projectDirectory).absoluteFilePath(file);
        return readPhaseNoise(path, error);
    }
    if (block == QLatin1String("vco")) {
        const double pn = value("pn", -110), offset = value("pn_offset", 100e3), floor = value("pn_floor", -160);
        if (!(offset > 0)) {
            if (error)
                *error = QStringLiteral("%1: pn_offset must be positive").arg(component.path);
            return std::nullopt;
        }
        return tabulate([=](double f) { return vcoLevel(f, pn, offset, floor); });
    }
    if (block == QLatin1String("pll")) {
        PllSettings pll;
        pll.frequency = value("freq", 0);
        pll.reference = value("ref_freq", 0);
        pll.referencePn = value("ref_pn", -150);
        pll.fom = value("fom", -226);
        pll.loopBandwidth = value("loop_bw", 100e3);
        pll.vcoPn = value("pn", -110);
        pll.vcoPnOffset = value("pn_offset", 100e3);
        pll.vcoFloor = value("pn_floor", -160);
        if (!(pll.frequency > 0) || !(pll.reference > 0) || !(pll.loopBandwidth > 0) || !(pll.vcoPnOffset > 0)) {
            if (error)
                *error = QStringLiteral("%1: freq, ref_freq, loop_bw and pn_offset must be positive").arg(component.path);
            return std::nullopt;
        }
        return tabulate([pll](double f) { return pllLevel(f, pll); });
    }
    return std::nullopt;
}

} // namespace lvd::rf

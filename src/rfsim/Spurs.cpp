#include "rfsim/Spurs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace lvd::rf {

// ---------------------------------------------------------------- spur tables

std::optional<double> SpurTable::levelDbc(int m, int n, double inputPowerDbm) const
{
    const int am = std::abs(m), an = std::abs(n);
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->m == am && it->n == an)
            return it->dbc - (am - 1) * (inputPowerDbm - referencePowerDbm);
    }
    return std::nullopt;
}

namespace {

bool isMissing(const QString &token)
{
    static const QStringList missing = {QStringLiteral("-"), QStringLiteral("--"), QStringLiteral("x"),
                                        QStringLiteral("n/a"), QStringLiteral("na")};
    return missing.contains(token.toLower());
}

bool toHarmonic(const QString &token, int *value)
{
    bool ok = false;
    const int v = token.toInt(&ok);
    if (!ok)
        return false;
    *value = std::abs(v);
    return true;
}

bool toLevel(const QString &token, double *value)
{
    bool ok = false;
    const double v = token.toDouble(&ok);
    if (!ok || !std::isfinite(v))
        return false;
    *value = v;
    return true;
}

} // namespace

std::optional<SpurTable> parseSpurTable(const QByteArray &text, QString *error)
{
    const auto fail = [&](int line, const QString &message) -> std::optional<SpurTable> {
        if (error)
            *error = line > 0 ? QStringLiteral("line %1: %2").arg(line).arg(message) : message;
        return std::nullopt;
    };
    static const QRegularExpression separators(QStringLiteral("[\\s,;]+"));
    static const QRegularExpression commentStart(QStringLiteral("[#!]"));

    SpurTable table;
    QList<int> columns; // LO harmonics of the matrix form; empty: rows "m n dBc"
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const int lineNumber = i + 1;
        QString line = lines.at(i);
        const qsizetype comment = line.indexOf(commentStart);
        if (comment >= 0)
            line.truncate(comment);
        const QStringList tokens = line.split(separators, Qt::SkipEmptyParts);
        if (tokens.isEmpty())
            continue;
        const QString keyword = tokens.first().toLower();

        if (keyword == QLatin1String("reference_power")) {
            double value = 0;
            const bool unitOk = tokens.size() == 2
                                || (tokens.size() == 3 && tokens.at(2).compare(QLatin1String("dBm"), Qt::CaseInsensitive) == 0);
            if (!unitOk || !toLevel(tokens.at(1), &value))
                return fail(lineNumber, QStringLiteral("expected \"reference_power <dBm>\""));
            table.referencePowerDbm = value;
            continue;
        }
        if (keyword == QLatin1String("lo_harmonics")) {
            columns.clear();
            for (qsizetype t = 1; t < tokens.size(); ++t) {
                int n = 0;
                if (!toHarmonic(tokens.at(t), &n))
                    return fail(lineNumber, QStringLiteral("\"%1\" is not an LO harmonic number").arg(tokens.at(t)));
                columns.append(n);
            }
            if (columns.isEmpty())
                return fail(lineNumber, QStringLiteral("lo_harmonics needs at least one harmonic number"));
            continue;
        }

        int m = 0;
        if (!toHarmonic(tokens.first(), &m))
            return fail(lineNumber, QStringLiteral("unknown keyword \"%1\"").arg(tokens.first()));
        if (!columns.isEmpty()) {
            if (tokens.size() != columns.size() + 1)
                return fail(lineNumber, QStringLiteral("expected the RF harmonic and %1 levels").arg(columns.size()));
            for (qsizetype t = 1; t < tokens.size(); ++t) {
                if (isMissing(tokens.at(t)))
                    continue;
                double dbc = 0;
                if (!toLevel(tokens.at(t), &dbc))
                    return fail(lineNumber, QStringLiteral("\"%1\" is not a level in dBc").arg(tokens.at(t)));
                const int n = columns.at(t - 1);
                if (m == 0 && n == 0)
                    continue;
                table.entries.push_back({m, n, dbc});
            }
        } else {
            int n = 0;
            double dbc = 0;
            if (tokens.size() != 3)
                return fail(lineNumber, QStringLiteral("expected \"m n dBc\" (or an lo_harmonics line before a matrix)"));
            if (!toHarmonic(tokens.at(1), &n))
                return fail(lineNumber, QStringLiteral("\"%1\" is not an LO harmonic number").arg(tokens.at(1)));
            if (isMissing(tokens.at(2)))
                continue;
            if (!toLevel(tokens.at(2), &dbc))
                return fail(lineNumber, QStringLiteral("\"%1\" is not a level in dBc").arg(tokens.at(2)));
            if (m == 0 && n == 0)
                return fail(lineNumber, QStringLiteral("m and n cannot both be 0"));
            table.entries.push_back({m, n, dbc});
        }
    }
    if (table.entries.empty())
        return fail(0, QStringLiteral("the spur table has no levels"));
    return table;
}

std::optional<SpurTable> readSpurTable(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot read %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return std::nullopt;
    }
    QString parseError;
    auto table = parseSpurTable(file.readAll(), &parseError);
    if (!table && error)
        *error = QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), parseError);
    return table;
}

double estimatedSpurDbc(int m, int n, double inputPowerDbm, double iip3Dbm)
{
    const int am = std::abs(m), an = std::abs(n);
    if (am == 1 && an == 1)
        return 0;
    const double margin = std::isfinite(iip3Dbm) ? std::max(0.0, iip3Dbm - inputPowerDbm) : 0.0;
    double dbc = std::max(0, am - 1) * margin;
    if (am >= 2 && am % 2 == 0)
        dbc += 10;
    if (an == 0)
        dbc += 25;
    else if (an % 2 == 0)
        dbc += 20 * std::log10(static_cast<double>(an)) + 25;
    else
        dbc += 20 * std::log10(static_cast<double>(an));
    return dbc;
}

// ---------------------------------------------------------------- analysis

namespace {

double parameter(const ChainStage &stage, const char *name, double fallback)
{
    return stage.component->parameterValues.value(QString::fromLatin1(name), fallback);
}

QString fileParameter(const ChainStage &stage, const char *name)
{
    QString text = stage.component->parameterText.value(QString::fromLatin1(name)).trimmed();
    if (text.size() >= 2 && text.startsWith(QLatin1Char('"')) && text.endsWith(QLatin1Char('"')))
        text = text.mid(1, text.size() - 2).trimmed();
    return text;
}

QString formatHz(double f)
{
    return QStringLiteral("%1 MHz").arg(f / 1e6, 0, 'g', 9);
}

class Analyser
{
public:
    Analyser(ChainEvaluator &evaluator, const SpurOptions &options, SpurResult &result)
        : m_evaluator(evaluator), m_chain(evaluator.chain()), m_options(options), m_result(result)
    {
    }

    void run()
    {
        if (!m_chain.ok()) {
            m_result.errors = m_chain.errors;
            return;
        }
        m_stageCount = static_cast<int>(m_chain.stages.size());
        double highest = m_chain.inputFrequency;
        for (const ChainStage &stage : m_chain.stages) {
            highest = std::max({highest, stage.inputFrequency, stage.outputFrequency});
            if (std::isfinite(stage.loFrequency))
                highest = std::max(highest, stage.loFrequency);
        }
        m_tolerance = 1e-9 * highest + 1e-3;
        m_searchStart = std::isfinite(m_options.searchStart) ? m_options.searchStart : 0.0;
        m_searchStop = std::isfinite(m_options.searchStop) ? m_options.searchStop : 3.0 * highest;

        QString error;
        double outputFrequency = NotSet;
        const double totalGain = m_evaluator.forwardGainDb(0, m_stageCount, m_chain.inputFrequency, &outputFrequency, &error);
        if (!std::isfinite(totalGain)) {
            m_result.errors.append(error.isEmpty() ? QStringLiteral("the wanted signal does not pass the chain") : error);
            return;
        }
        m_result.wantedOutputFrequency = outputFrequency;
        m_result.wantedOutputDbm = m_chain.inputPowerDbm + totalGain;

        for (int k = 0; k < m_stageCount; ++k) {
            if (m_chain.stages[static_cast<size_t>(k)].translation == ChainStage::Translation::Mixer)
                analyseMixer(k);
        }

        auto &spurs = m_result.spurs;
        spurs.erase(std::remove_if(spurs.begin(), spurs.end(),
                                   [&](const Spur &s) { return !(s.levelDbc <= m_options.minimumLevelDbc); }),
                    spurs.end());
        std::stable_sort(spurs.begin(), spurs.end(), [](const Spur &a, const Spur &b) {
            if (a.inBand != b.inBand)
                return a.inBand;
            return a.levelDbc < b.levelDbc;
        });
    }

private:
    struct Mixer {
        int index = 0;
        const ChainStage *stage = nullptr;
        double gainDb = 0;
        double iip3Dbm = 15;
        double rfIfIsolation = NotSet;
        std::optional<SpurTable> table;
        bool usedEstimate = false;
    };

    bool inBand(double f) const { return std::abs(f - m_result.wantedOutputFrequency) <= m_options.bandwidth / 2 + m_tolerance; }

    // Level of |m| x |n| in dBc below the wanted mixer output at mixer input power p.
    double mixerDbc(Mixer &mixer, int m, int n, double p, bool *estimated)
    {
        *estimated = false;
        const int am = std::abs(m), an = std::abs(n);
        if (am == 1 && an == 1)
            return 0;
        if (mixer.table) {
            if (const auto level = mixer.table->levelDbc(am, an, p))
                return *level;
        }
        if (am == 1 && an == 0 && std::isfinite(mixer.rfIfIsolation))
            return mixer.rfIfIsolation + mixer.gainDb; // direct RF feedthrough
        *estimated = true;
        mixer.usedEstimate = true;
        return estimatedSpurDbc(am, an, p, mixer.iip3Dbm);
    }

    // Frequency at the chain input that reaches the input of stage k at f, or NotSet.
    double toChainInput(int k, double f) const
    {
        for (int i = k - 1; i >= 0 && f > 0; --i) {
            const ChainStage &stage = m_chain.stages[static_cast<size_t>(i)];
            switch (stage.translation) {
            case ChainStage::Translation::None:
                break;
            case ChainStage::Translation::Multiplier:
                f /= stage.factor;
                break;
            case ChainStage::Translation::Mixer:
                if (stage.sumProduct)
                    f = f - stage.loFrequency;
                else
                    f = stage.inputFrequency > stage.loFrequency ? stage.loFrequency + f : stage.loFrequency - f;
                break;
            }
        }
        return f > m_tolerance ? f : NotSet;
    }

    void analyseMixer(int k)
    {
        Mixer mixer;
        mixer.index = k;
        mixer.stage = &m_chain.stages[static_cast<size_t>(k)];
        const ChainStage &stage = *mixer.stage;
        mixer.gainDb = parameter(stage, "gain", 0);
        mixer.iip3Dbm = parameter(stage, "iip3", 15);
        mixer.rfIfIsolation = parameter(stage, "rf_if_isolation", NotSet);
        if (!std::isfinite(stage.loFrequency)) {
            m_result.warnings.append(QStringLiteral("%1: no LO frequency; the mixer is skipped").arg(stage.path));
            return;
        }

        const QString file = fileParameter(stage, "spur_table");
        if (!file.isEmpty()) {
            const QString path = QFileInfo(file).isAbsolute() || m_options.projectDirectory.isEmpty()
                                     ? file
                                     : QDir(m_options.projectDirectory).absoluteFilePath(file);
            QString error;
            mixer.table = readSpurTable(path, &error);
            if (!mixer.table)
                m_result.warnings.append(QStringLiteral("%1: the spur table is not used (%2); levels are estimated")
                                             .arg(stage.path, error));
        }

        QString error;
        const double gainBefore = m_evaluator.forwardGainDb(0, k, m_chain.inputFrequency, nullptr, &error);
        const double gainAfterWanted = m_evaluator.forwardGainDb(0, k + 1, m_chain.inputFrequency, nullptr, &error);
        if (!std::isfinite(gainBefore) || !std::isfinite(gainAfterWanted)) {
            m_result.warnings.append(QStringLiteral("%1: %2").arg(stage.path, error));
            return;
        }
        const double mixerInputDbm = m_chain.inputPowerDbm + gainBefore;
        const double mixerOutputDbm = m_chain.inputPowerDbm + gainAfterWanted;

        products(mixer, mixerInputDbm, mixerOutputDbm);
        responses(mixer, mixerInputDbm, gainBefore);
        loLeakage(mixer);

        if (mixer.usedEstimate)
            m_result.warnings.append(mixer.table ? QStringLiteral("%1: products missing from the spur table are estimated "
                                                                  "from IIP3")
                                                                  .arg(stage.path)
                                                 : QStringLiteral("%1: no spur table; spur levels are estimated from IIP3")
                                                       .arg(stage.path));
    }

    void products(Mixer &mixer, double mixerInputDbm, double mixerOutputDbm)
    {
        const ChainStage &stage = *mixer.stage;
        const int order = m_options.maxOrder;
        for (int m = 1; m <= order; ++m) {
            for (int n = -(order - m); n <= order - m; ++n) {
                const double f = std::abs(m * stage.inputFrequency + n * stage.loFrequency);
                if (f <= m_tolerance)
                    continue;
                if (m == 1 && std::abs(n) == 1 && std::abs(f - stage.outputFrequency) <= m_tolerance)
                    continue; // the wanted product
                bool estimated = false;
                const double dbc = mixerDbc(mixer, m, n, mixerInputDbm, &estimated);
                QString error;
                double finalFrequency = NotSet;
                const double after = m_evaluator.forwardGainDb(mixer.index + 1, m_stageCount, f, &finalFrequency, &error);
                if (std::isnan(after)) {
                    m_result.warnings.append(QStringLiteral("%1: %2").arg(stage.path, error));
                    continue;
                }
                Spur spur;
                spur.kind = Spur::Kind::Product;
                spur.mixer = stage.path;
                spur.m = m;
                spur.n = n;
                spur.frequency = finalFrequency;
                spur.levelDbm = mixerOutputDbm - dbc + after;
                spur.levelDbc = m_result.wantedOutputDbm - spur.levelDbm;
                spur.inBand = inBand(finalFrequency);
                spur.estimated = estimated;
                if (m == 1 && n == 0)
                    spur.note = QStringLiteral("RF feedthrough");
                else if (m == 1 && std::abs(n) == 1)
                    spur.note = QStringLiteral("unwanted sideband");
                m_result.spurs.push_back(spur);
            }
        }
    }

    void responses(Mixer &mixer, double mixerInputDbm, double gainBefore)
    {
        const ChainStage &stage = *mixer.stage;
        const double fIf = stage.outputFrequency, fLo = stage.loFrequency;
        const int order = m_options.maxOrder;
        std::vector<Spur> found;
        for (int m = 1; m <= order; ++m) {
            for (int n = -(order - m); n <= order - m; ++n) {
                for (const double sign : {1.0, -1.0}) {
                    // m f + n f_LO = +-f_out
                    const double f = (sign * fIf - n * fLo) / m;
                    if (f <= m_tolerance || std::abs(f - stage.inputFrequency) <= m_tolerance)
                        continue;
                    const double fx = toChainInput(mixer.index, f);
                    if (!std::isfinite(fx) || fx < m_searchStart || fx > m_searchStop)
                        continue;
                    QString error;
                    double reached = NotSet;
                    const double pathGain = m_evaluator.forwardGainDb(0, mixer.index, fx, &reached, &error);
                    if (std::isnan(pathGain)) {
                        m_result.warnings.append(QStringLiteral("%1: %2").arg(stage.path, error));
                        continue;
                    }
                    if (std::abs(reached - f) > m_tolerance)
                        continue;
                    bool estimated = false;
                    const double dbc = mixerDbc(mixer, m, n, mixerInputDbm, &estimated);

                    Spur spur;
                    spur.mixer = stage.path;
                    spur.m = m;
                    spur.n = n;
                    spur.kind = Spur::Kind::SpuriousResponse;
                    const bool wantedSide = (f > fLo) == (stage.inputFrequency > fLo);
                    if (m == 1 && std::abs(n) == 1) {
                        spur.kind = Spur::Kind::Image;
                    } else if (m == 2 && std::abs(n) == 2 && wantedSide) {
                        spur.kind = Spur::Kind::HalfIf;
                    } else if (n == 0) {
                        spur.note = m == 1 ? QStringLiteral("IF feedthrough") : QString();
                    }
                    spur.frequency = m_result.wantedOutputFrequency;
                    spur.inputFrequency = fx;
                    spur.rejectionDb = gainBefore - pathGain + dbc;
                    spur.levelDbc = spur.rejectionDb;
                    spur.levelDbm = m_result.wantedOutputDbm - spur.rejectionDb;
                    spur.inBand = true;
                    spur.estimated = estimated;
                    if (spur.note.isEmpty())
                        spur.note = QStringLiteral("input at %1").arg(formatHz(fx));
                    found.push_back(spur);
                }
            }
        }
        // The same input frequency from several (m, n): keep the strongest.
        std::stable_sort(found.begin(), found.end(), [](const Spur &a, const Spur &b) { return a.rejectionDb < b.rejectionDb; });
        std::vector<Spur> unique;
        for (const Spur &spur : found) {
            const bool duplicate = std::any_of(unique.begin(), unique.end(), [&](const Spur &u) {
                return std::abs(u.inputFrequency - spur.inputFrequency) <= m_tolerance;
            });
            if (!duplicate)
                unique.push_back(spur);
        }
        m_result.spurs.insert(m_result.spurs.end(), unique.begin(), unique.end());
    }

    void loLeakage(Mixer &mixer)
    {
        const ChainStage &stage = *mixer.stage;
        if (!std::isfinite(stage.loPowerDbm)) {
            m_result.warnings.append(QStringLiteral("%1: unknown LO power; LO leakage is not computed").arg(stage.path));
            return;
        }
        // The chain leaves an up-converter at `rf` and enters it at `if`.
        const double toOutput = parameter(stage, stage.upConversion ? "lo_rf_isolation" : "lo_if_isolation", 0);
        const double toInput = parameter(stage, stage.upConversion ? "lo_if_isolation" : "lo_rf_isolation", 0);

        QString error;
        double finalFrequency = NotSet;
        const double after = m_evaluator.forwardGainDb(mixer.index + 1, m_stageCount, stage.loFrequency, &finalFrequency, &error);
        if (std::isnan(after)) {
            m_result.warnings.append(QStringLiteral("%1: %2").arg(stage.path, error));
        } else {
            Spur spur;
            spur.kind = Spur::Kind::LoLeakage;
            spur.mixer = stage.path;
            spur.n = 1;
            spur.frequency = finalFrequency;
            spur.levelDbm = stage.loPowerDbm - toOutput + after;
            spur.levelDbc = m_result.wantedOutputDbm - spur.levelDbm;
            spur.inBand = inBand(finalFrequency);
            spur.note = QStringLiteral("LO leakage at the output");
            m_result.spurs.push_back(spur);
        }

        const double before = m_evaluator.reverseGainDb(0, mixer.index, stage.loFrequency, &error);
        if (std::isnan(before)) {
            m_result.warnings.append(QStringLiteral("%1: %2").arg(stage.path, error));
        } else {
            Spur spur;
            spur.kind = Spur::Kind::LoLeakage;
            spur.mixer = stage.path;
            spur.n = 1;
            spur.frequency = stage.loFrequency;
            spur.inputFrequency = stage.loFrequency;
            spur.levelDbm = stage.loPowerDbm - toInput + before;
            spur.levelDbc = m_result.wantedOutputDbm - spur.levelDbm;
            spur.note = QStringLiteral("LO leakage at the input");
            m_result.spurs.push_back(spur);
        }
    }

    ChainEvaluator &m_evaluator;
    const SignalChain &m_chain;
    const SpurOptions &m_options;
    SpurResult &m_result;
    int m_stageCount = 0;
    double m_tolerance = 1e-3;
    double m_searchStart = 0;
    double m_searchStop = 0;
};

} // namespace

SpurResult analyseSpurs(ChainEvaluator &evaluator, const SpurOptions &options)
{
    SpurResult result;
    Analyser(evaluator, options, result).run();
    return result;
}

} // namespace lvd::rf

#include "antenna/PatternFile.h"

#include "core/JsonFile.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include <map>
#include <set>

namespace lvd::ant {

namespace {

enum class Column { Ignore, Frequency, Theta, Phi, Total, ThetaMag, ThetaPhase, PhiMag, PhiPhase, ThetaRe, ThetaIm, PhiRe, PhiIm };

struct ColumnInfo {
    Column kind = Column::Ignore;
    bool db = false;
    double scale = 1; // frequency unit
    int position = 0; // header position (whitespace headers)
};

struct Key {
    const char *text;
    Column kind;
    bool db;        // dB unless a unit says otherwise
    bool boundary;  // needs non-letter characters around it
};

// Most specific first; matched on the lowercase header without spaces.
constexpr Key kKeys[] = {
    {"re(etheta)", Column::ThetaRe, false, false},
    {"im(etheta)", Column::ThetaIm, false, false},
    {"re(ephi)", Column::PhiRe, false, false},
    {"im(ephi)", Column::PhiIm, false, false},
    {"etheta_re", Column::ThetaRe, false, false},
    {"etheta_im", Column::ThetaIm, false, false},
    {"ephi_re", Column::PhiRe, false, false},
    {"ephi_im", Column::PhiIm, false, false},
    {"etheta_mag_db", Column::ThetaMag, true, false},
    {"ephi_mag_db", Column::PhiMag, true, false},
    {"etheta_mag", Column::ThetaMag, false, false},
    {"etheta_phase", Column::ThetaPhase, false, false},
    {"ephi_mag", Column::PhiMag, false, false},
    {"ephi_phase", Column::PhiPhase, false, false},
    {"abs(theta)", Column::ThetaMag, false, false},
    {"phase(theta)", Column::ThetaPhase, false, false},
    {"abs(phi)", Column::PhiMag, false, false},
    {"phase(phi)", Column::PhiPhase, false, false},
    {"abs(dir.)", Column::Total, false, false},
    {"abs(dir)", Column::Total, false, false},
    {"abs(realizedgain)", Column::Total, false, false},
    {"abs(gain)", Column::Total, false, false},
    {"directivity(total)", Column::Total, true, false},
    {"gain(total)", Column::Total, true, false},
    {"directivity(theta)", Column::Ignore, false, false},
    {"directivity(phi)", Column::Ignore, false, false},
    {"gain(theta)", Column::Ignore, false, false},
    {"gain(phi)", Column::Ignore, false, false},
    {"ax.ratio", Column::Ignore, false, false},
    {"axialratio", Column::Ignore, false, false},
    {"frequency_hz", Column::Frequency, false, false},
    {"frequency_ghz", Column::Frequency, false, false},
    {"frequency_mhz", Column::Frequency, false, false},
    {"frequency", Column::Frequency, false, true},
    {"freq", Column::Frequency, false, true},
    {"theta_deg", Column::Theta, false, false},
    {"phi_deg", Column::Phi, false, false},
    {"theta", Column::Theta, false, true},
    {"phi", Column::Phi, false, true},
    {"realizedgain_dbi", Column::Total, true, false},
    {"gain_dbi", Column::Total, true, false},
    {"directivity_dbi", Column::Total, true, false},
    {"gain_db", Column::Total, true, false},
    {"realizedgain", Column::Total, true, true},
    {"directivity", Column::Total, true, true},
    {"gain", Column::Total, true, true},
    {"dbi", Column::Total, true, true},
};

// Lowercase; spaces inside brackets ("Abs(Phi  )[dBi   ]") are removed,
// other whitespace becomes '|' so that names stay apart.
QString normaliseHeader(QString text)
{
    text = text.toLower();
    text.remove(QLatin1Char('#'));
    text.replace(QLatin1Char('"'), QLatin1Char(' '));
    QString out;
    int depth = 0;
    for (const QChar c : text) {
        if (c == QLatin1Char('(') || c == QLatin1Char('['))
            ++depth;
        else if ((c == QLatin1Char(')') || c == QLatin1Char(']')) && depth > 0)
            --depth;
        if (c.isSpace()) {
            if (depth == 0 && !out.isEmpty() && !out.endsWith(QLatin1Char('|')))
                out += QLatin1Char('|');
            continue;
        }
        out += c;
    }
    while (out.endsWith(QLatin1Char('|')))
        out.chop(1);
    return out;
}

// The unit after a matched name, "[dBi]" or "(dB)"; empty when none.
QString unitAfter(const QString &text, int end)
{
    if (end < text.size() && text[end] == QLatin1Char('|') && end + 1 < text.size() && text[end + 1] == QLatin1Char('['))
        ++end; // "theta [deg.]"
    if (end < text.size() && (text[end] == QLatin1Char('[') || text[end] == QLatin1Char('('))) {
        const QChar close = text[end] == QLatin1Char('[') ? QLatin1Char(']') : QLatin1Char(')');
        const int stop = text.indexOf(close, end);
        if (stop > end)
            return text.mid(end + 1, stop - end - 1);
    }
    return {};
}

ColumnInfo makeColumn(const Key &key, const QString &matched, const QString &unit)
{
    ColumnInfo c;
    c.kind = key.kind;
    c.db = key.db;
    if (!unit.isEmpty() && (c.kind == Column::Total || c.kind == Column::ThetaMag || c.kind == Column::PhiMag))
        c.db = unit.contains(QLatin1String("db"));
    if (c.kind == Column::Frequency) {
        const QString u = unit.isEmpty() ? matched : unit;
        if (u.contains(QLatin1String("ghz")))
            c.scale = 1e9;
        else if (u.contains(QLatin1String("mhz")))
            c.scale = 1e6;
        else if (u.contains(QLatin1String("khz")))
            c.scale = 1e3;
    }
    return c;
}

bool letterAt(const QString &text, int i)
{
    return i >= 0 && i < text.size() && text[i].isLetter();
}

// Finds known names in a header. Token mode: one name per delimited token;
// position mode (whitespace headers with spaces inside names): all names in
// order of appearance.
std::vector<ColumnInfo> parseHeader(const QString &line, bool tokens, const QRegularExpression &delimiter)
{
    std::vector<ColumnInfo> columns;
    auto matchIn = [](const QString &text, std::vector<bool> &used, std::vector<ColumnInfo> &out) {
        for (const Key &key : kKeys) {
            const QString k = QString::fromLatin1(key.text);
            int from = 0;
            while (true) {
                const int at = text.indexOf(k, from);
                if (at < 0)
                    break;
                from = at + 1;
                const int end = at + int(k.size());
                bool free = true;
                for (int i = at; i < end; ++i)
                    free = free && !used[std::size_t(i)];
                if (!free)
                    continue;
                if (key.boundary && (letterAt(text, at - 1) || letterAt(text, end)))
                    continue;
                const QString unit = unitAfter(text, end);
                // The name and its unit bracket: "[dBi]" must not match "dbi" again.
                const int stop = unit.isEmpty() ? end : std::min(int(text.size()), end + int(unit.size()) + 3);
                for (int i = at; i < stop; ++i)
                    used[std::size_t(i)] = true;
                ColumnInfo c = makeColumn(key, k, unit);
                c.position = at;
                out.push_back(c);
            }
        }
    };

    if (tokens) {
        for (const QString &raw : line.split(delimiter)) {
            const QString token = normaliseHeader(raw);
            if (token.isEmpty())
                continue;
            std::vector<bool> used(std::size_t(token.size()), false);
            std::vector<ColumnInfo> found;
            matchIn(token, used, found);
            if (found.empty()) {
                columns.push_back({});
            } else {
                std::sort(found.begin(), found.end(), [](const ColumnInfo &a, const ColumnInfo &b) { return a.position < b.position; });
                columns.push_back(found.front());
            }
        }
        return columns;
    }
    const QString text = normaliseHeader(line);
    std::vector<bool> used(std::size_t(text.size()), false);
    matchIn(text, used, columns);
    std::sort(columns.begin(), columns.end(), [](const ColumnInfo &a, const ColumnInfo &b) { return a.position < b.position; });
    return columns;
}

struct Row {
    double theta, phi;
    Field field;
};

bool isNumberLine(const QStringList &fields)
{
    if (fields.isEmpty())
        return false;
    bool ok = false;
    fields.front().toDouble(&ok);
    return ok;
}

double roundKey(double v)
{
    return std::round(v * 1e6) / 1e6;
}

} // namespace

std::optional<PatternReadResult> parsePatternText(const QString &text, const QString &sourceName, QString *error,
                                                  double defaultFrequency)
{
    const QRegularExpression delimiters(QStringLiteral("[,;\\t]"));
    const QRegularExpression whitespace(QStringLiteral("[\\s,;]+"));
    const QRegularExpression frequencyLine(QStringLiteral("frequency\\s*[:=]\\s*([-+0-9.eE]+)"),
                                           QRegularExpression::CaseInsensitiveOption);

    std::vector<ColumnInfo> columns;
    std::map<double, std::vector<Row>> rows; // by frequency
    double blockFrequency = defaultFrequency;
    bool sawPhase = false;
    QStringList warnings;
    int lineNumber = 0;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        ++lineNumber;
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1Char('-')) && line.count(QLatin1Char('-')) > line.size() / 2)
            continue; // CST separator line
        const bool comment = line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char('*'))
                             || line.startsWith(QLatin1String("//")) || line.startsWith(QLatin1Char('%'));
        if (comment) {
            if (const auto m = frequencyLine.match(line); m.hasMatch()) {
                blockFrequency = m.captured(1).toDouble();
                continue;
            }
            const QString lower = line.toLower();
            if (lower.contains(QLatin1String("theta")) && lower.contains(QLatin1String("phi"))) {
                QString body = line;
                while (!body.isEmpty() && (body[0] == QLatin1Char('#') || body[0] == QLatin1Char('*') || body[0] == QLatin1Char('/')
                                           || body[0] == QLatin1Char('%')))
                    body.remove(0, 1);
                const bool tokens = body.contains(QLatin1Char(',')) || body.contains(QLatin1Char(';')) || body.contains(QLatin1Char('\t'));
                columns = parseHeader(tokens ? body : body.replace(QLatin1Char('"'), QLatin1Char(' ')), tokens, delimiters);
            }
            continue;
        }
        const QStringList fields = line.split(whitespace, Qt::SkipEmptyParts);
        if (!isNumberLine(fields)) {
            const bool tokens = line.contains(QLatin1Char(',')) || line.contains(QLatin1Char(';')) || line.contains(QLatin1Char('\t'));
            columns = parseHeader(line, tokens, delimiters);
            continue;
        }
        if (columns.empty()) {
            // No header: theta, phi, gain in dBi.
            if (fields.size() == 3) {
                columns = {makeColumn({"theta", Column::Theta, false, false}, {}, {}),
                           makeColumn({"phi", Column::Phi, false, false}, {}, {}),
                           makeColumn({"gain", Column::Total, true, false}, {}, {})};
                warnings << QStringLiteral("no header: columns taken as theta, phi, gain (dBi)");
            } else {
                setError(error, QStringLiteral("%1, line %2: no header line naming the columns").arg(sourceName).arg(lineNumber));
                return std::nullopt;
            }
        }
        if (fields.size() < int(columns.size())) {
            setError(error, QStringLiteral("%1, line %2: %3 values for %4 columns")
                                .arg(sourceName)
                                .arg(lineNumber)
                                .arg(fields.size())
                                .arg(columns.size()));
            return std::nullopt;
        }

        double f = blockFrequency, theta = NAN, phi = NAN, total = NAN;
        double tm = NAN, tp = 0, pm = NAN, pp = 0, tr = NAN, ti = 0, pr = NAN, pi = 0;
        bool tDb = false, pDb = false, totalDb = false;
        for (std::size_t i = 0; i < columns.size(); ++i) {
            bool ok = false;
            const double v = fields[int(i)].toDouble(&ok);
            if (!ok) {
                setError(error, QStringLiteral("%1, line %2: \"%3\" is not a number").arg(sourceName).arg(lineNumber).arg(fields[int(i)]));
                return std::nullopt;
            }
            const ColumnInfo &c = columns[i];
            switch (c.kind) {
            case Column::Frequency: f = v * c.scale; break;
            case Column::Theta: theta = v; break;
            case Column::Phi: phi = v; break;
            case Column::Total: total = v; totalDb = c.db; break;
            case Column::ThetaMag: tm = v; tDb = c.db; break;
            case Column::ThetaPhase: tp = v; sawPhase = true; break;
            case Column::PhiMag: pm = v; pDb = c.db; break;
            case Column::PhiPhase: pp = v; sawPhase = true; break;
            case Column::ThetaRe: tr = v; break;
            case Column::ThetaIm: ti = v; sawPhase = true; break;
            case Column::PhiRe: pr = v; break;
            case Column::PhiIm: pi = v; sawPhase = true; break;
            case Column::Ignore: break;
            }
        }
        if (std::isnan(theta) || std::isnan(phi)) {
            setError(error, QStringLiteral("%1: the header names no theta or phi column").arg(sourceName));
            return std::nullopt;
        }
        // Negative theta is the same direction as (|theta|, phi + 180); the
        // field components keep their values (theta_hat and phi_hat both
        // flip sign under this relabelling and the measured field vector is
        // unchanged, so the components flip too).
        bool folded = false;
        if (theta < 0) {
            theta = -theta;
            phi += 180.0;
            folded = true;
        }
        Field field;
        if (!std::isnan(tr) && !std::isnan(pr)) {
            field = {cplx(tr, ti), cplx(pr, pi)};
        } else if (!std::isnan(tm) && !std::isnan(pm)) {
            const double a = tDb ? amplitudeFromDb(tm) : tm;
            const double b = pDb ? amplitudeFromDb(pm) : pm;
            field = {std::polar(a, toRadians(tp)), std::polar(b, toRadians(pp))};
        } else if (!std::isnan(total)) {
            const double a = totalDb ? amplitudeFromDb(total) : std::sqrt(std::max(total, 0.0));
            const double p = toRadians(phi);
            field = {a * std::cos(p), -a * std::sin(p)};
        } else {
            setError(error, QStringLiteral("%1: the header names no field or gain columns").arg(sourceName));
            return std::nullopt;
        }
        if (folded && (!std::isnan(tr) || !std::isnan(tm)))
            field = {-field.eTheta, -field.ePhi};
        phi =std::fmod(std::fmod(phi, 360.0) + 360.0, 360.0);
        if (theta > 180.0 + 1e-6) {
            warnings << QStringLiteral("line %1: theta %2 is outside 0..180 and was skipped").arg(lineNumber).arg(theta);
            continue;
        }
        rows[roundKey(f)].push_back({roundKey(theta), roundKey(phi), field});
    }

    if (rows.empty()) {
        setError(error, QStringLiteral("%1 holds no pattern data").arg(sourceName));
        return std::nullopt;
    }

    PatternReadResult result;
    PatternTable &table = result.table;
    table.source = sourceName;
    table.hasPhase = sawPhase;
    std::set<double> thetas, phis;
    for (const auto &[f, list] : rows)
        for (const Row &r : list) {
            thetas.insert(r.theta);
            phis.insert(r.phi >= 360.0 - 1e-6 ? 0.0 : r.phi);
        }
    table.theta.assign(thetas.begin(), thetas.end());
    table.phi.assign(phis.begin(), phis.end());
    std::map<double, std::size_t> ti, pi;
    for (std::size_t i = 0; i < table.theta.size(); ++i)
        ti[table.theta[i]] = i;
    for (std::size_t i = 0; i < table.phi.size(); ++i)
        pi[table.phi[i]] = i;

    double peak = 0;
    int missing = 0;
    for (const auto &[f, list] : rows) {
        table.frequencies.push_back(f);
        std::vector<Field> grid(table.theta.size() * table.phi.size());
        std::vector<bool> filled(grid.size(), false);
        for (const Row &r : list) {
            const std::size_t k = ti[r.theta] * table.phi.size() + pi[r.phi >= 360.0 - 1e-6 ? 0.0 : r.phi];
            grid[k] = r.field;
            filled[k] = true;
            peak = std::max(peak, r.field.power());
        }
        // The poles have one direction: copy a measured value to the other phi.
        for (std::size_t it = 0; it < table.theta.size(); ++it) {
            if (table.theta[it] != 0.0 && table.theta[it] != 180.0)
                continue;
            std::optional<std::size_t> source;
            for (std::size_t ip = 0; ip < table.phi.size(); ++ip)
                if (filled[it * table.phi.size() + ip])
                    source = ip;
            if (!source)
                continue;
            // E_theta and E_phi rotate with phi at a pole: rebuild them from the
            // Cartesian field of the measured cut.
            const double p0 = toRadians(table.phi[*source]);
            const Field f0 = grid[it * table.phi.size() + *source];
            const double sign = table.theta[it] == 0.0 ? 1.0 : -1.0;
            const cplx ex = f0.eTheta * sign * std::cos(p0) - f0.ePhi * std::sin(p0);
            const cplx ey = f0.eTheta * sign * std::sin(p0) + f0.ePhi * std::cos(p0);
            for (std::size_t ip = 0; ip < table.phi.size(); ++ip) {
                const std::size_t k = it * table.phi.size() + ip;
                if (filled[k])
                    continue;
                const double p = toRadians(table.phi[ip]);
                grid[k] = {sign * (ex * std::cos(p) + ey * std::sin(p)), -ex * std::sin(p) + ey * std::cos(p)};
                filled[k] = true;
            }
        }
        for (bool b : filled)
            missing += b ? 0 : 1;
        table.fields.push_back(std::move(grid));
    }
    if (missing > 0)
        warnings << QStringLiteral("%1 grid points have no data and are set to zero").arg(missing);
    if (table.frequencies.size() == 1 && table.frequencies.front() == 0 && defaultFrequency <= 0)
        warnings << QStringLiteral("the file gives no frequency; the pattern is used at every frequency");
    if (table.theta.back() < 179.0)
        warnings << QStringLiteral("theta stops at %1 degrees; the rest of the sphere is taken as zero").arg(table.theta.back());
    result.peakDbi = powerDb(peak);
    result.warnings = warnings;
    return result;
}

std::optional<PatternReadResult> readPatternFile(const QString &path, QString *error, double defaultFrequency)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setError(error, QStringLiteral("cannot open %1: %2").arg(path, file.errorString()));
        return std::nullopt;
    }
    const QString text = QString::fromUtf8(file.readAll());
    auto result = parsePatternText(text, QFileInfo(path).fileName(), error, defaultFrequency);
    if (result)
        result->table.source = QFileInfo(path).absoluteFilePath();
    return result;
}

QString patternCsvText(const PatternTable &table)
{
    QString text;
    QTextStream out(&text);
    out << "# lvd RF cad far-field pattern; fields normalised so that |E|^2 is directivity or gain\n";
    out << "frequency_hz,theta_deg,phi_deg,etheta_re,etheta_im,ephi_re,ephi_im\n";
    out.setRealNumberPrecision(10);
    for (std::size_t f = 0; f < table.frequencies.size(); ++f)
        for (std::size_t it = 0; it < table.theta.size(); ++it)
            for (std::size_t ip = 0; ip < table.phi.size(); ++ip) {
                const Field &v = table.fields[f][it * table.phi.size() + ip];
                out << table.frequencies[f] << ',' << table.theta[it] << ',' << table.phi[ip] << ',' << v.eTheta.real()
                    << ',' << v.eTheta.imag() << ',' << v.ePhi.real() << ',' << v.ePhi.imag() << '\n';
            }
    return text;
}

bool writePatternCsv(const PatternTable &table, const QString &path, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setError(error, QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
        return false;
    }
    file.write(patternCsvText(table).toUtf8());
    if (!file.commit()) {
        setError(error, QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
        return false;
    }
    return true;
}

} // namespace lvd::ant

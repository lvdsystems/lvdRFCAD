#include "rfdata/Touchstone.h"

#include "rfdata/NetworkMath.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace lvd::rf {

namespace {

enum class Parameter { S, Y, Z, G, H };
enum class MatrixFormat { Full, Lower, Upper };
enum class Section { None, Network, Noise, Information };

struct DataLine {
    int line = 0;
    std::vector<double> values;
};

struct Record {
    int line = 0;
    std::vector<double> values; // frequency (file unit) followed by the data
};

QString lineError(int line, const QString &message)
{
    return QStringLiteral("line %1: %2").arg(line).arg(message);
}

QString formatName(TouchstoneFormat format)
{
    switch (format) {
    case TouchstoneFormat::RealImaginary:
        return QStringLiteral("RI");
    case TouchstoneFormat::MagnitudeAngle:
        return QStringLiteral("MA");
    case TouchstoneFormat::DecibelAngle:
        return QStringLiteral("DB");
    }
    return {};
}

std::optional<double> unitFactor(const QString &unit)
{
    const QString u = unit.toLower();
    if (u == QLatin1String("hz"))
        return 1.0;
    if (u == QLatin1String("khz"))
        return 1e3;
    if (u == QLatin1String("mhz"))
        return 1e6;
    if (u == QLatin1String("ghz"))
        return 1e9;
    return std::nullopt;
}

Complex pairToComplex(double first, double second, TouchstoneFormat format)
{
    switch (format) {
    case TouchstoneFormat::RealImaginary:
        return {first, second};
    case TouchstoneFormat::MagnitudeAngle:
        return std::polar(first, second * Pi / 180.0);
    case TouchstoneFormat::DecibelAngle:
        return fromDbPhase(first, second);
    }
    return {};
}

// Hybrid parameters (V1 = h11 I1 + h12 V2, I2 = h21 I1 + h22 V2, currents
// into the ports) to S. The parameters are first normalised to the port
// references (v = V / sqrt(z0), i = I sqrt(z0)), then the unit-reference
// formulas apply.
std::optional<CMatrix> hToS(const CMatrix &h, double z01, double z02)
{
    const double r = std::sqrt(z02 / z01);
    const Complex h11 = h(0, 0) / z01, h12 = h(0, 1) * r, h21 = h(1, 0) * r, h22 = h(1, 1) * z02;
    const Complex den = (h11 + 1.0) * (h22 + 1.0) - h12 * h21;
    if (std::abs(den) < 1e-300)
        return std::nullopt;
    CMatrix s(2, 2);
    s(0, 0) = ((h11 - 1.0) * (h22 + 1.0) - h12 * h21) / den;
    s(0, 1) = 2.0 * h12 / den;
    s(1, 0) = -2.0 * h21 / den;
    s(1, 1) = ((1.0 + h11) * (1.0 - h22) + h12 * h21) / den;
    return s;
}

// Inverse hybrid parameters (I1 = g11 V1 + g12 I2, V2 = g21 V1 + g22 I2): G = H^-1.
std::optional<CMatrix> gToS(const CMatrix &g, double z01, double z02)
{
    const Complex det = g(0, 0) * g(1, 1) - g(0, 1) * g(1, 0);
    if (std::abs(det) < 1e-300)
        return std::nullopt;
    CMatrix h(2, 2);
    h << g(1, 1) / det, -g(0, 1) / det, -g(1, 0) / det, g(0, 0) / det;
    return hToS(h, z01, z02);
}

// Infers the port count of version 1 data without a file name: the first
// record of an N-port has 1 + 2 N^2 values; N >= 3 spreads it over lines
// with 2 min(N, 4) values each after the first line, so a line with an odd
// number of values starts the next record.
int inferPortCount(const std::vector<DataLine> &lines)
{
    if (lines.empty())
        return 0;
    const size_t first = lines[0].values.size();
    if (first == 3)
        return 1;
    if (first == 7)
        return 3;
    if (first != 9)
        return 0;
    size_t total = first;
    for (size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].values.size() % 2 == 1)
            break;
        total += lines[i].values.size();
    }
    for (int n = 1; n <= 99; ++n) {
        if (static_cast<size_t>(1 + 2 * n * n) == total)
            return n;
    }
    return 0;
}

class Parser
{
public:
    Parser(const QByteArray &text, int portHint) : m_text(text), m_ports(portHint) {}

    TouchstoneReadResult run()
    {
        TouchstoneReadResult result;
        if (!scan() || !build()) {
            result.error = m_error;
            result.warnings = m_warnings;
            return result;
        }
        result.network = std::move(m_network);
        result.warnings = m_warnings;
        return result;
    }

private:
    bool fail(int line, const QString &message)
    {
        m_error = line > 0 ? lineError(line, message) : message;
        return false;
    }
    void warn(int line, const QString &message) { m_warnings << lineError(line, message); }

    bool scan();
    bool keyword(int line, const QString &content);
    bool optionLine(int line, const QString &content);
    bool referenceValues(int line, const QStringList &tokens);
    bool build();
    bool splitRecords(std::vector<Record> &network, std::vector<Record> &noise);
    std::optional<CMatrix> toS(const Record &record);

    QByteArray m_text;
    int m_ports = 0;
    QString m_error;
    QStringList m_warnings;
    NetworkData m_network;

    int m_version = 1;
    bool m_optionSeen = false;
    bool m_dataSeen = false;
    double m_unit = 1e9;
    Parameter m_parameter = Parameter::S;
    TouchstoneFormat m_format = TouchstoneFormat::MagnitudeAngle;
    double m_r = DefaultZ0;

    // Version 2 state
    bool m_portsKeyword = false;
    std::optional<bool> m_order21First;
    int m_frequencyCount = -1;
    int m_noiseFrequencyCount = -1;
    std::vector<double> m_reference;
    bool m_referencePending = false;
    int m_referenceLine = 0;
    MatrixFormat m_matrixFormat = MatrixFormat::Full;
    Section m_section = Section::None;
    bool m_ended = false;

    QStringList m_comments;
    std::vector<DataLine> m_networkLines;
    std::vector<DataLine> m_noiseLines;
};

bool Parser::scan()
{
    const QList<QByteArray> lines = m_text.split('\n');
    for (int index = 0; index < lines.size() && !m_ended; ++index) {
        const int lineNumber = index + 1;
        QString line = QString::fromUtf8(lines[index]);
        line.remove(QLatin1Char('\r'));
        const qsizetype bang = line.indexOf(QLatin1Char('!'));
        if (bang >= 0) {
            if (line.left(bang).trimmed().isEmpty()) {
                QString comment = line.mid(bang + 1);
                if (comment.startsWith(QLatin1Char(' ')))
                    comment.remove(0, 1);
                while (!comment.isEmpty() && comment.back().isSpace())
                    comment.chop(1);
                m_comments << comment;
            }
            line.truncate(bang);
        }
        const QString content = line.trimmed();
        if (content.isEmpty())
            continue;

        if (m_section == Section::Information) {
            if (content.toLower().simplified().startsWith(QLatin1String("[end information]")))
                m_section = Section::None;
            continue;
        }
        if (content.startsWith(QLatin1Char('['))) {
            if (m_referencePending)
                return fail(m_referenceLine, QStringLiteral("[Reference] has fewer values than ports"));
            if (!keyword(lineNumber, content))
                return false;
            continue;
        }
        if (content.startsWith(QLatin1Char('#'))) {
            if (m_referencePending)
                return fail(m_referenceLine, QStringLiteral("[Reference] has fewer values than ports"));
            if (!optionLine(lineNumber, content))
                return false;
            continue;
        }

        const QStringList tokens = content.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (m_referencePending) {
            if (!referenceValues(lineNumber, tokens))
                return false;
            continue;
        }
        DataLine data;
        data.line = lineNumber;
        data.values.reserve(static_cast<size_t>(tokens.size()));
        for (const QString &token : tokens) {
            bool ok = false;
            const double value = token.toDouble(&ok);
            if (!ok)
                return fail(lineNumber, QStringLiteral("'%1' is not a number").arg(token));
            if (!std::isfinite(value))
                return fail(lineNumber, QStringLiteral("non-finite value '%1'").arg(token));
            data.values.push_back(value);
        }
        if (m_version == 2) {
            if (m_section == Section::Network)
                m_networkLines.push_back(std::move(data));
            else if (m_section == Section::Noise)
                m_noiseLines.push_back(std::move(data));
            else
                return fail(lineNumber, QStringLiteral("data outside [Network Data] and [Noise Data]"));
        } else {
            if (!m_optionSeen && !m_dataSeen)
                warn(lineNumber, QStringLiteral("no option line before the data; using # GHz S MA R 50"));
            m_networkLines.push_back(std::move(data));
        }
        m_dataSeen = true;
    }
    if (m_referencePending)
        return fail(m_referenceLine, QStringLiteral("[Reference] has fewer values than ports"));
    return true;
}

bool Parser::keyword(int line, const QString &content)
{
    const qsizetype close = content.indexOf(QLatin1Char(']'));
    if (close < 0)
        return fail(line, QStringLiteral("unterminated keyword '%1'").arg(content));
    const QString name = content.mid(1, close - 1).simplified().toLower();
    const QString argument = content.mid(close + 1).trimmed();
    const QStringList arguments = argument.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);

    if (name == QLatin1String("version")) {
        if (m_optionSeen || m_dataSeen || m_version == 2)
            return fail(line, QStringLiteral("[Version] must be the first line of the file"));
        bool ok = false;
        const double version = argument.toDouble(&ok);
        if (!ok || version < 2.0 || version >= 3.0)
            return fail(line, QStringLiteral("unsupported Touchstone version '%1'").arg(argument));
        if (version > 2.1)
            warn(line, QStringLiteral("Touchstone version %1 is newer than 2.1; reading it as 2.1").arg(argument));
        m_version = 2;
        return true;
    }
    if (m_version != 2)
        return fail(line, QStringLiteral("keyword [%1] in a version 1 file (missing [Version] 2.0?)").arg(name));

    auto positiveInteger = [&](int &target, bool allowZero) {
        bool ok = false;
        const int value = arguments.value(0).toInt(&ok);
        if (!ok || value < (allowZero ? 0 : 1))
            return fail(line, QStringLiteral("[%1] needs a %2 integer").arg(name, allowZero ? QStringLiteral("non-negative") : QStringLiteral("positive")));
        target = value;
        return true;
    };

    if (name == QLatin1String("number of ports")) {
        if (m_portsKeyword)
            return fail(line, QStringLiteral("[Number of Ports] given twice"));
        int ports = 0;
        if (!positiveInteger(ports, false))
            return false;
        m_ports = ports;
        m_portsKeyword = true;
        return true;
    }
    const bool needsPorts = name == QLatin1String("two-port data order") || name == QLatin1String("reference")
                            || name == QLatin1String("network data") || name == QLatin1String("noise data");
    if (needsPorts && !m_portsKeyword)
        return fail(line, QStringLiteral("[%1] before [Number of Ports]").arg(name));

    if (name == QLatin1String("two-port data order")) {
        if (argument == QLatin1String("12_21"))
            m_order21First = false;
        else if (argument == QLatin1String("21_12"))
            m_order21First = true;
        else
            return fail(line, QStringLiteral("[Two-Port Data Order] must be 12_21 or 21_12"));
        if (m_ports != 2)
            warn(line, QStringLiteral("[Two-Port Data Order] in a file with %1 ports is ignored").arg(m_ports));
        return true;
    }
    if (name == QLatin1String("number of frequencies"))
        return positiveInteger(m_frequencyCount, false);
    if (name == QLatin1String("number of noise frequencies"))
        return positiveInteger(m_noiseFrequencyCount, false);
    if (name == QLatin1String("reference")) {
        m_reference.clear();
        m_referencePending = true;
        m_referenceLine = line;
        return referenceValues(line, arguments);
    }
    if (name == QLatin1String("matrix format")) {
        const QString format = argument.toLower();
        if (format == QLatin1String("full"))
            m_matrixFormat = MatrixFormat::Full;
        else if (format == QLatin1String("lower"))
            m_matrixFormat = MatrixFormat::Lower;
        else if (format == QLatin1String("upper"))
            m_matrixFormat = MatrixFormat::Upper;
        else
            return fail(line, QStringLiteral("[Matrix Format] must be Full, Lower or Upper"));
        return true;
    }
    if (name == QLatin1String("mixed-mode order"))
        return fail(line, QStringLiteral("mixed-mode data is not supported"));
    if (name == QLatin1String("network data")) {
        if (!m_optionSeen)
            warn(line, QStringLiteral("no option line before [Network Data]; using # GHz S MA R 50"));
        if (m_ports == 2 && !m_order21First) {
            warn(line, QStringLiteral("two-port file without [Two-Port Data Order]; assuming 12_21"));
            m_order21First = false;
        }
        if (m_frequencyCount < 0)
            warn(line, QStringLiteral("[Number of Frequencies] is missing"));
        m_section = Section::Network;
        return true;
    }
    if (name == QLatin1String("noise data")) {
        if (m_ports != 2)
            return fail(line, QStringLiteral("noise data requires a two-port"));
        m_section = Section::Noise;
        return true;
    }
    if (name == QLatin1String("end")) {
        m_ended = true;
        return true;
    }
    if (name == QLatin1String("begin information")) {
        m_section = Section::Information;
        return true;
    }
    warn(line, QStringLiteral("unknown keyword [%1] ignored").arg(name));
    return true;
}

bool Parser::referenceValues(int line, const QStringList &tokens)
{
    for (const QString &token : tokens) {
        bool ok = false;
        const double value = token.toDouble(&ok);
        if (!ok || !(value > 0) || !std::isfinite(value))
            return fail(line, QStringLiteral("invalid reference impedance '%1'").arg(token));
        if (static_cast<int>(m_reference.size()) >= m_ports)
            return fail(line, QStringLiteral("[Reference] has more values than ports"));
        m_reference.push_back(value);
    }
    if (static_cast<int>(m_reference.size()) == m_ports)
        m_referencePending = false;
    return true;
}

bool Parser::optionLine(int line, const QString &content)
{
    if (m_optionSeen) {
        warn(line, QStringLiteral("additional option line ignored"));
        return true;
    }
    if (m_dataSeen)
        return fail(line, QStringLiteral("option line after the data"));
    m_optionSeen = true;
    const QStringList tokens = content.mid(1).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (int i = 0; i < tokens.size(); ++i) {
        const QString token = tokens[i].toUpper();
        if (const auto factor = unitFactor(token)) {
            m_unit = *factor;
        } else if (token == QLatin1String("S")) {
            m_parameter = Parameter::S;
        } else if (token == QLatin1String("Y")) {
            m_parameter = Parameter::Y;
        } else if (token == QLatin1String("Z")) {
            m_parameter = Parameter::Z;
        } else if (token == QLatin1String("G")) {
            m_parameter = Parameter::G;
        } else if (token == QLatin1String("H")) {
            m_parameter = Parameter::H;
        } else if (token == QLatin1String("RI")) {
            m_format = TouchstoneFormat::RealImaginary;
        } else if (token == QLatin1String("MA")) {
            m_format = TouchstoneFormat::MagnitudeAngle;
        } else if (token == QLatin1String("DB")) {
            m_format = TouchstoneFormat::DecibelAngle;
        } else if (token == QLatin1String("R")) {
            if (i + 1 >= tokens.size())
                return fail(line, QStringLiteral("R without a value in the option line"));
            bool ok = false;
            const double r = tokens[++i].toDouble(&ok);
            if (!ok || !(r > 0) || !std::isfinite(r))
                return fail(line, QStringLiteral("invalid reference resistance '%1'").arg(tokens[i]));
            m_r = r;
        } else {
            warn(line, QStringLiteral("unknown option '%1' ignored").arg(tokens[i]));
        }
    }
    return true;
}

// Groups the numbers into per-frequency records. Version 1 two-port files
// switch to noise data at the first 5-value line whose frequency is at or
// below the previous one.
bool Parser::splitRecords(std::vector<Record> &network, std::vector<Record> &noise)
{
    const int n = m_ports;
    const int pairs = m_matrixFormat == MatrixFormat::Full ? n * n : n * (n + 1) / 2;
    const size_t perRecord = static_cast<size_t>(1 + 2 * pairs);

    Record current;
    bool inNoise = false;
    for (const DataLine &line : m_networkLines) {
        if (current.values.empty() && m_version == 1 && n == 2 && !network.empty() && line.values.size() == 5
            && line.values[0] <= network.back().values[0]) {
            inNoise = true;
        }
        if (inNoise) {
            m_noiseLines.push_back(line);
            continue;
        }
        size_t i = 0;
        while (i < line.values.size()) {
            if (current.values.empty()) {
                if (i > 0 && m_version == 1) {
                    return fail(line.line, QStringLiteral("a new frequency must start a line; expected %1 values per frequency "
                                                          "for %2 ports")
                                               .arg(perRecord)
                                               .arg(n));
                }
                current.line = line.line;
            }
            const size_t take = std::min(perRecord - current.values.size(), line.values.size() - i);
            current.values.insert(current.values.end(), line.values.begin() + static_cast<std::ptrdiff_t>(i),
                                  line.values.begin() + static_cast<std::ptrdiff_t>(i + take));
            i += take;
            if (current.values.size() == perRecord) {
                network.push_back(std::move(current));
                current = {};
            }
        }
    }
    if (!current.values.empty()) {
        return fail(current.line, QStringLiteral("incomplete data: %1 of %2 values for the frequency starting here")
                                      .arg(current.values.size())
                                      .arg(perRecord));
    }

    for (const DataLine &line : m_noiseLines) {
        if (line.values.size() != 5)
            return fail(line.line, QStringLiteral("noise data lines need 5 values, found %1").arg(line.values.size()));
        noise.push_back({line.line, line.values});
    }
    return true;
}

std::optional<CMatrix> Parser::toS(const Record &record)
{
    const int n = m_ports;
    CMatrix m = CMatrix::Zero(n, n);
    auto value = [&](size_t pair) {
        return pairToComplex(record.values[1 + 2 * pair], record.values[2 + 2 * pair], m_format);
    };
    if (n == 2 && (m_version == 1 || m_order21First.value_or(false))) {
        m(0, 0) = value(0);
        m(1, 0) = value(1);
        m(0, 1) = value(2);
        m(1, 1) = value(3);
    } else if (m_matrixFormat == MatrixFormat::Full) {
        size_t k = 0;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                m(i, j) = value(k++);
    } else {
        size_t k = 0;
        for (int i = 0; i < n; ++i) {
            const int from = m_matrixFormat == MatrixFormat::Lower ? 0 : i;
            const int to = m_matrixFormat == MatrixFormat::Lower ? i : n - 1;
            for (int j = from; j <= to; ++j) {
                m(i, j) = value(k);
                m(j, i) = value(k);
                ++k;
            }
        }
    }

    const std::vector<double> &z0 = m_network.z0;
    const bool normalised = m_version == 1; // version 1 Y, Z, G, H data are normalised to R
    // Z and Y have an S-matrix unless z + z0 (y + 1/z0) is singular.
    auto convertible = [&](const CMatrix &matrix, bool impedance) {
        Eigen::VectorXd scale(n);
        for (int i = 0; i < n; ++i)
            scale(i) = impedance ? 1.0 / std::sqrt(z0[static_cast<size_t>(i)]) : std::sqrt(z0[static_cast<size_t>(i)]);
        const CMatrix normalisedMatrix = scale.asDiagonal() * matrix * scale.asDiagonal();
        return Eigen::FullPivLU<CMatrix>(normalisedMatrix + CMatrix::Identity(n, n)).isInvertible();
    };
    switch (m_parameter) {
    case Parameter::S:
        return m;
    case Parameter::Z:
        if (normalised)
            m *= m_r;
        if (!convertible(m, true))
            return std::nullopt;
        return zToS(m, z0);
    case Parameter::Y:
        if (normalised)
            m /= m_r;
        if (!convertible(m, false))
            return std::nullopt;
        return yToS(m, z0);
    case Parameter::H:
        if (normalised) {
            m(0, 0) *= m_r;
            m(1, 1) /= m_r;
        }
        return hToS(m, z0[0], z0[1]);
    case Parameter::G:
        if (normalised) {
            m(0, 0) /= m_r;
            m(1, 1) *= m_r;
        }
        return gToS(m, z0[0], z0[1]);
    }
    return std::nullopt;
}

bool Parser::build()
{
    if (m_version == 1 && m_ports <= 0) {
        m_ports = inferPortCount(m_networkLines);
        if (m_ports <= 0)
            return fail(0, QStringLiteral("cannot determine the number of ports (use a .sNp file name)"));
    }
    if (m_version == 2 && !m_portsKeyword)
        return fail(0, QStringLiteral("[Number of Ports] is missing"));
    if ((m_parameter == Parameter::G || m_parameter == Parameter::H) && m_ports != 2)
        return fail(0, QStringLiteral("G and H parameters are defined for two-ports only"));
    if (m_version == 1 && m_matrixFormat != MatrixFormat::Full)
        m_matrixFormat = MatrixFormat::Full;

    m_network.z0 = m_reference.empty() ? std::vector<double>(static_cast<size_t>(m_ports), m_r) : m_reference;

    std::vector<Record> records, noiseRecords;
    if (!splitRecords(records, noiseRecords))
        return false;
    if (records.empty())
        return fail(0, QStringLiteral("the file contains no network data"));
    if (m_frequencyCount >= 0 && static_cast<int>(records.size()) != m_frequencyCount) {
        warn(records.front().line, QStringLiteral("[Number of Frequencies] is %1 but the file has %2 frequencies")
                                       .arg(m_frequencyCount)
                                       .arg(records.size()));
    }
    if (m_noiseFrequencyCount >= 0 && static_cast<int>(noiseRecords.size()) != m_noiseFrequencyCount) {
        warn(noiseRecords.empty() ? records.front().line : noiseRecords.front().line,
             QStringLiteral("[Number of Noise Frequencies] is %1 but the file has %2 noise frequencies")
                 .arg(m_noiseFrequencyCount)
                 .arg(noiseRecords.size()));
    }

    // Order by frequency, dropping duplicates (the first one wins).
    auto sortRecords = [this](std::vector<Record> &list, const QString &what) {
        bool sorted = true;
        for (size_t i = 1; i < list.size(); ++i) {
            if (list[i].values[0] < list[i - 1].values[0]) {
                sorted = false;
                break;
            }
        }
        if (!sorted) {
            warn(list.front().line, QStringLiteral("%1 frequencies are not increasing; the data was sorted").arg(what));
            std::stable_sort(list.begin(), list.end(),
                             [](const Record &a, const Record &b) { return a.values[0] < b.values[0]; });
        }
        std::vector<Record> unique;
        for (Record &record : list) {
            if (!unique.empty() && record.values[0] == unique.back().values[0]) {
                warn(record.line, QStringLiteral("duplicate %1 frequency %2 ignored").arg(what).arg(record.values[0]));
                continue;
            }
            if (record.values[0] < 0) {
                m_error = lineError(record.line, QStringLiteral("negative frequency"));
                return false;
            }
            unique.push_back(std::move(record));
        }
        list = std::move(unique);
        return true;
    };
    if (!sortRecords(records, QStringLiteral("network")) || !sortRecords(noiseRecords, QStringLiteral("noise")))
        return false;

    m_network.frequencies.reserve(records.size());
    m_network.s.reserve(records.size());
    for (const Record &record : records) {
        auto s = toS(record);
        if (!s || !s->allFinite())
            return fail(record.line, QStringLiteral("the parameters cannot be converted to S-parameters"));
        m_network.frequencies.push_back(record.values[0] * m_unit);
        m_network.s.push_back(std::move(*s));
    }
    // Version 1 noise resistance is normalised to R, version 2 is in ohm.
    const double rnScale = m_version == 1 ? m_r : 1.0;
    for (const Record &record : noiseRecords) {
        NoiseParameters noise;
        noise.fminDb = record.values[1];
        noise.gammaOpt = std::polar(record.values[2], record.values[3] * Pi / 180.0);
        noise.rn = record.values[4] * rnScale;
        m_network.noiseFrequencies.push_back(record.values[0] * m_unit);
        m_network.noise.push_back(noise);
    }
    m_network.comment = m_comments.join(QLatin1Char('\n'));

    const QString problem = m_network.validate();
    if (!problem.isEmpty())
        return fail(0, problem);
    return true;
}

QString number(double value, int precision)
{
    return QString::number(value, 'g', precision);
}

QStringList pairText(Complex value, TouchstoneFormat format, int precision)
{
    switch (format) {
    case TouchstoneFormat::RealImaginary:
        return {number(value.real(), precision), number(value.imag(), precision)};
    case TouchstoneFormat::MagnitudeAngle:
        return {number(std::abs(value), precision), number(std::arg(value) * 180.0 / Pi, precision)};
    case TouchstoneFormat::DecibelAngle: {
        // Exact zeros have no dB value; -9999 dB reads back as 0.
        const double db = std::abs(value) > 0 ? db20(value) : -9999.0;
        return {number(db, precision), number(std::arg(value) * 180.0 / Pi, precision)};
    }
    }
    return {};
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

TouchstoneReadResult parseTouchstone(const QByteArray &text, int portCountHint)
{
    return Parser(text, portCountHint).run();
}

TouchstoneReadResult readTouchstone(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        TouchstoneReadResult result;
        result.error = QStringLiteral("Cannot open %1: %2").arg(path, file.errorString());
        return result;
    }
    int ports = 0;
    static const QRegularExpression extension(QStringLiteral("^[sSyYzZgGhH](\\d+)[pP]$"));
    const QRegularExpressionMatch match = extension.match(QFileInfo(path).suffix());
    if (match.hasMatch())
        ports = match.captured(1).toInt();
    TouchstoneReadResult result = parseTouchstone(file.readAll(), ports);
    if (!result.network)
        result.error = QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), result.error);
    return result;
}

QByteArray formatTouchstone(const NetworkData &network, const TouchstoneWriteOptions &options, QString *error)
{
    const QString problem = network.validate();
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Invalid network: %1").arg(problem));
        return {};
    }
    const auto unit = unitFactor(options.frequencyUnit);
    if (!unit) {
        setError(error, QStringLiteral("Unknown frequency unit '%1'").arg(options.frequencyUnit));
        return {};
    }
    if (options.version != 1 && options.version != 2) {
        setError(error, QStringLiteral("Unsupported Touchstone version %1").arg(options.version));
        return {};
    }
    const int n = network.portCount();
    const int precision = std::clamp(options.precision, 1, 17);
    // Frequencies are written with enough digits to survive the unit scaling.
    const int frequencyPrecision = std::max(precision, 15);
    const double r = network.z0.front();
    const bool sameReference =
        std::all_of(network.z0.begin(), network.z0.end(), [r](double z) { return z == r; });
    if (options.version == 1 && !sameReference) {
        setError(error, QStringLiteral("Touchstone version 1 needs the same reference impedance on all ports; use version 2"));
        return {};
    }
    if (options.version == 1 && network.hasNoiseParameters()
        && network.noiseFrequencies.front() > network.frequencies.back()) {
        setError(error, QStringLiteral("Touchstone version 1 needs the first noise frequency at or below the "
                                       "highest network frequency; use version 2"));
        return {};
    }

    const QString unitName = [&] {
        const QString u = options.frequencyUnit.toLower();
        if (u == QLatin1String("hz"))
            return QStringLiteral("Hz");
        if (u == QLatin1String("khz"))
            return QStringLiteral("kHz");
        if (u == QLatin1String("mhz"))
            return QStringLiteral("MHz");
        return QStringLiteral("GHz");
    }();

    QString out;
    QTextStream stream(&out);
    if (!network.comment.isEmpty()) {
        for (const QString &line : network.comment.split(QLatin1Char('\n')))
            stream << (line.isEmpty() ? QStringLiteral("!") : QStringLiteral("! ") + line) << '\n';
    }
    if (options.version == 2)
        stream << "[Version] 2.0\n";
    stream << "# " << unitName << " S " << formatName(options.format) << " R " << number(r, 17) << '\n';
    if (options.version == 2) {
        stream << "[Number of Ports] " << n << '\n';
        if (n == 2)
            stream << "[Two-Port Data Order] 12_21\n";
        stream << "[Number of Frequencies] " << network.frequencyCount() << '\n';
        if (network.hasNoiseParameters())
            stream << "[Number of Noise Frequencies] " << network.noise.size() << '\n';
        if (!sameReference) {
            stream << "[Reference]";
            for (double z : network.z0)
                stream << ' ' << number(z, 17);
            stream << '\n';
        }
        stream << "[Network Data]\n";
    }

    for (int k = 0; k < network.frequencyCount(); ++k) {
        const CMatrix &s = network.s[static_cast<size_t>(k)];
        const QString frequency = number(network.frequencies[static_cast<size_t>(k)] / *unit, frequencyPrecision);
        if (n <= 2) {
            QStringList fields{frequency};
            if (n == 1) {
                fields << pairText(s(0, 0), options.format, precision);
            } else if (options.version == 1) {
                for (const auto &[i, j] : {std::pair(0, 0), std::pair(1, 0), std::pair(0, 1), std::pair(1, 1)})
                    fields << pairText(s(i, j), options.format, precision);
            } else {
                for (const auto &[i, j] : {std::pair(0, 0), std::pair(0, 1), std::pair(1, 0), std::pair(1, 1)})
                    fields << pairText(s(i, j), options.format, precision);
            }
            stream << fields.join(QLatin1Char(' ')) << '\n';
            continue;
        }
        // One matrix row per line, at most four pairs per line in version 1.
        const int pairsPerLine = options.version == 1 ? 4 : n;
        for (int i = 0; i < n; ++i) {
            for (int j0 = 0; j0 < n; j0 += pairsPerLine) {
                QStringList fields;
                fields << ((i == 0 && j0 == 0) ? frequency : QString(frequency.size(), QLatin1Char(' ')));
                for (int j = j0; j < std::min(n, j0 + pairsPerLine); ++j)
                    fields << pairText(s(i, j), options.format, precision);
                stream << fields.join(QLatin1Char(' ')) << '\n';
            }
        }
    }

    if (network.hasNoiseParameters()) {
        if (options.version == 2)
            stream << "[Noise Data]\n";
        const double rnScale = options.version == 1 ? r : 1.0;
        for (size_t k = 0; k < network.noise.size(); ++k) {
            const NoiseParameters &noise = network.noise[k];
            stream << number(network.noiseFrequencies[k] / *unit, frequencyPrecision) << ' '
                   << number(noise.fminDb, precision) << ' ' << number(std::abs(noise.gammaOpt), precision) << ' '
                   << number(std::arg(noise.gammaOpt) * 180.0 / Pi, precision) << ' '
                   << number(noise.rn / rnScale, precision) << '\n';
        }
    }
    if (options.version == 2)
        stream << "[End]\n";
    stream.flush();
    return out.toUtf8();
}

bool writeTouchstone(const QString &path, const NetworkData &network, const TouchstoneWriteOptions &options,
                     QString *error)
{
    const QByteArray text = formatTouchstone(network, options, error);
    if (text.isEmpty())
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Cannot write %1: %2").arg(path, file.errorString()));
        return false;
    }
    file.write(text);
    if (!file.commit()) {
        setError(error, QStringLiteral("Cannot write %1: %2").arg(path, file.errorString()));
        return false;
    }
    return true;
}

} // namespace lvd::rf

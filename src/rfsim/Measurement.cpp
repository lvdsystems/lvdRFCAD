#include "rfsim/Measurement.h"

#include "rfdata/NetworkMath.h"
#include "rfdata/NetworkOps.h"

#include <QRegularExpression>

#include <cmath>
#include <functional>

namespace lvd::rf {

namespace {

using Values = std::vector<Complex>;

struct Value {
    Values data;
    bool isComplex = false;
    QString unit;
};

class Evaluator
{
public:
    Evaluator(const LinearResult &result, const QString &text) : m_result(result), m_text(text) {}

    std::optional<Value> run(QString *error)
    {
        skipSpace();
        auto value = expression();
        skipSpace();
        if (value && m_pos < m_text.size())
            fail(QStringLiteral("unexpected '%1' at position %2").arg(m_text.at(m_pos)).arg(m_pos + 1));
        if (!m_error.isEmpty()) {
            *error = m_error;
            return std::nullopt;
        }
        return value;
    }

private:
    int count() const { return static_cast<int>(m_result.network.frequencies.size()); }

    void fail(const QString &message)
    {
        if (m_error.isEmpty())
            m_error = message;
    }

    void skipSpace()
    {
        while (m_pos < m_text.size() && m_text.at(m_pos).isSpace())
            ++m_pos;
    }

    bool accept(QChar c)
    {
        skipSpace();
        if (m_pos < m_text.size() && m_text.at(m_pos) == c) {
            ++m_pos;
            return true;
        }
        return false;
    }

    Value constant(double number) { return Value{Values(static_cast<size_t>(count()), Complex(number, 0)), false, {}}; }

    std::optional<Value> expression()
    {
        auto left = term();
        while (left) {
            if (accept(QLatin1Char('+')))
                left = combine(*left, term(), [](Complex a, Complex b) { return a + b; }, true);
            else if (accept(QLatin1Char('-')))
                left = combine(*left, term(), [](Complex a, Complex b) { return a - b; }, true);
            else
                break;
        }
        return left;
    }

    std::optional<Value> term()
    {
        auto left = factor();
        while (left) {
            if (accept(QLatin1Char('*')))
                left = combine(*left, factor(), [](Complex a, Complex b) { return a * b; }, false);
            else if (accept(QLatin1Char('/')))
                left = combine(*left, factor(), [](Complex a, Complex b) { return a / b; }, false);
            else
                break;
        }
        return left;
    }

    std::optional<Value> combine(const Value &left, const std::optional<Value> &right,
                                 const std::function<Complex(Complex, Complex)> &op, bool keepUnit)
    {
        if (!right)
            return std::nullopt;
        Value out;
        out.isComplex = left.isComplex || right->isComplex;
        out.unit = keepUnit && left.unit == right->unit ? left.unit : QString();
        out.data.resize(left.data.size());
        for (size_t i = 0; i < left.data.size(); ++i)
            out.data[i] = op(left.data[i], right->data[i]);
        return out;
    }

    std::optional<Value> factor()
    {
        if (accept(QLatin1Char('-'))) {
            auto inner = factor();
            if (inner) {
                for (Complex &v : inner->data)
                    v = -v;
            }
            return inner;
        }
        if (accept(QLatin1Char('('))) {
            auto inner = expression();
            if (!accept(QLatin1Char(')')))
                fail(QStringLiteral("missing ')' at position %1").arg(m_pos + 1));
            return inner;
        }
        skipSpace();
        if (m_pos >= m_text.size()) {
            fail(QStringLiteral("unexpected end of the expression"));
            return std::nullopt;
        }
        const QChar c = m_text.at(m_pos);
        if (c.isDigit() || c == QLatin1Char('.')) {
            const qsizetype start = m_pos;
            while (m_pos < m_text.size()
                   && (m_text.at(m_pos).isDigit() || m_text.at(m_pos) == QLatin1Char('.')
                       || m_text.at(m_pos).toLower() == QLatin1Char('e')
                       || ((m_text.at(m_pos) == QLatin1Char('-') || m_text.at(m_pos) == QLatin1Char('+'))
                           && m_text.at(m_pos - 1).toLower() == QLatin1Char('e'))))
                ++m_pos;
            bool ok = false;
            const double number = m_text.mid(start, m_pos - start).toDouble(&ok);
            if (!ok) {
                fail(QStringLiteral("invalid number at position %1").arg(start + 1));
                return std::nullopt;
            }
            return constant(number);
        }
        if (!c.isLetter()) {
            fail(QStringLiteral("unexpected '%1' at position %2").arg(c).arg(m_pos + 1));
            return std::nullopt;
        }
        const qsizetype start = m_pos;
        while (m_pos < m_text.size() && (m_text.at(m_pos).isLetterOrNumber() || m_text.at(m_pos) == QLatin1Char('_')))
            ++m_pos;
        const QString name = m_text.mid(start, m_pos - start);

        // Arguments: expressions, except that port indices are plain integers.
        QList<Value> arguments;
        QList<int> integers;
        if (accept(QLatin1Char('('))) {
            if (!accept(QLatin1Char(')'))) {
                do {
                    skipSpace();
                    static const QRegularExpression integer(QStringLiteral("^\\d+\\s*[,)]"));
                    const auto match = integer.matchView(QStringView(m_text).mid(m_pos));
                    if (match.hasMatch() && takesPorts(name)) {
                        const qsizetype digitsEnd = m_pos + match.capturedLength() - 1;
                        integers.append(m_text.mid(m_pos, digitsEnd - m_pos).trimmed().toInt());
                        m_pos = digitsEnd;
                    } else {
                        auto argument = expression();
                        if (!argument)
                            return std::nullopt;
                        arguments.append(*argument);
                    }
                } while (accept(QLatin1Char(',')));
                if (!accept(QLatin1Char(')'))) {
                    fail(QStringLiteral("missing ')' after the arguments of %1").arg(name));
                    return std::nullopt;
                }
            }
        }
        return call(name, arguments, integers);
    }

    static bool takesPorts(const QString &name)
    {
        static const QStringList names = {QStringLiteral("S"), QStringLiteral("Z"), QStringLiteral("Y"),
                                          QStringLiteral("Zin"), QStringLiteral("Yin"), QStringLiteral("VSWR"),
                                          QStringLiteral("RL"), QStringLiteral("NF")};
        return names.contains(name);
    }

    // Port number -> index into the result, or -1.
    int portIndex(int number)
    {
        for (size_t i = 0; i < m_result.portNumbers.size(); ++i) {
            if (m_result.portNumbers[i] == number)
                return static_cast<int>(i);
        }
        fail(QStringLiteral("there is no port %1").arg(number));
        return -1;
    }

    bool requireTwoPort(const QString &name)
    {
        if (m_result.network.portCount() != 2) {
            fail(QStringLiteral("%1 needs a two-port result").arg(name));
            return false;
        }
        return true;
    }

    std::optional<Value> perFrequency(const std::function<Complex(int)> &function, bool isComplex, const QString &unit)
    {
        Value out{Values(static_cast<size_t>(count())), isComplex, unit};
        for (int i = 0; i < count(); ++i)
            out.data[static_cast<size_t>(i)] = function(i);
        return out;
    }

    std::optional<Value> networkParameter(QChar kind, int row, int col)
    {
        const int r = portIndex(row), c = portIndex(col);
        if (r < 0 || c < 0)
            return std::nullopt;
        const NetworkData &n = m_result.network;
        if (kind == QLatin1Char('S'))
            return perFrequency([&](int i) { return n.s[static_cast<size_t>(i)](r, c); }, true, {});
        bool missing = false;
        auto value = perFrequency(
            [&](int i) {
                const auto m = kind == QLatin1Char('Z') ? sToZ(n.s[static_cast<size_t>(i)], n.z0)
                                                        : sToY(n.s[static_cast<size_t>(i)], n.z0);
                if (!m) {
                    missing = true;
                    return Complex(std::nan(""), std::nan(""));
                }
                return (*m)(r, c);
            },
            true, kind == QLatin1Char('Z') ? QStringLiteral("ohm") : QStringLiteral("S"));
        Q_UNUSED(missing) // undefined points are NaN and left out of plots
        return value;
    }

    std::optional<Value> noiseFigure(int outputNumber, int inputNumber)
    {
        if (!m_result.network.hasNoiseCorrelation()) {
            fail(QStringLiteral("the analysis was run without noise"));
            return std::nullopt;
        }
        const int o = portIndex(outputNumber), in = portIndex(inputNumber);
        if (o < 0 || in < 0)
            return std::nullopt;
        const NetworkData &n = m_result.network;
        return perFrequency(
            [&](int i) {
                const double gain = std::norm(n.s[static_cast<size_t>(i)](o, in));
                const double added = n.noiseCorrelation[static_cast<size_t>(i)](o, o).real();
                return Complex(10.0 * std::log10(1.0 + added / (T0 * gain)), 0);
            },
            false, QStringLiteral("dB"));
    }

    std::optional<Value> noiseParameter(const QString &name)
    {
        if (!requireTwoPort(name))
            return std::nullopt;
        if (!m_result.network.hasNoiseCorrelation()) {
            fail(QStringLiteral("the analysis was run without noise"));
            return std::nullopt;
        }
        const NetworkData &n = m_result.network;
        const auto parameters = [&](int i) {
            return noiseParametersFromCorrelation(n.s[static_cast<size_t>(i)], n.noiseCorrelation[static_cast<size_t>(i)],
                                                  n.z0[0]);
        };
        if (name == QLatin1String("NFmin"))
            return perFrequency([&](int i) { return Complex(parameters(i).fminDb, 0); }, false, QStringLiteral("dB"));
        if (name == QLatin1String("Rn"))
            return perFrequency([&](int i) { return Complex(parameters(i).rn, 0); }, false, QStringLiteral("ohm"));
        return perFrequency([&](int i) { return parameters(i).gammaOpt; }, true, {});
    }

    std::optional<Value> call(const QString &name, const QList<Value> &arguments, const QList<int> &integers)
    {
        // Sij shorthand with single-digit port numbers.
        static const QRegularExpression shorthand(QStringLiteral("^([SZY])(\\d)(\\d)$"));
        if (const auto match = shorthand.match(name); match.hasMatch() && arguments.isEmpty() && integers.isEmpty())
            return networkParameter(match.captured(1).at(0), match.captured(2).toInt(), match.captured(3).toInt());

        const auto needInts = [&](int n) {
            if (integers.size() != n || !arguments.isEmpty()) {
                fail(QStringLiteral("%1 takes %2 port number(s)").arg(name).arg(n));
                return false;
            }
            return true;
        };
        const auto needOne = [&]() {
            if (arguments.size() != 1 || !integers.isEmpty()) {
                fail(QStringLiteral("%1 takes one argument").arg(name));
                return false;
            }
            return true;
        };
        const NetworkData &n = m_result.network;

        if (name == QLatin1String("S") || name == QLatin1String("Z") || name == QLatin1String("Y")) {
            if (!needInts(2))
                return std::nullopt;
            return networkParameter(name.at(0), integers[0], integers[1]);
        }
        if (name == QLatin1String("Zin") || name == QLatin1String("Yin") || name == QLatin1String("VSWR")
            || name == QLatin1String("RL")) {
            if (!needInts(1))
                return std::nullopt;
            const int k = portIndex(integers[0]);
            if (k < 0)
                return std::nullopt;
            const double z0 = n.z0[static_cast<size_t>(k)];
            if (name == QLatin1String("Zin"))
                return perFrequency([&](int i) { return impedanceFromReflection(n.s[static_cast<size_t>(i)](k, k), z0); },
                                    true, QStringLiteral("ohm"));
            if (name == QLatin1String("Yin"))
                return perFrequency(
                    [&](int i) { return 1.0 / impedanceFromReflection(n.s[static_cast<size_t>(i)](k, k), z0); }, true,
                    QStringLiteral("S"));
            if (name == QLatin1String("VSWR"))
                return perFrequency([&](int i) { return Complex(vswr(n.s[static_cast<size_t>(i)](k, k)), 0); }, false, {});
            return perFrequency([&](int i) { return Complex(-db20(n.s[static_cast<size_t>(i)](k, k)), 0); }, false,
                                QStringLiteral("dB"));
        }
        if (name == QLatin1String("NF")) {
            if (integers.isEmpty() && arguments.isEmpty()) {
                if (!requireTwoPort(name))
                    return std::nullopt;
                return noiseFigure(m_result.portNumbers[1], m_result.portNumbers[0]);
            }
            if (!needInts(2))
                return std::nullopt;
            return noiseFigure(integers[0], integers[1]);
        }
        if (name == QLatin1String("NFmin") || name == QLatin1String("Rn") || name == QLatin1String("GammaOpt")) {
            if (!integers.isEmpty() || !arguments.isEmpty()) {
                fail(QStringLiteral("%1 takes no arguments").arg(name));
                return std::nullopt;
            }
            return noiseParameter(name);
        }
        if (name == QLatin1String("K") || name == QLatin1String("Delta") || name == QLatin1String("mu")
            || name == QLatin1String("mup") || name == QLatin1String("MAG") || name == QLatin1String("GUmax")
            || name == QLatin1String("U") || name == QLatin1String("GammaMS") || name == QLatin1String("GammaML")) {
            if (!integers.isEmpty() || !arguments.isEmpty()) {
                fail(QStringLiteral("%1 takes no arguments").arg(name));
                return std::nullopt;
            }
            if (!requireTwoPort(name))
                return std::nullopt;
            const auto s = [&](int i) -> const CMatrix & { return n.s[static_cast<size_t>(i)]; };
            if (name == QLatin1String("K"))
                return perFrequency([&](int i) { return Complex(stabilityK(s(i)), 0); }, false, {});
            if (name == QLatin1String("Delta"))
                return perFrequency([&](int i) { return stabilityDelta(s(i)); }, true, {});
            if (name == QLatin1String("mu"))
                return perFrequency([&](int i) { return Complex(stabilityMu(s(i)), 0); }, false, {});
            if (name == QLatin1String("mup"))
                return perFrequency([&](int i) { return Complex(stabilityMuPrime(s(i)), 0); }, false, {});
            if (name == QLatin1String("GUmax"))
                return perFrequency([&](int i) { return Complex(maximumUnilateralGainDb(s(i)), 0); }, false,
                                    QStringLiteral("dB"));
            if (name == QLatin1String("U"))
                return perFrequency([&](int i) { return Complex(masonUnilateralGainDb(s(i)), 0); }, false,
                                    QStringLiteral("dB"));
            if (name == QLatin1String("GammaMS") || name == QLatin1String("GammaML")) {
                const bool source = name == QLatin1String("GammaMS");
                return perFrequency(
                    [&](int i) {
                        const auto match = simultaneousConjugateMatch(s(i));
                        if (!match)
                            return Complex(std::nan(""), std::nan("")); // K <= 1: no simultaneous match
                        return source ? match->gammaS : match->gammaL;
                    },
                    true, {});
            }
            return perFrequency([&](int i) { return Complex(maximumAvailableGainDb(s(i)), 0); }, false,
                                QStringLiteral("dB"));
        }

        // Functions of one value
        static const QStringList unary = {QStringLiteral("dB"),  QStringLiteral("dB10"), QStringLiteral("mag"),
                                          QStringLiteral("phase"), QStringLiteral("uphase"), QStringLiteral("re"),
                                          QStringLiteral("im"),  QStringLiteral("conj"), QStringLiteral("GD"),
                                          QStringLiteral("abs")};
        if (unary.contains(name)) {
            if (!needOne())
                return std::nullopt;
            const Value &x = arguments.first();
            Value out{Values(x.data.size()), false, {}};
            if (name == QLatin1String("dB") || name == QLatin1String("dB10")) {
                const double factor = name == QLatin1String("dB") ? 20.0 : 10.0;
                for (size_t i = 0; i < x.data.size(); ++i)
                    out.data[i] = factor * std::log10(std::abs(x.data[i]));
                out.unit = QStringLiteral("dB");
            } else if (name == QLatin1String("mag") || name == QLatin1String("abs")) {
                for (size_t i = 0; i < x.data.size(); ++i)
                    out.data[i] = std::abs(x.data[i]);
                out.unit = x.unit;
            } else if (name == QLatin1String("phase")) {
                for (size_t i = 0; i < x.data.size(); ++i)
                    out.data[i] = std::arg(x.data[i]) * 180.0 / Pi;
                out.unit = QStringLiteral("deg");
            } else if (name == QLatin1String("uphase") || name == QLatin1String("GD")) {
                std::vector<double> phase(x.data.size());
                for (size_t i = 0; i < x.data.size(); ++i) {
                    phase[i] = std::arg(x.data[i]);
                    if (i > 0) {
                        while (phase[i] - phase[i - 1] > Pi)
                            phase[i] -= 2 * Pi;
                        while (phase[i] - phase[i - 1] < -Pi)
                            phase[i] += 2 * Pi;
                    }
                }
                if (name == QLatin1String("uphase")) {
                    for (size_t i = 0; i < phase.size(); ++i)
                        out.data[i] = phase[i] * 180.0 / Pi;
                    out.unit = QStringLiteral("deg");
                } else {
                    const std::vector<double> &f = n.frequencies;
                    const size_t last = phase.size() - 1;
                    for (size_t i = 0; i < phase.size(); ++i) {
                        if (phase.size() < 2) {
                            out.data[i] = std::nan("");
                            continue;
                        }
                        const size_t a = i == 0 ? 0 : (i == last ? last - 1 : i - 1);
                        const size_t b = i == 0 ? 1 : (i == last ? last : i + 1);
                        out.data[i] = -(phase[b] - phase[a]) / (2 * Pi * (f[b] - f[a]));
                    }
                    out.unit = QStringLiteral("s");
                }
            } else if (name == QLatin1String("re")) {
                for (size_t i = 0; i < x.data.size(); ++i)
                    out.data[i] = x.data[i].real();
                out.unit = x.unit;
            } else if (name == QLatin1String("im")) {
                for (size_t i = 0; i < x.data.size(); ++i)
                    out.data[i] = x.data[i].imag();
                out.unit = x.unit;
            } else { // conj
                for (size_t i = 0; i < x.data.size(); ++i)
                    out.data[i] = std::conj(x.data[i]);
                out.isComplex = x.isComplex;
                out.unit = x.unit;
            }
            return out;
        }
        fail(QStringLiteral("unknown measurement \"%1\"").arg(name));
        return std::nullopt;
    }

    const LinearResult &m_result;
    QString m_text;
    qsizetype m_pos = 0;
    QString m_error;
};

} // namespace

std::vector<double> Measurement::real() const
{
    std::vector<double> result(values.size());
    for (size_t i = 0; i < values.size(); ++i)
        result[i] = values[i].real();
    return result;
}

Measurement evaluateMeasurement(const LinearResult &result, const QString &expression)
{
    Measurement measurement;
    measurement.frequencies = result.network.frequencies;
    if (!result.ok()) {
        measurement.error = QStringLiteral("there is no simulation result");
        return measurement;
    }
    if (expression.trimmed().isEmpty()) {
        measurement.error = QStringLiteral("the measurement is empty");
        return measurement;
    }
    Evaluator evaluator(result, expression);
    const auto value = evaluator.run(&measurement.error);
    if (value) {
        measurement.values = value->data;
        measurement.isComplex = value->isComplex;
        measurement.unit = value->unit;
    }
    return measurement;
}

QStringList defaultMeasurements(const LinearResult &result)
{
    QStringList list;
    const std::vector<int> &ports = result.portNumbers;
    const size_t n = std::min<size_t>(ports.size(), 4);
    for (size_t r = 0; r < n; ++r) {
        for (size_t c = 0; c < n; ++c)
            list.append(QStringLiteral("dB(S(%1,%2))").arg(ports[r]).arg(ports[c]));
    }
    if (ports.size() == 2 && result.network.hasNoiseCorrelation())
        list.append(QStringLiteral("NF"));
    return list;
}

} // namespace lvd::rf

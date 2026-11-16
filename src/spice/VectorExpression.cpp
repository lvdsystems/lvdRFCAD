#include "spice/VectorExpression.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace lvd::spice {

namespace {

constexpr double Pi = std::numbers::pi;
constexpr char16_t MicroSign = 0x00B5;
constexpr char16_t GreekMu = 0x03BC;
constexpr char16_t GreekOmega = 0x03A9;
constexpr char16_t OhmSign = 0x2126;

enum class Function {
    Db, Db10, Mag, Phase, Cph, Real, Imag, Conj,
    Sqrt, Exp, Ln, Log10, Sin, Cos, Tan, Atan, Sinh, Cosh, Tanh,
    Deriv, Integ, GroupDelay, Mean, Rms, Max, Min, PeakToPeak
};

struct FunctionName {
    const char *name;
    Function function;
};

// ngspice spells log10 "log"; it is accepted as an alias for users coming from there.
constexpr FunctionName functionNames[] = {
    {"db", Function::Db},       {"db10", Function::Db10},     {"mag", Function::Mag},
    {"abs", Function::Mag},     {"ph", Function::Phase},      {"phase", Function::Phase},
    {"cph", Function::Cph},     {"real", Function::Real},     {"re", Function::Real},
    {"imag", Function::Imag},   {"im", Function::Imag},       {"conj", Function::Conj},
    {"sqrt", Function::Sqrt},   {"exp", Function::Exp},       {"ln", Function::Ln},
    {"log10", Function::Log10}, {"log", Function::Log10},     {"sin", Function::Sin},
    {"cos", Function::Cos},     {"tan", Function::Tan},       {"atan", Function::Atan},
    {"sinh", Function::Sinh},   {"cosh", Function::Cosh},     {"tanh", Function::Tanh},
    {"deriv", Function::Deriv}, {"integ", Function::Integ},   {"gd", Function::GroupDelay},
    {"groupdelay", Function::GroupDelay},                     {"mean", Function::Mean},
    {"avg", Function::Mean},    {"rms", Function::Rms},       {"max", Function::Max},
    {"min", Function::Min},     {"pkpk", Function::PeakToPeak},
};

std::optional<Function> findFunction(const QString &lowerName)
{
    for (const auto &entry : functionNames) {
        if (lowerName == QLatin1String(entry.name))
            return entry.function;
    }
    return std::nullopt;
}

std::optional<Complex> findConstant(const QString &lowerName)
{
    if (lowerName == QLatin1String("pi"))
        return Complex(Pi, 0);
    if (lowerName == QLatin1String("e"))
        return Complex(std::numbers::e, 0);
    if (lowerName == QLatin1String("j"))
        return Complex(0, 1);
    return std::nullopt;
}

bool isUnitWord(const QString &word)
{
    static const char *const units[] = {"v", "a", "s", "hz", "f", "h", "w", "ohm", "deg", "db"};
    const QString lower = word.toLower();
    for (const char *unit : units) {
        if (lower == QLatin1String(unit))
            return true;
    }
    return word == QString(QChar(GreekOmega)) || word == QString(QChar(OhmSign));
}

std::optional<double> prefixValue(QChar c)
{
    switch (c.unicode()) {
    case u'a': return 1e-18;
    case u'f': return 1e-15;
    case u'p': return 1e-12;
    case u'n': return 1e-9;
    case u'u':
    case MicroSign:
    case GreekMu: return 1e-6;
    case u'm': return 1e-3;
    case u'k': return 1e3;
    case u'M': return 1e6;
    case u'G': return 1e9;
    case u'T': return 1e12;
    default: return std::nullopt;
    }
}

bool isLetter(QChar c)
{
    return c.isLetter();
}

bool isNameStart(QChar c)
{
    return c.isLetter() || c == QLatin1Char('_');
}

bool isNameChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('#') || c == QLatin1Char('.');
}

} // namespace

struct VectorExpression::Node {
    enum class Kind { Number, Vector, Negate, Binary, Call };

    Kind kind = Kind::Number;
    Complex value;     // Number
    QString name;      // Vector: canonical name
    QString unit;      // Vector: "V" for v(), "A" for i(), empty to take the resolver's
    char16_t op = 0;   // Binary
    Function function = Function::Db;
    QString functionName; // as typed, for messages
    std::vector<std::shared_ptr<const Node>> args;
};

namespace {

using Node = VectorExpression::Node;
using NodePtr = std::shared_ptr<const Node>;

// ---------------------------------------------------------------------------
// Parser: recursive descent, precedence  + -  <  * /  <  unary -  <  ^

class Parser
{
public:
    explicit Parser(const QString &text) : m_text(text) {}

    NodePtr run(QString *error)
    {
        skipSpace();
        if (m_pos >= m_text.size()) {
            setError(QStringLiteral("empty expression"));
        } else {
            auto node = expression();
            skipSpace();
            if (node && m_pos < m_text.size())
                setError(QStringLiteral("unexpected '%1' at position %2").arg(m_text.at(m_pos)).arg(m_pos + 1));
            if (m_error.isEmpty())
                return node;
        }
        if (error)
            *error = m_error;
        return nullptr;
    }

private:
    void setError(const QString &message)
    {
        if (m_error.isEmpty())
            m_error = message;
    }

    void skipSpace()
    {
        while (m_pos < m_text.size() && m_text.at(m_pos).isSpace())
            ++m_pos;
    }

    QChar peek(qsizetype offset = 0) const
    {
        const qsizetype i = m_pos + offset;
        return i < m_text.size() ? m_text.at(i) : QChar();
    }

    bool accept(QChar c)
    {
        skipSpace();
        if (peek() == c) {
            ++m_pos;
            return true;
        }
        return false;
    }

    static NodePtr binary(char16_t op, NodePtr a, NodePtr b)
    {
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Binary;
        node->op = op;
        node->args = {std::move(a), std::move(b)};
        return node;
    }

    NodePtr expression()
    {
        auto left = term();
        while (left) {
            skipSpace();
            const QChar c = peek();
            if (c != QLatin1Char('+') && c != QLatin1Char('-'))
                break;
            ++m_pos;
            auto right = term();
            if (!right)
                return nullptr;
            left = binary(c.unicode(), left, right);
        }
        return left;
    }

    NodePtr term()
    {
        auto left = unary();
        while (left) {
            skipSpace();
            const QChar c = peek();
            if (c != QLatin1Char('*') && c != QLatin1Char('/'))
                break;
            ++m_pos;
            auto right = unary();
            if (!right)
                return nullptr;
            left = binary(c.unicode(), left, right);
        }
        return left;
    }

    NodePtr unary()
    {
        skipSpace();
        if (peek() == QLatin1Char('+')) {
            ++m_pos;
            return unary();
        }
        if (peek() == QLatin1Char('-')) {
            ++m_pos;
            auto operand = unary();
            if (!operand)
                return nullptr;
            auto node = std::make_shared<Node>();
            node->kind = Node::Kind::Negate;
            node->args = {operand};
            return node;
        }
        return power();
    }

    NodePtr power()
    {
        auto base = primary();
        if (base && accept(QLatin1Char('^'))) {
            auto exponent = unary(); // right-associative, and 2^-1 works
            if (!exponent)
                return nullptr;
            return binary(u'^', base, exponent);
        }
        return base;
    }

    NodePtr primary()
    {
        skipSpace();
        if (m_pos >= m_text.size()) {
            setError(QStringLiteral("unexpected end of expression"));
            return nullptr;
        }
        const QChar c = peek();
        if (c == QLatin1Char('(')) {
            ++m_pos;
            auto inner = expression();
            if (!inner)
                return nullptr;
            if (!accept(QLatin1Char(')'))) {
                setError(QStringLiteral("missing ')' at position %1").arg(m_pos + 1));
                return nullptr;
            }
            return inner;
        }
        if (c.isDigit() || (c == QLatin1Char('.') && peek(1).isDigit()))
            return number();
        if (isNameStart(c))
            return name();
        setError(QStringLiteral("unexpected '%1' at position %2").arg(c).arg(m_pos + 1));
        return nullptr;
    }

    qsizetype readDigits()
    {
        const qsizetype start = m_pos;
        while (peek().isDigit())
            ++m_pos;
        return m_pos - start;
    }

    // Length of the SI prefix at the current position (3 for meg), 0 if none.
    qsizetype prefixLength() const
    {
        if (m_text.mid(m_pos, 3).compare(QLatin1String("meg"), Qt::CaseInsensitive) == 0)
            return 3;
        return prefixValue(peek()) ? 1 : 0;
    }

    double prefixMultiplier(qsizetype length) const
    {
        return length == 3 ? 1e6 : *prefixValue(peek());
    }

    NodePtr number()
    {
        const qsizetype start = m_pos;
        QString mantissa;
        double multiplier = 1.0;
        bool prefixDone = false;

        readDigits();
        mantissa = m_text.mid(start, m_pos - start);
        if (peek() == QLatin1Char('.')) {
            ++m_pos;
            const qsizetype fraction = m_pos;
            readDigits();
            mantissa += QLatin1Char('.') + m_text.mid(fraction, m_pos - fraction);
        } else if (const qsizetype length = prefixLength(); length > 0 && peek(length).isDigit()) {
            // European notation: the prefix stands for the decimal point (4n7 = 4.7n).
            multiplier = prefixMultiplier(length);
            m_pos += length;
            const qsizetype fraction = m_pos;
            readDigits();
            mantissa += QLatin1Char('.') + m_text.mid(fraction, m_pos - fraction);
            prefixDone = true;
        }
        if (!prefixDone && (peek() == QLatin1Char('e') || peek() == QLatin1Char('E'))) {
            const bool sign = peek(1) == QLatin1Char('+') || peek(1) == QLatin1Char('-');
            if (peek(sign ? 2 : 1).isDigit()) {
                const qsizetype exponent = m_pos;
                m_pos += sign ? 2 : 1;
                readDigits();
                mantissa += m_text.mid(exponent, m_pos - exponent);
            }
        }
        if (!prefixDone) {
            if (const qsizetype length = prefixLength(); length > 0) {
                multiplier = prefixMultiplier(length);
                m_pos += length;
            }
        }
        // A unit after the number is allowed and ignored (10mV, 2.4GHz), but
        // anything else glued to it is a mistake such as 2j for 2*j.
        const qsizetype unitStart = m_pos;
        while (isLetter(peek()))
            ++m_pos;
        if (m_pos > unitStart) {
            const QString unit = m_text.mid(unitStart, m_pos - unitStart);
            if (!isUnitWord(unit)) {
                setError(QStringLiteral("unknown unit '%1' after the number at position %2 (write * to multiply)")
                             .arg(unit)
                             .arg(start + 1));
                return nullptr;
            }
        }
        bool ok = false;
        const double value = mantissa.toDouble(&ok);
        if (!ok) {
            setError(QStringLiteral("invalid number at position %1").arg(start + 1));
            return nullptr;
        }
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Number;
        node->value = Complex(value * multiplier, 0);
        return node;
    }

    static NodePtr vectorNode(const QString &name, const QString &unit)
    {
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Vector;
        node->name = name;
        node->unit = unit;
        return node;
    }

    // v(node), v(a,b) or i(device): the arguments are raw names, which may
    // contain characters the expression syntax does not allow (v(1), v(x1.n2)).
    NodePtr probe(QChar kind)
    {
        const qsizetype open = m_pos;
        ++m_pos; // '('
        QStringList names;
        qsizetype start = m_pos;
        for (;;) {
            if (m_pos >= m_text.size()) {
                setError(QStringLiteral("missing ')' after '%1(' at position %2").arg(kind).arg(open + 1));
                return nullptr;
            }
            const QChar c = m_text.at(m_pos);
            if (c == QLatin1Char(',') || c == QLatin1Char(')')) {
                const QString name = m_text.mid(start, m_pos - start).trimmed().toLower();
                if (name.isEmpty()) {
                    setError(QStringLiteral("missing name in '%1()' at position %2").arg(kind).arg(m_pos + 1));
                    return nullptr;
                }
                names << name;
                ++m_pos;
                if (c == QLatin1Char(')'))
                    break;
                start = m_pos;
            } else if (c == QLatin1Char('(')) {
                setError(QStringLiteral("unexpected '(' at position %1").arg(m_pos + 1));
                return nullptr;
            } else {
                ++m_pos;
            }
        }
        const bool voltage = kind == QLatin1Char('v');
        if (names.size() > (voltage ? 2 : 1)) {
            setError(voltage ? QStringLiteral("v() takes one or two nodes")
                             : QStringLiteral("i() takes one device name"));
            return nullptr;
        }
        const QString unit = voltage ? QStringLiteral("V") : QStringLiteral("A");
        const auto first = vectorNode(QStringLiteral("%1(%2)").arg(kind, names[0]), unit);
        if (names.size() == 1)
            return first;
        return binary(u'-', first, vectorNode(QStringLiteral("v(%1)").arg(names[1]), unit));
    }

    NodePtr name()
    {
        const qsizetype start = m_pos;
        while (m_pos < m_text.size() && isNameChar(m_text.at(m_pos)))
            ++m_pos;
        const QString typed = m_text.mid(start, m_pos - start);
        const QString lower = typed.toLower();
        const qsizetype afterName = m_pos;
        skipSpace();
        const bool call = peek() == QLatin1Char('(');
        if (!call) {
            m_pos = afterName;
            if (const auto constant = findConstant(lower)) {
                auto node = std::make_shared<Node>();
                node->kind = Node::Kind::Number;
                node->value = *constant;
                return node;
            }
            if (findFunction(lower)) {
                setError(QStringLiteral("function '%1' needs an argument in parentheses").arg(typed));
                return nullptr;
            }
            return vectorNode(lower, QString());
        }
        if (lower == QLatin1String("v") || lower == QLatin1String("i"))
            return probe(lower.at(0));
        const auto function = findFunction(lower);
        if (!function) {
            setError(QStringLiteral("unknown function '%1'").arg(typed));
            return nullptr;
        }
        ++m_pos; // '('
        std::vector<NodePtr> args;
        skipSpace();
        if (peek() != QLatin1Char(')')) {
            for (;;) {
                auto arg = expression();
                if (!arg)
                    return nullptr;
                args.push_back(arg);
                if (!accept(QLatin1Char(',')))
                    break;
            }
        }
        if (!accept(QLatin1Char(')'))) {
            setError(QStringLiteral("missing ')' at position %1").arg(m_pos + 1));
            return nullptr;
        }
        if (args.size() != 1) {
            setError(QStringLiteral("'%1' takes one argument, not %2").arg(typed).arg(args.size()));
            return nullptr;
        }
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Call;
        node->function = *function;
        node->functionName = typed;
        node->args = std::move(args);
        return node;
    }

    const QString &m_text;
    qsizetype m_pos = 0;
    QString m_error;
};

// ---------------------------------------------------------------------------
// Evaluation

using Scale = std::shared_ptr<const std::vector<double>>;

struct Value {
    Scale x; // null for a scalar
    std::vector<Complex> y;
    bool isComplex = false;
    QString unit;

    bool scalar() const { return !x; }
    bool plainNumber() const { return scalar() && unit.isEmpty(); }
};

Value scalarValue(Complex v, bool isComplex, const QString &unit = QString())
{
    return Value{nullptr, {v}, isComplex, unit};
}

// Derivative along a non-uniform scale: three-point formulas (exact for
// quadratics) inside and at the ends, two-point where steps repeat an x.
std::vector<Complex> derivative(const std::vector<double> &x, const std::vector<Complex> &y)
{
    const std::size_t n = y.size();
    std::vector<Complex> d(n, Complex(0, 0));
    if (n < 2)
        return d;
    const auto slope = [&](std::size_t a, std::size_t b) {
        return x[b] > x[a] ? (y[b] - y[a]) / (x[b] - x[a]) : Complex(0, 0);
    };
    for (std::size_t i = 0; i < n; ++i) {
        if (i == 0 || i == n - 1) {
            const bool start = i == 0;
            const std::size_t i1 = start ? 1 : n - 2;
            const std::size_t i2 = start ? 2 : n - 3;
            const double h1 = std::abs(x[i1] - x[i]);
            const double h2 = n >= 3 ? std::abs(x[i2] - x[i1]) : 0.0;
            if (h1 > 0 && h2 > 0) {
                // One-sided second-order formula; mirrored at the end.
                const double s = start ? 1.0 : -1.0;
                d[i] = s * (-(2 * h1 + h2) / (h1 * (h1 + h2)) * y[i] + (h1 + h2) / (h1 * h2) * y[i1]
                            - h1 / (h2 * (h1 + h2)) * y[i2]);
            } else {
                d[i] = start ? slope(0, 1) : slope(n - 2, n - 1);
            }
            continue;
        }
        const double h1 = x[i] - x[i - 1];
        const double h2 = x[i + 1] - x[i];
        if (h1 > 0 && h2 > 0) {
            d[i] = -h2 / (h1 * (h1 + h2)) * y[i - 1] + (h2 - h1) / (h1 * h2) * y[i] + h1 / (h2 * (h1 + h2)) * y[i + 1];
        } else if (h2 > 0) {
            d[i] = slope(i, i + 1);
        } else {
            d[i] = slope(i - 1, i);
        }
    }
    return d;
}

std::vector<double> unwrappedPhase(const std::vector<Complex> &y)
{
    std::vector<double> phase(y.size());
    for (std::size_t i = 0; i < y.size(); ++i) {
        phase[i] = std::arg(y[i]);
        if (i > 0) {
            const double step = phase[i] - phase[i - 1];
            phase[i] -= 2 * Pi * std::round(step / (2 * Pi));
        }
    }
    return phase;
}

double dB(double magnitude, double factor)
{
    // A floor instead of -inf keeps plots and axis scaling finite.
    return factor * std::log10(std::max(magnitude, std::numeric_limits<double>::min()));
}

class Evaluator
{
public:
    Evaluator(const VectorExpression::Resolver &resolve) : m_resolve(resolve) {}

    // Real values keep +0 imaginary parts: -0 (from negation or products of
    // negative numbers) would put them on the branch cut of arg and sqrt.
    std::optional<Value> eval(const Node &node)
    {
        auto value = evalNode(node);
        if (value && !value->isComplex) {
            for (auto &y : value->y)
                y = Complex(y.real(), 0.0);
        }
        return value;
    }

    QString error;

private:
    std::optional<Value> evalNode(const Node &node)
    {
        switch (node.kind) {
        case Node::Kind::Number:
            return scalarValue(node.value, node.value.imag() != 0);
        case Node::Kind::Vector:
            return vector(node);
        case Node::Kind::Negate: {
            auto v = eval(*node.args[0]);
            if (v) {
                for (auto &y : v->y)
                    y = -y;
            }
            return v;
        }
        case Node::Kind::Binary: {
            auto a = eval(*node.args[0]);
            if (!a)
                return std::nullopt;
            auto b = eval(*node.args[1]);
            if (!b)
                return std::nullopt;
            return binary(node.op, *a, *b);
        }
        case Node::Kind::Call: {
            auto a = eval(*node.args[0]);
            if (!a)
                return std::nullopt;
            return call(node, std::move(*a));
        }
        }
        return std::nullopt;
    }

    std::optional<Value> fail(const QString &message)
    {
        if (error.isEmpty())
            error = message;
        return std::nullopt;
    }

    std::optional<Value> vector(const Node &node)
    {
        std::optional<Waveform> w;
        if (m_resolve)
            w = m_resolve(node.name);
        if (!w)
            return fail(QStringLiteral("unknown vector '%1'").arg(node.name));
        if (w->x.size() != w->y.size()) {
            return fail(QStringLiteral("vector '%1' has %2 values but a scale of %3 points")
                            .arg(node.name)
                            .arg(w->y.size())
                            .arg(w->x.size()));
        }
        if (w->y.empty())
            return fail(QStringLiteral("vector '%1' is empty").arg(node.name));
        Value v;
        v.x = std::make_shared<const std::vector<double>>(std::move(w->x));
        v.y = std::move(w->y);
        v.isComplex = w->isComplex;
        v.unit = node.unit.isEmpty() ? w->unit : node.unit;
        return v;
    }

    std::optional<Value> binary(char16_t op, const Value &a, const Value &b)
    {
        if (!a.scalar() && !b.scalar() && a.y.size() != b.y.size()) {
            return fail(QStringLiteral("cannot combine traces of different lengths (%1 and %2 points)")
                            .arg(a.y.size())
                            .arg(b.y.size()));
        }
        Value r;
        r.x = a.scalar() ? b.x : a.x;
        const std::size_t n = std::max(a.y.size(), b.y.size());
        r.y.resize(n);
        r.isComplex = a.isComplex || b.isComplex;
        const bool realPower = op == u'^' && !r.isComplex;
        bool becameComplex = false;
        for (std::size_t i = 0; i < n; ++i) {
            const Complex u = a.y[a.scalar() ? 0 : i];
            const Complex v = b.y[b.scalar() ? 0 : i];
            switch (op) {
            case u'+': r.y[i] = u + v; break;
            case u'-': r.y[i] = u - v; break;
            case u'*': r.y[i] = u * v; break;
            case u'/': r.y[i] = u / v; break;
            default:
                if (realPower) {
                    // Real powers stay real (and exact) unless the base is
                    // negative with a fractional exponent.
                    const double p = std::pow(u.real(), v.real());
                    if (std::isnan(p) && !std::isnan(u.real()) && !std::isnan(v.real())) {
                        r.y[i] = std::pow(u, v);
                        becameComplex = true;
                    } else {
                        r.y[i] = p;
                    }
                } else {
                    r.y[i] = std::pow(u, v);
                }
                break;
            }
        }
        r.isComplex = r.isComplex || becameComplex;

        switch (op) {
        case u'+':
        case u'-':
            if (a.unit == b.unit || b.plainNumber())
                r.unit = a.unit;
            else if (a.plainNumber())
                r.unit = b.unit;
            break;
        case u'*':
            if (b.plainNumber())
                r.unit = a.unit;
            else if (a.plainNumber())
                r.unit = b.unit;
            break;
        case u'/':
            if (b.plainNumber())
                r.unit = a.unit;
            break;
        default:
            break;
        }
        return r;
    }

    // Applies an elementwise function. For real input the real version is
    // used where it is defined, so real traces stay real; outside its domain
    // (sqrt or ln of a negative number) the complex result is kept.
    template <typename RealFn, typename ComplexFn>
    static Value elementwise(Value v, RealFn realFn, ComplexFn complexFn)
    {
        if (v.isComplex) {
            for (auto &y : v.y)
                y = complexFn(y);
            return v;
        }
        bool becameComplex = false;
        for (auto &y : v.y) {
            const double r = realFn(y.real());
            if (std::isnan(r) && !std::isnan(y.real())) {
                y = complexFn(y);
                becameComplex = true;
            } else {
                y = r;
            }
        }
        v.isComplex = becameComplex;
        return v;
    }

    template <typename Fn>
    static Value toReal(Value v, Fn fn, const QString &unit)
    {
        for (auto &y : v.y)
            y = fn(y);
        v.isComplex = false;
        v.unit = unit;
        return v;
    }

    // Replaces the values by a constant over the same scale.
    static Value constant(Value v, Complex c, bool isComplex)
    {
        std::fill(v.y.begin(), v.y.end(), c);
        v.isComplex = isComplex;
        return v;
    }

    std::optional<Value> needsScale(const Node &node)
    {
        return fail(QStringLiteral("'%1' needs a trace with a scale, not a constant").arg(node.functionName));
    }

    std::optional<Value> call(const Node &node, Value v)
    {
        const QString unit = v.unit;
        switch (node.function) {
        case Function::Db:
            return toReal(std::move(v), [](Complex y) { return Complex(dB(std::abs(y), 20), 0); }, QStringLiteral("dB"));
        case Function::Db10:
            return toReal(std::move(v), [](Complex y) { return Complex(dB(std::abs(y), 10), 0); }, QStringLiteral("dB"));
        case Function::Mag:
            return toReal(std::move(v), [](Complex y) { return Complex(std::abs(y), 0); }, unit);
        case Function::Phase:
            return toReal(std::move(v), [](Complex y) { return Complex(std::arg(y) * 180 / Pi, 0); },
                          QStringLiteral("deg"));
        case Function::Real:
            return toReal(std::move(v), [](Complex y) { return Complex(y.real(), 0); }, unit);
        case Function::Imag:
            return toReal(std::move(v), [](Complex y) { return Complex(y.imag(), 0); }, unit);
        case Function::Conj:
            for (auto &y : v.y)
                y = std::conj(y);
            return v;
        case Function::Cph: {
            const auto phase = unwrappedPhase(v.y);
            for (std::size_t i = 0; i < v.y.size(); ++i)
                v.y[i] = phase[i] * 180 / Pi;
            v.isComplex = false;
            v.unit = QStringLiteral("deg");
            return v;
        }
        case Function::Sqrt:
            v = elementwise(std::move(v), [](double y) { return std::sqrt(y); }, [](Complex y) { return std::sqrt(y); });
            v.unit.clear();
            return v;
        case Function::Exp:
            return dimensionless(elementwise(std::move(v), [](double y) { return std::exp(y); },
                                             [](Complex y) { return std::exp(y); }));
        case Function::Ln:
            return dimensionless(elementwise(std::move(v), [](double y) { return std::log(y); },
                                             [](Complex y) { return std::log(y); }));
        case Function::Log10:
            return dimensionless(elementwise(std::move(v), [](double y) { return std::log10(y); },
                                             [](Complex y) { return std::log10(y); }));
        case Function::Sin:
            return dimensionless(elementwise(std::move(v), [](double y) { return std::sin(y); },
                                             [](Complex y) { return std::sin(y); }));
        case Function::Cos:
            return dimensionless(elementwise(std::move(v), [](double y) { return std::cos(y); },
                                             [](Complex y) { return std::cos(y); }));
        case Function::Tan:
            return dimensionless(elementwise(std::move(v), [](double y) { return std::tan(y); },
                                             [](Complex y) { return std::tan(y); }));
        case Function::Atan:
            return dimensionless(elementwise(std::move(v), [](double y) { return std::atan(y); },
                                             [](Complex y) { return std::atan(y); }));
        case Function::Sinh:
            return dimensionless(elementwise(std::move(v), [](double y) { return std::sinh(y); },
                                             [](Complex y) { return std::sinh(y); }));
        case Function::Cosh:
            return dimensionless(elementwise(std::move(v), [](double y) { return std::cosh(y); },
                                             [](Complex y) { return std::cosh(y); }));
        case Function::Tanh:
            return dimensionless(elementwise(std::move(v), [](double y) { return std::tanh(y); },
                                             [](Complex y) { return std::tanh(y); }));
        case Function::Deriv:
            if (v.scalar())
                return scalarValue(Complex(0, 0), false);
            v.y = derivative(*v.x, v.y);
            v.unit.clear();
            return v;
        case Function::Integ: {
            if (v.scalar())
                return needsScale(node);
            const auto &x = *v.x;
            Complex sum(0, 0);
            Complex previous = v.y[0];
            v.y[0] = 0;
            for (std::size_t i = 1; i < v.y.size(); ++i) {
                const Complex current = v.y[i];
                sum += 0.5 * (x[i] - x[i - 1]) * (current + previous);
                previous = current;
                v.y[i] = sum;
            }
            v.unit.clear();
            return v;
        }
        case Function::GroupDelay: {
            if (v.scalar())
                return needsScale(node);
            const auto phase = unwrappedPhase(v.y);
            const auto slope = derivative(*v.x, std::vector<Complex>(phase.begin(), phase.end()));
            for (std::size_t i = 0; i < v.y.size(); ++i)
                v.y[i] = -slope[i].real() / (2 * Pi);
            v.isComplex = false;
            v.unit = QStringLiteral("s");
            return v;
        }
        case Function::Mean:
        case Function::Rms:
        case Function::Max:
        case Function::Min:
        case Function::PeakToPeak:
            return statistic(node.function, std::move(v));
        }
        return std::nullopt;
    }

    static Value dimensionless(Value v)
    {
        v.unit.clear();
        return v;
    }

    // mean, rms, max, min and pkpk: one number, spread over the whole scale.
    // Averages are weighted by the scale with the trace taken as piecewise
    // linear, as in WaveformMath; extremes use the magnitude of complex traces.
    static std::optional<Value> statistic(Function function, Value v)
    {
        const auto magnitude = [&v](std::size_t i) { return v.isComplex ? std::abs(v.y[i]) : v.y[i].real(); };
        const std::size_t n = v.y.size();
        const double width = v.scalar() ? 0.0 : v.x->back() - v.x->front();
        switch (function) {
        case Function::Mean: {
            Complex sum(0, 0);
            if (width > 0) {
                for (std::size_t i = 1; i < n; ++i)
                    sum += 0.5 * ((*v.x)[i] - (*v.x)[i - 1]) * (v.y[i] + v.y[i - 1]);
                sum /= width;
            } else {
                for (const auto &y : v.y)
                    sum += y;
                sum /= double(n);
            }
            const bool isComplex = v.isComplex;
            return constant(std::move(v), sum, isComplex);
        }
        case Function::Rms: {
            double sum = 0;
            if (width > 0) {
                for (std::size_t i = 1; i < n; ++i) {
                    const Complex a = v.y[i - 1];
                    const Complex b = v.y[i];
                    sum += ((*v.x)[i] - (*v.x)[i - 1]) * (std::norm(a) + (a * std::conj(b)).real() + std::norm(b)) / 3.0;
                }
                sum /= width;
            } else {
                for (const auto &y : v.y)
                    sum += std::norm(y);
                sum /= double(n);
            }
            return constant(std::move(v), Complex(std::sqrt(sum), 0), false);
        }
        default: {
            double hi = magnitude(0);
            double lo = hi;
            for (std::size_t i = 1; i < n; ++i) {
                hi = std::max(hi, magnitude(i));
                lo = std::min(lo, magnitude(i));
            }
            const double result = function == Function::Max ? hi : (function == Function::Min ? lo : hi - lo);
            return constant(std::move(v), Complex(result, 0), false);
        }
        }
    }

    const VectorExpression::Resolver &m_resolve;
};

void collectVectors(const Node &node, QStringList &names)
{
    if (node.kind == Node::Kind::Vector && !names.contains(node.name))
        names << node.name;
    for (const auto &arg : node.args)
        collectVectors(*arg, names);
}

} // namespace

std::optional<VectorExpression> VectorExpression::parse(const QString &text, QString *error)
{
    Parser parser(text);
    auto root = parser.run(error);
    if (!root)
        return std::nullopt;
    VectorExpression expression;
    expression.m_text = text;
    expression.m_root = std::move(root);
    return expression;
}

std::optional<Waveform> VectorExpression::evaluate(const Resolver &resolve, QString *error) const
{
    if (!m_root) {
        if (error)
            *error = QStringLiteral("empty expression");
        return std::nullopt;
    }
    Evaluator evaluator(resolve);
    auto value = evaluator.eval(*m_root);
    if (!value) {
        if (error)
            *error = evaluator.error;
        return std::nullopt;
    }
    Waveform w;
    if (value->x)
        w.x = *value->x;
    w.y = std::move(value->y);
    w.isComplex = value->isComplex;
    w.unit = value->unit;
    return w;
}

QStringList VectorExpression::referencedVectors() const
{
    QStringList names;
    if (m_root)
        collectVectors(*m_root, names);
    return names;
}

} // namespace lvd::spice

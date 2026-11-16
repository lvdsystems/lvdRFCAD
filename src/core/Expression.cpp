#include "core/Expression.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace lvd {

struct Expression::Node {
    enum class Kind { Number, Variable, Negate, Binary, Call };

    Kind kind = Kind::Number;
    double value = 0.0;
    QString name; // variable or function name
    char16_t op = 0; // binary operator
    int function = -1; // index into functionTable()
    std::vector<std::shared_ptr<const Node>> args;
};

namespace {

using NodePtr = std::shared_ptr<const Expression::Node>;
using Node = Expression::Node;

constexpr char16_t MicroSign = 0x00B5;
constexpr char16_t GreekMu = 0x03BC;
constexpr char16_t GreekOmega = 0x03A9;
constexpr char16_t OhmSign = 0x2126;

// ---------------------------------------------------------------------------
// Names

enum class Function {
    Sqrt, Exp, Ln, Log10, Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Sinh, Cosh, Tanh,
    Abs, Min, Max, Pow, Floor, Ceil, Round, Db10, Db20, Undb10, Undb20
};

struct FunctionInfo {
    const char *name;
    Function id;
    int arity; // -1: one or more
};

const std::vector<FunctionInfo> &functionTable()
{
    static const std::vector<FunctionInfo> table = {
        {"sqrt", Function::Sqrt, 1},   {"exp", Function::Exp, 1},       {"ln", Function::Ln, 1},
        {"log10", Function::Log10, 1}, {"sin", Function::Sin, 1},       {"cos", Function::Cos, 1},
        {"tan", Function::Tan, 1},     {"asin", Function::Asin, 1},     {"acos", Function::Acos, 1},
        {"atan", Function::Atan, 1},   {"atan2", Function::Atan2, 2},   {"sinh", Function::Sinh, 1},
        {"cosh", Function::Cosh, 1},   {"tanh", Function::Tanh, 1},     {"abs", Function::Abs, 1},
        {"min", Function::Min, -1},    {"max", Function::Max, -1},      {"pow", Function::Pow, 2},
        {"floor", Function::Floor, 1}, {"ceil", Function::Ceil, 1},     {"round", Function::Round, 1},
        {"db10", Function::Db10, 1},   {"db20", Function::Db20, 1},     {"undb10", Function::Undb10, 1},
        {"undb20", Function::Undb20, 1},
    };
    return table;
}

int findFunction(const QString &name)
{
    const auto &table = functionTable();
    for (size_t i = 0; i < table.size(); ++i) {
        if (name == QLatin1StringView(table[i].name))
            return int(i);
    }
    return -1;
}

std::optional<double> findConstant(const QString &name)
{
    if (name == u"pi")
        return 3.14159265358979323846;
    if (name == u"c0")
        return 299792458.0;
    if (name == u"kB")
        return 1.380649e-23;
    if (name == u"T0")
        return 290.0;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Characters

bool isDigit(QChar c)
{
    return c.unicode() >= u'0' && c.unicode() <= u'9';
}

bool isAsciiLetter(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
}

bool isIdentifierStart(QChar c)
{
    return isAsciiLetter(c) || c.unicode() == u'_';
}

bool isIdentifierChar(QChar c)
{
    return isIdentifierStart(c) || isDigit(c);
}

bool isUnitSymbol(QChar c)
{
    const char16_t u = c.unicode();
    return u == MicroSign || u == GreekMu || u == GreekOmega || u == OhmSign;
}

bool isUnitStart(QChar c)
{
    return isAsciiLetter(c) || isUnitSymbol(c);
}

bool isUnitChar(QChar c)
{
    return isIdentifierChar(c) || isUnitSymbol(c);
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

bool startsWithMeg(QStringView text)
{
    return text.size() >= 3 && text.first(3).compare(u"meg", Qt::CaseInsensitive) == 0;
}

bool isKnownUnit(QStringView word)
{
    static const QStringList units = {
        QStringLiteral("Hz"),  QStringLiteral("F"),   QStringLiteral("H"),   QStringLiteral("ohm"),
        QStringLiteral("Ohm"), QString(QChar(GreekOmega)), QString(QChar(OhmSign)), QStringLiteral("V"),
        QStringLiteral("A"),   QStringLiteral("W"),   QStringLiteral("S"),   QStringLiteral("s"),
        QStringLiteral("m"),   QStringLiteral("dB"),  QStringLiteral("dBm"), QStringLiteral("dBW"),
        QStringLiteral("dBc"), QStringLiteral("dBi"), QStringLiteral("deg"), QStringLiteral("rad"),
        QStringLiteral("K"),
    };
    for (const QString &unit : units) {
        if (word == unit)
            return true;
    }
    return false;
}

// Multiplier for a word written after a number and a space, or nullopt if the
// word is not a (prefixed) unit. The whole word as a unit takes precedence, so
// "m" is metres and "dB" is decibels.
std::optional<double> spacedUnitMultiplier(QStringView word)
{
    if (isKnownUnit(word))
        return 1.0;
    if (startsWithMeg(word) && word.size() > 3 && isKnownUnit(word.sliced(3)))
        return 1e6;
    if (word.size() > 1 && isKnownUnit(word.sliced(1))) {
        if (const auto prefix = prefixValue(word.front()))
            return prefix;
    }
    return std::nullopt;
}

QString position(qsizetype index)
{
    return QString::number(index + 1);
}

// ---------------------------------------------------------------------------
// Numbers

struct NumberScan {
    std::optional<double> value;
    qsizetype end = 0;
    QString error;
};

double toDouble(const QString &digits)
{
    const std::string text = digits.toStdString();
    return std::strtod(text.c_str(), nullptr);
}

// Scans a number starting at `start`, which must be a digit or a '.'.
NumberScan scanNumber(const QString &text, qsizetype start)
{
    NumberScan scan;
    const qsizetype n = text.size();
    qsizetype i = start;
    QString mantissa;
    bool hasDigits = false;
    bool hasPointOrExponent = false;

    while (i < n && isDigit(text[i])) {
        mantissa += text[i++];
        hasDigits = true;
    }
    if (i < n && text[i] == u'.') {
        hasPointOrExponent = true;
        mantissa += text[i++];
        while (i < n && isDigit(text[i])) {
            mantissa += text[i++];
            hasDigits = true;
        }
    }
    if (!hasDigits) {
        scan.error = QStringLiteral("invalid number at position %1").arg(position(start));
        return scan;
    }
    if (i < n && (text[i] == u'e' || text[i] == u'E')) {
        qsizetype j = i + 1;
        if (j < n && (text[j] == u'+' || text[j] == u'-'))
            ++j;
        if (j < n && isDigit(text[j])) {
            hasPointOrExponent = true;
            mantissa += text.mid(i, j - i);
            i = j;
            while (i < n && isDigit(text[i]))
                mantissa += text[i++];
        }
    }

    double value = toDouble(mantissa);
    double multiplier = 1.0;
    bool hasPrefix = false;
    if (startsWithMeg(QStringView(text).sliced(i))) {
        multiplier = 1e6;
        i += 3;
        hasPrefix = true;
    } else if (i < n) {
        if (const auto prefix = prefixValue(text[i])) {
            multiplier = *prefix;
            ++i;
            hasPrefix = true;
        }
    }

    bool hasSuffix = hasPrefix;
    if (hasPrefix && i < n && isDigit(text[i])) {
        // European notation: the prefix stands for the decimal point.
        if (hasPointOrExponent) {
            scan.error = QStringLiteral("invalid number '%1' at position %2")
                             .arg(text.mid(start, i + 1 - start), position(start));
            return scan;
        }
        QString fraction;
        while (i < n && isDigit(text[i]))
            fraction += text[i++];
        if (i < n && isUnitChar(text[i])) {
            scan.error = QStringLiteral("unexpected '%1' after number '%2' at position %3")
                             .arg(text[i])
                             .arg(text.mid(start, i - start), position(i));
            return scan;
        }
        value = toDouble(mantissa + QLatin1Char('.') + fraction);
    } else {
        while (i < n && isUnitChar(text[i])) {
            ++i;
            hasSuffix = true;
        }
    }

    if (!hasSuffix) {
        qsizetype j = i;
        while (j < n && text[j].isSpace())
            ++j;
        if (j > i && j < n && isUnitStart(text[j])) {
            qsizetype k = j;
            while (k < n && isUnitChar(text[k]))
                ++k;
            if (const auto unitMultiplier = spacedUnitMultiplier(QStringView(text).sliced(j, k - j))) {
                multiplier = *unitMultiplier;
                i = k;
            }
        }
    }

    const double result = value * multiplier;
    if (!std::isfinite(result)) {
        scan.error = QStringLiteral("number '%1' at position %2 is out of range")
                         .arg(text.mid(start, i - start), position(start));
        return scan;
    }
    scan.value = result;
    scan.end = i;
    return scan;
}

// ---------------------------------------------------------------------------
// Lexer

struct Token {
    enum class Type { Number, Identifier, Operator, LParen, RParen, Comma, End };

    Type type = Type::End;
    qsizetype pos = 0;
    QString text;
    double value = 0.0;
};

bool tokenize(const QString &text, std::vector<Token> &tokens, QString &error)
{
    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = text[i];
        if (c.isSpace()) {
            ++i;
            continue;
        }
        Token token;
        token.pos = i;
        if (isDigit(c) || (c == u'.' && i + 1 < n && isDigit(text[i + 1]))) {
            const NumberScan scan = scanNumber(text, i);
            if (!scan.value) {
                error = scan.error;
                return false;
            }
            token.type = Token::Type::Number;
            token.value = *scan.value;
            token.text = text.mid(i, scan.end - i);
            i = scan.end;
        } else if (isIdentifierStart(c)) {
            qsizetype j = i;
            while (j < n && isIdentifierChar(text[j]))
                ++j;
            token.type = Token::Type::Identifier;
            token.text = text.mid(i, j - i);
            i = j;
        } else {
            switch (c.unicode()) {
            case u'+':
            case u'-':
            case u'*':
            case u'/':
            case u'^': token.type = Token::Type::Operator; break;
            case u'(': token.type = Token::Type::LParen; break;
            case u')': token.type = Token::Type::RParen; break;
            case u',': token.type = Token::Type::Comma; break;
            default:
                error = QStringLiteral("unexpected character '%1' at position %2").arg(c).arg(position(i));
                return false;
            }
            token.text = QString(c);
            ++i;
        }
        tokens.push_back(token);
    }
    Token end;
    end.pos = n;
    tokens.push_back(end);
    return true;
}

// ---------------------------------------------------------------------------
// Parser

class Parser
{
public:
    explicit Parser(std::vector<Token> tokens) : m_tokens(std::move(tokens)) {}

    NodePtr parse()
    {
        if (peek().type == Token::Type::End) {
            fail(QStringLiteral("empty expression"));
            return nullptr;
        }
        NodePtr root = parseSum();
        if (root && peek().type != Token::Type::End) {
            unexpected(peek());
            return nullptr;
        }
        return root;
    }

    QString error() const { return m_error; }

private:
    const Token &peek() const { return m_tokens[m_index]; }
    const Token &next()
    {
        const Token &token = m_tokens[m_index];
        if (token.type != Token::Type::End)
            ++m_index;
        return token;
    }

    bool isOperator(char16_t op) const
    {
        return peek().type == Token::Type::Operator && peek().text.front().unicode() == op;
    }

    void fail(const QString &message)
    {
        if (m_error.isEmpty())
            m_error = message;
    }

    void unexpected(const Token &token)
    {
        if (token.type == Token::Type::End)
            fail(QStringLiteral("unexpected end of expression"));
        else
            fail(QStringLiteral("unexpected '%1' at position %2").arg(token.text, position(token.pos)));
    }

    static NodePtr makeBinary(char16_t op, NodePtr left, NodePtr right)
    {
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Binary;
        node->op = op;
        node->args = {std::move(left), std::move(right)};
        return node;
    }

    NodePtr parseSum()
    {
        NodePtr left = parseProduct();
        while (left && (isOperator(u'+') || isOperator(u'-'))) {
            const char16_t op = next().text.front().unicode();
            NodePtr right = parseProduct();
            if (!right)
                return nullptr;
            left = makeBinary(op, std::move(left), std::move(right));
        }
        return left;
    }

    NodePtr parseProduct()
    {
        NodePtr left = parseUnary();
        while (left && (isOperator(u'*') || isOperator(u'/'))) {
            const char16_t op = next().text.front().unicode();
            NodePtr right = parseUnary();
            if (!right)
                return nullptr;
            left = makeBinary(op, std::move(left), std::move(right));
        }
        return left;
    }

    NodePtr parseUnary()
    {
        if (isOperator(u'+')) {
            next();
            return parseUnary();
        }
        if (isOperator(u'-')) {
            next();
            NodePtr operand = parseUnary();
            if (!operand)
                return nullptr;
            auto node = std::make_shared<Node>();
            node->kind = Node::Kind::Negate;
            node->args = {std::move(operand)};
            return node;
        }
        return parsePower();
    }

    // The exponent is a unary expression, which makes ^ right-associative and
    // allows 2^-1, while -2^2 is -(2^2).
    NodePtr parsePower()
    {
        NodePtr base = parsePrimary();
        if (base && isOperator(u'^')) {
            next();
            NodePtr exponent = parseUnary();
            if (!exponent)
                return nullptr;
            return makeBinary(u'^', std::move(base), std::move(exponent));
        }
        return base;
    }

    NodePtr parsePrimary()
    {
        const Token &token = next();
        switch (token.type) {
        case Token::Type::Number: {
            auto node = std::make_shared<Node>();
            node->value = token.value;
            return node;
        }
        case Token::Type::Identifier:
            if (peek().type == Token::Type::LParen)
                return parseCall(token);
            return parseName(token);
        case Token::Type::LParen: {
            NodePtr inner = parseSum();
            if (!inner)
                return nullptr;
            if (!expectClose(token))
                return nullptr;
            return inner;
        }
        default:
            unexpected(token);
            return nullptr;
        }
    }

    bool expectClose(const Token &open)
    {
        if (peek().type == Token::Type::RParen) {
            next();
            return true;
        }
        if (peek().type == Token::Type::End)
            fail(QStringLiteral("missing ')' for '(' at position %1").arg(position(open.pos)));
        else
            fail(QStringLiteral("expected ')' at position %1, found '%2'").arg(position(peek().pos), peek().text));
        return false;
    }

    NodePtr parseName(const Token &token)
    {
        if (const auto constant = findConstant(token.text)) {
            auto node = std::make_shared<Node>();
            node->value = *constant;
            return node;
        }
        if (findFunction(token.text) >= 0) {
            fail(QStringLiteral("function '%1' at position %2 needs arguments in parentheses")
                     .arg(token.text, position(token.pos)));
            return nullptr;
        }
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Variable;
        node->name = token.text;
        return node;
    }

    NodePtr parseCall(const Token &nameToken)
    {
        const int function = findFunction(nameToken.text);
        if (function < 0) {
            if (findConstant(nameToken.text))
                fail(QStringLiteral("'%1' is a constant, not a function").arg(nameToken.text));
            else
                fail(QStringLiteral("unknown function '%1' at position %2").arg(nameToken.text, position(nameToken.pos)));
            return nullptr;
        }
        const Token &open = next();
        std::vector<NodePtr> args;
        if (peek().type != Token::Type::RParen) {
            for (;;) {
                NodePtr arg = parseSum();
                if (!arg)
                    return nullptr;
                args.push_back(std::move(arg));
                if (peek().type != Token::Type::Comma)
                    break;
                next();
            }
        }
        if (!expectClose(open))
            return nullptr;

        const FunctionInfo &info = functionTable()[size_t(function)];
        const int count = int(args.size());
        if (info.arity < 0 && count < 1) {
            fail(QStringLiteral("function '%1' needs at least 1 argument").arg(nameToken.text));
            return nullptr;
        }
        if (info.arity >= 0 && count != info.arity) {
            fail(QStringLiteral("function '%1' takes %2 argument%3, got %4")
                     .arg(nameToken.text)
                     .arg(info.arity)
                     .arg(info.arity == 1 ? QString() : QStringLiteral("s"))
                     .arg(count));
            return nullptr;
        }
        auto node = std::make_shared<Node>();
        node->kind = Node::Kind::Call;
        node->name = nameToken.text;
        node->function = function;
        node->args = std::move(args);
        return node;
    }

    std::vector<Token> m_tokens;
    size_t m_index = 0;
    QString m_error;
};

// ---------------------------------------------------------------------------
// Evaluation

std::optional<double> fail(QString &error, const QString &message)
{
    error = message;
    return std::nullopt;
}

std::optional<double> finite(double value, QString &error, const QString &what)
{
    if (!std::isfinite(value))
        return fail(error, QStringLiteral("result of %1 is not a finite number").arg(what));
    return value;
}

std::optional<double> callFunction(const Node &node, const std::vector<double> &a, QString &error)
{
    const FunctionInfo &info = functionTable()[size_t(node.function)];
    const QString what = QStringLiteral("'%1'").arg(node.name);
    const auto requirePositive = [&]() -> bool {
        if (a[0] <= 0.0) {
            error = QStringLiteral("%1 of a non-positive number").arg(node.name);
            return false;
        }
        return true;
    };
    double result = 0.0;
    switch (info.id) {
    case Function::Sqrt:
        if (a[0] < 0.0)
            return fail(error, QStringLiteral("sqrt of a negative number"));
        result = std::sqrt(a[0]);
        break;
    case Function::Exp: result = std::exp(a[0]); break;
    case Function::Ln:
        if (!requirePositive())
            return std::nullopt;
        result = std::log(a[0]);
        break;
    case Function::Log10:
        if (!requirePositive())
            return std::nullopt;
        result = std::log10(a[0]);
        break;
    case Function::Sin: result = std::sin(a[0]); break;
    case Function::Cos: result = std::cos(a[0]); break;
    case Function::Tan: result = std::tan(a[0]); break;
    case Function::Asin:
    case Function::Acos:
        if (a[0] < -1.0 || a[0] > 1.0)
            return fail(error, QStringLiteral("%1 argument outside [-1, 1]").arg(node.name));
        result = info.id == Function::Asin ? std::asin(a[0]) : std::acos(a[0]);
        break;
    case Function::Atan: result = std::atan(a[0]); break;
    case Function::Atan2: result = std::atan2(a[0], a[1]); break;
    case Function::Sinh: result = std::sinh(a[0]); break;
    case Function::Cosh: result = std::cosh(a[0]); break;
    case Function::Tanh: result = std::tanh(a[0]); break;
    case Function::Abs: result = std::fabs(a[0]); break;
    case Function::Min: result = *std::min_element(a.begin(), a.end()); break;
    case Function::Max: result = *std::max_element(a.begin(), a.end()); break;
    case Function::Pow: result = std::pow(a[0], a[1]); break;
    case Function::Floor: result = std::floor(a[0]); break;
    case Function::Ceil: result = std::ceil(a[0]); break;
    case Function::Round: result = std::round(a[0]); break;
    case Function::Db10:
        if (!requirePositive())
            return std::nullopt;
        result = 10.0 * std::log10(a[0]);
        break;
    case Function::Db20:
        if (!requirePositive())
            return std::nullopt;
        result = 20.0 * std::log10(a[0]);
        break;
    case Function::Undb10: result = std::pow(10.0, a[0] / 10.0); break;
    case Function::Undb20: result = std::pow(10.0, a[0] / 20.0); break;
    }
    return finite(result, error, what);
}

std::optional<double> evaluateNode(const Node &node, const Expression::Lookup &lookup, QString &error)
{
    switch (node.kind) {
    case Node::Kind::Number:
        return node.value;
    case Node::Kind::Variable: {
        const std::optional<double> value = lookup ? lookup(node.name) : std::nullopt;
        if (!value)
            return fail(error, QStringLiteral("undefined variable '%1'").arg(node.name));
        if (!std::isfinite(*value))
            return fail(error, QStringLiteral("variable '%1' is not a finite number").arg(node.name));
        return value;
    }
    case Node::Kind::Negate: {
        const auto operand = evaluateNode(*node.args[0], lookup, error);
        if (!operand)
            return std::nullopt;
        return -*operand;
    }
    case Node::Kind::Binary: {
        const auto left = evaluateNode(*node.args[0], lookup, error);
        if (!left)
            return std::nullopt;
        const auto right = evaluateNode(*node.args[1], lookup, error);
        if (!right)
            return std::nullopt;
        double result = 0.0;
        switch (node.op) {
        case u'+': result = *left + *right; break;
        case u'-': result = *left - *right; break;
        case u'*': result = *left * *right; break;
        case u'/':
            if (*right == 0.0)
                return fail(error, QStringLiteral("division by zero"));
            result = *left / *right;
            break;
        default: result = std::pow(*left, *right); break;
        }
        return finite(result, error, QStringLiteral("'%1'").arg(QChar(node.op)));
    }
    case Node::Kind::Call: {
        std::vector<double> args;
        args.reserve(node.args.size());
        for (const NodePtr &arg : node.args) {
            const auto value = evaluateNode(*arg, lookup, error);
            if (!value)
                return std::nullopt;
            args.push_back(*value);
        }
        return callFunction(node, args, error);
    }
    }
    return fail(error, QStringLiteral("internal error"));
}

void collectVariables(const Node &node, QStringList &names)
{
    if (node.kind == Node::Kind::Variable && !names.contains(node.name))
        names.append(node.name);
    for (const NodePtr &arg : node.args)
        collectVariables(*arg, names);
}

// ---------------------------------------------------------------------------
// Formatting

QString stripTrailingZeros(QString text)
{
    if (!text.contains(QLatin1Char('.')))
        return text;
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

int decimalExponent(double value)
{
    return int(std::floor(std::log10(std::fabs(value))));
}

QString formatFixed(double value, int digits)
{
    if (value == 0.0)
        return QStringLiteral("0");
    const int decimals = digits - 1 - decimalExponent(value);
    if (decimals < 0) {
        // Round to the significant digits left of the decimal point.
        const double scale = std::pow(10.0, double(-decimals));
        return QString::number(std::round(value / scale) * scale, 'f', 0);
    }
    QString text = stripTrailingZeros(QString::number(value, 'f', decimals));
    return text == u"-0" ? QStringLiteral("0") : text;
}

QString formatScientific(double value, int digits)
{
    const QString text = QString::number(value, 'e', digits - 1);
    const qsizetype e = text.indexOf(QLatin1Char('e'));
    if (e < 0)
        return text;
    return stripTrailingZeros(text.left(e)) + QLatin1Char('e') + QString::number(text.mid(e + 1).toInt());
}

double powerOfTen(int exponent)
{
    return std::pow(10.0, double(exponent));
}

} // namespace

// ---------------------------------------------------------------------------

std::optional<double> parseNumber(const QString &text)
{
    const QString trimmed = text.trimmed();
    qsizetype start = 0;
    double sign = 1.0;
    if (!trimmed.isEmpty() && (trimmed.front() == u'+' || trimmed.front() == u'-')) {
        sign = trimmed.front() == u'-' ? -1.0 : 1.0;
        start = 1;
    }
    if (start >= trimmed.size())
        return std::nullopt;
    const QChar first = trimmed[start];
    if (!isDigit(first) && first != u'.')
        return std::nullopt;
    const NumberScan scan = scanNumber(trimmed, start);
    if (!scan.value || scan.end != trimmed.size())
        return std::nullopt;
    return sign * *scan.value;
}

QString formatEngineering(double value, const QString &unit, int significantDigits)
{
    const int digits = std::max(1, significantDigits);
    const auto join = [&unit](const QString &number, const QString &prefix) {
        if (unit.isEmpty())
            return number + prefix;
        return number + QLatin1Char(' ') + prefix + unit;
    };

    if (!std::isfinite(value))
        return join(QString::number(value), QString());

    static const QStringList plainUnits = {
        QStringLiteral("dB"),  QStringLiteral("dBm"), QStringLiteral("dBW"), QStringLiteral("dBc"),
        QStringLiteral("dBi"), QStringLiteral("deg"), QStringLiteral("%"),
    };
    if (plainUnits.contains(unit)) {
        if (value != 0.0) {
            const int exponent = decimalExponent(value);
            if (exponent > 15 || exponent < -6)
                return join(formatScientific(value, digits), QString());
        }
        return join(formatFixed(value, digits), QString());
    }

    if (value == 0.0)
        return join(QStringLiteral("0"), QString());

    const int exponent = decimalExponent(value);
    int exponent3 = exponent >= 0 ? exponent / 3 * 3 : -((-exponent + 2) / 3) * 3;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (exponent3 > 12 || exponent3 < -15)
            break;
        const double mantissa = exponent3 >= 0 ? value / powerOfTen(exponent3) : value * powerOfTen(-exponent3);
        const QString number = formatFixed(mantissa, digits);
        if (std::fabs(number.toDouble()) >= 1000.0) {
            exponent3 += 3; // rounding carried into the next prefix
            continue;
        }
        static const char16_t *const prefixes[] = {u"f", u"p", u"n", u"µ", u"m", u"", u"k", u"M", u"G", u"T"};
        return join(number, QString::fromUtf16(prefixes[(exponent3 + 15) / 3]));
    }
    return join(formatScientific(value, digits), QString());
}

// ---------------------------------------------------------------------------

Expression::Expression() = default;

Expression Expression::parse(const QString &text)
{
    Expression expression;
    expression.m_text = text;
    std::vector<Token> tokens;
    QString error;
    if (!tokenize(text, tokens, error)) {
        expression.m_error = error;
        return expression;
    }
    Parser parser(std::move(tokens));
    expression.m_root = parser.parse();
    if (!expression.m_root)
        expression.m_error = parser.error();
    return expression;
}

QStringList Expression::variables() const
{
    QStringList names;
    if (m_root)
        collectVariables(*m_root, names);
    return names;
}

std::optional<double> Expression::evaluate(const Lookup &lookup, QString *error) const
{
    QString message;
    std::optional<double> result;
    if (!m_root)
        message = m_error.isEmpty() ? QStringLiteral("empty expression") : m_error;
    else
        result = evaluateNode(*m_root, lookup, message);
    if (!result && error)
        *error = message;
    return result;
}

bool isReservedName(const QString &name)
{
    return findFunction(name) >= 0 || findConstant(name).has_value();
}

bool isValidVariableName(const QString &name)
{
    if (name.isEmpty() || !isIdentifierStart(name.front()))
        return false;
    for (const QChar c : name) {
        if (!isIdentifierChar(c))
            return false;
    }
    return !isReservedName(name);
}

// ---------------------------------------------------------------------------

namespace {

// Strongly connected components (Tarjan) of the local dependency graph.
class Components
{
public:
    explicit Components(const std::vector<std::vector<int>> &edges)
        : m_edges(edges), m_index(edges.size(), -1), m_low(edges.size(), 0), m_onStack(edges.size(), false),
          m_component(edges.size(), -1)
    {
        for (int v = 0; v < int(edges.size()); ++v) {
            if (m_index[size_t(v)] < 0)
                visit(v);
        }
    }

    int component(int v) const { return m_component[size_t(v)]; }

private:
    void visit(int v)
    {
        const size_t sv = size_t(v);
        m_index[sv] = m_low[sv] = m_counter++;
        m_stack.push_back(v);
        m_onStack[sv] = true;
        for (const int w : m_edges[sv]) {
            const size_t sw = size_t(w);
            if (m_index[sw] < 0) {
                visit(w);
                m_low[sv] = std::min(m_low[sv], m_low[sw]);
            } else if (m_onStack[sw]) {
                m_low[sv] = std::min(m_low[sv], m_index[sw]);
            }
        }
        if (m_low[sv] == m_index[sv]) {
            int w = -1;
            do {
                w = m_stack.back();
                m_stack.pop_back();
                m_onStack[size_t(w)] = false;
                m_component[size_t(w)] = m_componentCount;
            } while (w != v);
            ++m_componentCount;
        }
    }

    const std::vector<std::vector<int>> &m_edges;
    std::vector<int> m_index;
    std::vector<int> m_low;
    std::vector<bool> m_onStack;
    std::vector<int> m_component;
    std::vector<int> m_stack;
    int m_counter = 0;
    int m_componentCount = 0;
};

// Shortest path from v back to itself inside v's component.
QList<int> cycleThrough(int v, const std::vector<std::vector<int>> &edges, const Components &components)
{
    const int component = components.component(v);
    std::vector<int> parent(edges.size(), -1);
    std::vector<bool> seen(edges.size(), false);
    QList<int> queue{v};
    for (qsizetype head = 0; head < queue.size(); ++head) {
        const int u = queue[head];
        for (const int w : edges[size_t(u)]) {
            if (components.component(w) != component)
                continue;
            if (w == v) {
                QList<int> path{v};
                for (int x = u; x != v; x = parent[size_t(x)])
                    path.prepend(x);
                path.prepend(v);
                return path;
            }
            if (!seen[size_t(w)]) {
                seen[size_t(w)] = true;
                parent[size_t(w)] = u;
                queue.append(w);
            }
        }
    }
    return {v, v};
}

} // namespace

VariableResults evaluateVariables(const QList<QPair<QString, QString>> &definitions, const Expression::Lookup &outer)
{
    VariableResults results;

    // Local variables: the first definition with a valid name.
    QList<QString> names;
    std::vector<Expression> expressions;
    QHash<QString, int> local;
    QStringList duplicates;
    for (const auto &definition : definitions) {
        const QString &name = definition.first;
        if (!isValidVariableName(name)) {
            QString message;
            if (name.isEmpty())
                message = QStringLiteral("empty variable name");
            else if (isReservedName(name))
                message = QStringLiteral("'%1' is a reserved name (constant or function)").arg(name);
            else
                message = QStringLiteral("'%1' is not a valid variable name").arg(name);
            results.errors.insert(name, message);
            continue;
        }
        if (local.contains(name)) {
            duplicates.append(name);
            continue;
        }
        local.insert(name, int(names.size()));
        names.append(name);
        expressions.push_back(Expression::parse(definition.second));
    }

    const int count = int(names.size());
    std::vector<std::vector<int>> edges(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        for (const QString &reference : expressions[size_t(i)].variables()) {
            const auto it = local.constFind(reference);
            if (it != local.constEnd())
                edges[size_t(i)].push_back(it.value());
        }
    }

    enum class State { Pending, Done, Failed };
    std::vector<State> state(static_cast<size_t>(count), State::Pending);

    const Components components(edges);
    std::vector<int> componentSize(static_cast<size_t>(count), 0);
    for (int i = 0; i < count; ++i)
        ++componentSize[size_t(components.component(i))];
    for (int i = 0; i < count; ++i) {
        const auto &out = edges[size_t(i)];
        const bool selfLoop = std::find(out.begin(), out.end(), i) != out.end();
        if (componentSize[size_t(components.component(i))] < 2 && !selfLoop)
            continue;
        QStringList path;
        for (const int v : cycleThrough(i, edges, components))
            path.append(names[v]);
        results.errors.insert(names[i], QStringLiteral("circular definition: %1").arg(path.join(QStringLiteral(" -> "))));
        state[size_t(i)] = State::Failed;
    }

    const Expression::Lookup lookup = [&](const QString &name) -> std::optional<double> {
        const auto it = local.constFind(name);
        if (it != local.constEnd()) {
            const auto value = results.values.constFind(name);
            if (value != results.values.constEnd())
                return value.value();
            return std::nullopt;
        }
        return outer ? outer(name) : std::nullopt;
    };

    // Outside cycles the graph is acyclic, so this recursion terminates.
    std::function<void(int)> evaluate = [&](int i) {
        if (state[size_t(i)] != State::Pending)
            return;
        const QString &name = names[i];
        const Expression &expression = expressions[size_t(i)];
        if (!expression.isValid()) {
            results.errors.insert(name, expression.error());
            state[size_t(i)] = State::Failed;
            return;
        }
        for (const int dependency : edges[size_t(i)]) {
            evaluate(dependency);
            if (state[size_t(dependency)] == State::Failed) {
                results.errors.insert(name, QStringLiteral("depends on '%1', which has an error").arg(names[dependency]));
                state[size_t(i)] = State::Failed;
                return;
            }
        }
        QString error;
        if (const auto value = expression.evaluate(lookup, &error)) {
            results.values.insert(name, *value);
            state[size_t(i)] = State::Done;
        } else {
            results.errors.insert(name, error);
            state[size_t(i)] = State::Failed;
        }
    };
    for (int i = 0; i < count; ++i)
        evaluate(i);

    // Later definitions of a name are errors; the first one keeps its value.
    for (const QString &name : std::as_const(duplicates)) {
        if (!results.errors.contains(name))
            results.errors.insert(name, QStringLiteral("'%1' is defined more than once").arg(name));
    }
    return results;
}

} // namespace lvd

#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace lvd {

// Value and expression syntax: see docs/file-format.md, section 3.2.

// Parses a single number with optional SI prefix and unit, such as "2.4GHz",
// "4n7" or "10 dBm". Surrounding whitespace is allowed. Returns nullopt if the
// text is not exactly one number.
std::optional<double> parseNumber(const QString &text);

// Formats a value with an engineering prefix: (2.4e9, "Hz") gives "2.4 GHz",
// (4.7e-9, "F") gives "4.7 nF". Units in dB, dBm, dBW, dBc, dBi, deg and %
// are never given a prefix. significantDigits is at least 1.
QString formatEngineering(double value, const QString &unit = QString(), int significantDigits = 4);

class Expression
{
public:
    // Looks up a variable; returns nullopt if it is not defined.
    using Lookup = std::function<std::optional<double>(const QString &name)>;

    Expression(); // empty and invalid

    // Never throws. A syntax error gives an invalid expression with an error message.
    static Expression parse(const QString &text);

    QString text() const { return m_text; }
    bool isValid() const { return m_root != nullptr; }
    QString error() const { return m_error; }

    // Variable names the expression refers to, without duplicates, in order of
    // first appearance. Constants and function names are not included.
    QStringList variables() const;

    // Returns nullopt with *error set for an invalid expression, an undefined
    // variable, a wrong function argument count, or a result that is not a
    // finite number (division by zero, sqrt of a negative number, ...).
    std::optional<double> evaluate(const Lookup &lookup = {}, QString *error = nullptr) const;

    struct Node; // implementation detail

private:
    QString m_text;
    QString m_error;
    std::shared_ptr<const Node> m_root;
};

// True for names that variables cannot use: constants and function names.
bool isReservedName(const QString &name);

// True for a syntactically valid identifier that is not reserved.
bool isValidVariableName(const QString &name);

struct VariableResults {
    QHash<QString, double> values; // successfully evaluated variables
    QHash<QString, QString> errors; // variable name -> message
};

// Evaluates an ordered list of (name, expression text) definitions. Variables
// may refer to each other in any order; circular references are errors for
// every variable on the cycle, and a variable depending on a failed one fails
// too. Names not defined in the list are resolved through `outer` (for example
// a parent sheet). Invalid or duplicate names are errors.
VariableResults evaluateVariables(const QList<QPair<QString, QString>> &definitions,
                                  const Expression::Lookup &outer = {});

} // namespace lvd

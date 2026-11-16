#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Expression.h"

#include <cmath>
#include <limits>

using namespace lvd;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const double Pi = 3.14159265358979323846;

QString q(const char *text)
{
    return QString::fromUtf8(text);
}

// Parses and evaluates; fails the test if either step fails.
double eval(const char *text, const Expression::Lookup &lookup = {})
{
    const Expression expression = Expression::parse(q(text));
    INFO("expression: " << text << " error: " << expression.error().toStdString());
    REQUIRE(expression.isValid());
    QString error;
    const auto value = expression.evaluate(lookup, &error);
    INFO("evaluation error: " << error.toStdString());
    REQUIRE(value.has_value());
    return *value;
}

QString parseError(const char *text)
{
    const Expression expression = Expression::parse(q(text));
    INFO("expression: " << text);
    REQUIRE_FALSE(expression.isValid());
    REQUIRE_FALSE(expression.error().isEmpty());
    return expression.error();
}

QString evalError(const char *text, const Expression::Lookup &lookup = {})
{
    const Expression expression = Expression::parse(q(text));
    INFO("expression: " << text << " parse error: " << expression.error().toStdString());
    REQUIRE(expression.isValid());
    QString error;
    REQUIRE_FALSE(expression.evaluate(lookup, &error).has_value());
    REQUIRE_FALSE(error.isEmpty());
    return error;
}

double number(const char *text)
{
    const auto value = parseNumber(q(text));
    INFO("number: " << text);
    REQUIRE(value.has_value());
    return *value;
}

bool isNumber(const char *text)
{
    return parseNumber(q(text)).has_value();
}

std::string str(const QString &text)
{
    return text.toStdString();
}

} // namespace

TEST_CASE("plain numbers")
{
    CHECK(number("15") == 15.0);
    CHECK(number("0") == 0.0);
    CHECK_THAT(number("2.5"), WithinRel(2.5));
    CHECK_THAT(number(".5"), WithinRel(0.5));
    CHECK_THAT(number("5."), WithinRel(5.0));
    CHECK_THAT(number("1e-3"), WithinRel(1e-3));
    CHECK_THAT(number("2.5E6"), WithinRel(2.5e6));
    CHECK_THAT(number("1e+3"), WithinRel(1e3));
    CHECK_THAT(number("  42  "), WithinRel(42.0));
    CHECK_THAT(number("-3.5"), WithinRel(-3.5));
    CHECK_THAT(number("+3.5"), WithinRel(3.5));
}

TEST_CASE("SI prefixes")
{
    CHECK_THAT(number("1a"), WithinRel(1e-18));
    CHECK_THAT(number("1f"), WithinRel(1e-15));
    CHECK_THAT(number("1p"), WithinRel(1e-12));
    CHECK_THAT(number("1n"), WithinRel(1e-9));
    CHECK_THAT(number("1u"), WithinRel(1e-6));
    CHECK_THAT(number("1µ"), WithinRel(1e-6));
    CHECK_THAT(number("1m"), WithinRel(1e-3));
    CHECK_THAT(number("1k"), WithinRel(1e3));
    CHECK_THAT(number("1M"), WithinRel(1e6));
    CHECK_THAT(number("1G"), WithinRel(1e9));
    CHECK_THAT(number("1T"), WithinRel(1e12));
    CHECK_THAT(number("1meg"), WithinRel(1e6));
    CHECK_THAT(number("1MEG"), WithinRel(1e6));
    CHECK_THAT(number("1Meg"), WithinRel(1e6));
    CHECK_THAT(number("1.5e3k"), WithinRel(1.5e6));
}

TEST_CASE("attached units are ignored")
{
    CHECK_THAT(number("2.4GHz"), WithinRel(2.4e9));
    CHECK_THAT(number("1.2pF"), WithinRel(1.2e-12));
    CHECK_THAT(number("10mW"), WithinRel(10e-3));
    CHECK_THAT(number("50ohm"), WithinRel(50.0));
    CHECK_THAT(number("50Ω"), WithinRel(50.0));
    CHECK_THAT(number("4.7kΩ"), WithinRel(4700.0));
    CHECK_THAT(number("15dBm"), WithinRel(15.0));
    CHECK_THAT(number("5dB"), WithinRel(5.0));
    CHECK_THAT(number("5F"), WithinRel(5.0));
    CHECK_THAT(number("5fF"), WithinRel(5e-15));
    CHECK_THAT(number("300K"), WithinRel(300.0));
    CHECK_THAT(number("2µF"), WithinRel(2e-6));
    CHECK_THAT(number("3megohm"), WithinRel(3e6));
    CHECK_THAT(number("2x"), WithinRel(2.0)); // SPICE: unknown letters are a unit
}

TEST_CASE("European notation")
{
    CHECK_THAT(number("4n7"), WithinRel(4.7e-9));
    CHECK_THAT(number("2k2"), WithinRel(2200.0));
    CHECK_THAT(number("1M5"), WithinRel(1.5e6));
    CHECK_THAT(number("2meg2"), WithinRel(2.2e6));
    CHECK_THAT(number("4u7"), WithinRel(4.7e-6));
    CHECK_THAT(number("4µ7"), WithinRel(4.7e-6));
    CHECK_THAT(number("1p05"), WithinRel(1.05e-12));
    CHECK_FALSE(isNumber("4n7F"));
    CHECK_FALSE(isNumber("2k2k"));
    CHECK_FALSE(isNumber("1.5k2"));
    CHECK_FALSE(isNumber("1e3k2"));
    CHECK(parseError("4n7F + 1").contains(QStringLiteral("position")));
}

TEST_CASE("spaced units")
{
    CHECK_THAT(number("2.4 GHz"), WithinRel(2.4e9));
    CHECK_THAT(number("10 dBm"), WithinRel(10.0));
    CHECK_THAT(number("5 dB"), WithinRel(5.0));
    CHECK_THAT(number("3 mm"), WithinRel(3e-3));
    CHECK_THAT(number("2 m"), WithinRel(2.0));
    CHECK_THAT(number("2 ms"), WithinRel(2e-3));
    CHECK_THAT(number("1.2 pF"), WithinRel(1.2e-12));
    CHECK_THAT(number("2 F"), WithinRel(2.0));
    CHECK_THAT(number("50 ohm"), WithinRel(50.0));
    CHECK_THAT(number("50 Ohm"), WithinRel(50.0));
    CHECK_THAT(number("50 Ω"), WithinRel(50.0));
    CHECK_THAT(number("4.7 kΩ"), WithinRel(4700.0));
    CHECK_THAT(number("10 µH"), WithinRel(10e-6));
    CHECK_THAT(number("10 uH"), WithinRel(10e-6));
    CHECK_THAT(number("3 V"), WithinRel(3.0));
    CHECK_THAT(number("3 mA"), WithinRel(3e-3));
    CHECK_THAT(number("3 kW"), WithinRel(3e3));
    CHECK_THAT(number("3 mS"), WithinRel(3e-3));
    CHECK_THAT(number("3 dBW"), WithinRel(3.0));
    CHECK_THAT(number("3 dBc"), WithinRel(3.0));
    CHECK_THAT(number("3 dBi"), WithinRel(3.0));
    CHECK_THAT(number("45 deg"), WithinRel(45.0));
    CHECK_THAT(number("1 rad"), WithinRel(1.0));
    CHECK_THAT(number("300 K"), WithinRel(300.0));
    CHECK_THAT(number("2 megohm"), WithinRel(2e6));
    CHECK_THAT(number("2\t MHz"), WithinRel(2e6));

    CHECK_FALSE(isNumber("2 x"));
    CHECK_FALSE(isNumber("2 freq"));
    CHECK_FALSE(isNumber("2 T"));
    CHECK_FALSE(isNumber("2 GHzz"));
    CHECK_FALSE(isNumber("2G Hz")); // a spaced unit only follows a bare number
    CHECK_FALSE(isNumber("4n7 F"));
    CHECK(parseError("2 x").contains(QStringLiteral("'x'")));
    CHECK(parseError("2 freq").contains(QStringLiteral("'freq'")));

    CHECK_THAT(eval("2.4 GHz / 2"), WithinRel(1.2e9));
    CHECK_THAT(eval("10 dBm + 3 dB"), WithinRel(13.0));
    CHECK_THAT(eval("(3 mm)*2"), WithinRel(6e-3));
}

TEST_CASE("parseNumber rejects non-numbers")
{
    CHECK_FALSE(isNumber(""));
    CHECK_FALSE(isNumber("   "));
    CHECK_FALSE(isNumber("abc"));
    CHECK_FALSE(isNumber("1+2"));
    CHECK_FALSE(isNumber("1 2"));
    CHECK_FALSE(isNumber("-"));
    CHECK_FALSE(isNumber("."));
    CHECK_FALSE(isNumber("- 5"));
    CHECK_FALSE(isNumber("1e999"));
}

TEST_CASE("operators, precedence and associativity")
{
    CHECK_THAT(eval("1 + 2 * 3"), WithinRel(7.0));
    CHECK_THAT(eval("(1 + 2) * 3"), WithinRel(9.0));
    CHECK_THAT(eval("10 - 4 - 3"), WithinRel(3.0));
    CHECK_THAT(eval("100 / 10 / 5"), WithinRel(2.0));
    CHECK_THAT(eval("2 ^ 3 ^ 2"), WithinRel(512.0));
    CHECK_THAT(eval("-2^2"), WithinRel(-4.0));
    CHECK_THAT(eval("(-2)^2"), WithinRel(4.0));
    CHECK_THAT(eval("2^-1"), WithinRel(0.5));
    CHECK_THAT(eval("-3"), WithinRel(-3.0));
    CHECK_THAT(eval("+3"), WithinRel(3.0));
    CHECK_THAT(eval("--3"), WithinRel(3.0));
    CHECK_THAT(eval("2 * -3"), WithinRel(-6.0));
    CHECK_THAT(eval("2*3^2"), WithinRel(18.0));
    CHECK_THAT(eval("1 - -1"), WithinRel(2.0));
    CHECK_THAT(eval("((((1))))"), WithinRel(1.0));
    CHECK_THAT(eval("1k + 2k2"), WithinRel(3200.0));
    CHECK_THAT(eval("1n*1G"), WithinRel(1.0));
    CHECK_THAT(eval("  6/  4 "), WithinRel(1.5));
}

TEST_CASE("functions")
{
    CHECK_THAT(eval("sqrt(16)"), WithinRel(4.0));
    CHECK_THAT(eval("exp(1)"), WithinRel(std::exp(1.0)));
    CHECK_THAT(eval("ln(exp(2))"), WithinRel(2.0));
    CHECK_THAT(eval("log10(1000)"), WithinRel(3.0));
    CHECK_THAT(eval("sin(pi/2)"), WithinRel(1.0));
    CHECK_THAT(eval("cos(pi)"), WithinRel(-1.0));
    CHECK_THAT(eval("tan(pi/4)"), WithinRel(1.0, 1e-12));
    CHECK_THAT(eval("asin(1)"), WithinRel(Pi / 2));
    CHECK_THAT(eval("acos(0)"), WithinRel(Pi / 2));
    CHECK_THAT(eval("atan(1)"), WithinRel(Pi / 4));
    CHECK_THAT(eval("atan2(1, -1)"), WithinRel(3 * Pi / 4));
    CHECK_THAT(eval("sinh(1)"), WithinRel(std::sinh(1.0)));
    CHECK_THAT(eval("cosh(1)"), WithinRel(std::cosh(1.0)));
    CHECK_THAT(eval("tanh(0.5)"), WithinRel(std::tanh(0.5)));
    CHECK_THAT(eval("abs(-7)"), WithinRel(7.0));
    CHECK_THAT(eval("min(3)"), WithinRel(3.0));
    CHECK_THAT(eval("min(3, -1, 2)"), WithinRel(-1.0));
    CHECK_THAT(eval("max(3, -1, 2)"), WithinRel(3.0));
    CHECK_THAT(eval("max(1)"), WithinRel(1.0));
    CHECK_THAT(eval("pow(2, 10)"), WithinRel(1024.0));
    CHECK_THAT(eval("floor(2.7)"), WithinRel(2.0));
    CHECK_THAT(eval("floor(-2.2)"), WithinRel(-3.0));
    CHECK_THAT(eval("ceil(2.2)"), WithinRel(3.0));
    CHECK_THAT(eval("round(2.5)"), WithinRel(3.0));
    CHECK_THAT(eval("round(-2.5)"), WithinRel(-3.0));
    CHECK_THAT(eval("round(2.4)"), WithinRel(2.0));
    CHECK_THAT(eval("db10(100)"), WithinRel(20.0));
    CHECK_THAT(eval("db20(100)"), WithinRel(40.0));
    CHECK_THAT(eval("undb10(20)"), WithinRel(100.0));
    CHECK_THAT(eval("undb20(40)"), WithinRel(100.0));
    CHECK_THAT(eval("undb10(db10(2.5))"), WithinRel(2.5));
    CHECK_THAT(eval("sqrt(2)^2"), WithinRel(2.0));
    CHECK_THAT(eval("max(1, min(5, 4)) * 2"), WithinRel(8.0));
    CHECK_THAT(eval("sqrt( 4 )+sqrt(9)"), WithinRel(5.0));
}

TEST_CASE("function call errors")
{
    CHECK(parseError("foo(1)") == QStringLiteral("unknown function 'foo' at position 1"));
    CHECK(parseError("1 + bar(2)").contains(QStringLiteral("unknown function 'bar'")));
    CHECK(parseError("sqrt(1, 2)").contains(QStringLiteral("'sqrt' takes 1 argument, got 2")));
    CHECK(parseError("atan2(1)").contains(QStringLiteral("'atan2' takes 2 arguments, got 1")));
    CHECK(parseError("pow(1, 2, 3)").contains(QStringLiteral("'pow' takes 2 arguments, got 3")));
    CHECK(parseError("sqrt()").contains(QStringLiteral("got 0")));
    CHECK(parseError("min()").contains(QStringLiteral("at least 1 argument")));
    CHECK(parseError("max()").contains(QStringLiteral("at least 1 argument")));
    CHECK(parseError("sqrt + 1").contains(QStringLiteral("'sqrt'")));
    CHECK(parseError("pi(2)").contains(QStringLiteral("constant")));
    CHECK(parseError("sqrt(4").contains(QStringLiteral("missing ')'")));
    CHECK(parseError("min(1,)").contains(QStringLiteral("unexpected ')'")));
    CHECK(parseError("min(1 2)").contains(QStringLiteral("expected ')'")));
}

TEST_CASE("constants")
{
    CHECK_THAT(eval("pi"), WithinRel(Pi));
    CHECK_THAT(eval("c0"), WithinRel(299792458.0));
    CHECK_THAT(eval("kB"), WithinRel(1.380649e-23));
    CHECK_THAT(eval("T0"), WithinRel(290.0));
    CHECK_THAT(eval("kB*T0"), WithinRel(1.380649e-23 * 290.0));
    CHECK_THAT(eval("c0 / 2.4GHz"), WithinRel(299792458.0 / 2.4e9));
    CHECK(Expression::parse(QStringLiteral("2*pi*c0 + kB*T0")).variables().isEmpty());
}

TEST_CASE("syntax errors")
{
    CHECK(parseError("") == QStringLiteral("empty expression"));
    CHECK(parseError("   ") == QStringLiteral("empty expression"));
    CHECK(Expression().error().isEmpty());
    CHECK_FALSE(Expression().isValid());

    CHECK(parseError("(1+2))") == QStringLiteral("unexpected ')' at position 6"));
    CHECK(parseError("1 +") == QStringLiteral("unexpected end of expression"));
    CHECK(parseError("* 2") == QStringLiteral("unexpected '*' at position 1"));
    CHECK(parseError("(1 + 2") == QStringLiteral("missing ')' for '(' at position 1"));
    CHECK(parseError("()").contains(QStringLiteral("unexpected ')' at position 2")));
    CHECK(parseError("1 # 2") == QStringLiteral("unexpected character '#' at position 3"));
    CHECK(parseError("2 3").contains(QStringLiteral("unexpected '3' at position 3")));
    CHECK(parseError("a b").contains(QStringLiteral("unexpected 'b'")));
    CHECK(parseError("1,2").contains(QStringLiteral("unexpected ','")));
    CHECK(parseError("1e999").contains(QStringLiteral("out of range")));
    CHECK(parseError("1.5k2").contains(QStringLiteral("invalid number")));

    const Expression invalid = Expression::parse(QStringLiteral("1 +"));
    CHECK(invalid.text() == QStringLiteral("1 +"));
    QString error;
    CHECK_FALSE(invalid.evaluate({}, &error).has_value());
    CHECK(error == invalid.error());
    CHECK_FALSE(invalid.evaluate().has_value()); // no error pointer is fine
    CHECK(invalid.variables().isEmpty());

    QString emptyError;
    CHECK_FALSE(Expression().evaluate({}, &emptyError).has_value());
    CHECK_FALSE(emptyError.isEmpty());
}

TEST_CASE("variables and lookup")
{
    const auto lookup = [](const QString &name) -> std::optional<double> {
        if (name == QStringLiteral("gain"))
            return 15.0;
        if (name == QStringLiteral("f"))
            return 2.4e9;
        if (name == QStringLiteral("bad"))
            return std::numeric_limits<double>::infinity();
        return std::nullopt;
    };
    CHECK_THAT(eval("gain + 3", lookup), WithinRel(18.0));
    CHECK_THAT(eval("c0 / f", lookup), WithinRel(299792458.0 / 2.4e9));
    CHECK(evalError("gain_lna + 3", lookup) == QStringLiteral("undefined variable 'gain_lna'"));
    CHECK(evalError("gain + 3") == QStringLiteral("undefined variable 'gain'"));
    CHECK(evalError("bad", lookup).contains(QStringLiteral("not a finite number")));

    const Expression e = Expression::parse(QStringLiteral("b + a*sqrt(b) - _x1 + pi + a"));
    REQUIRE(e.isValid());
    CHECK(e.variables() == QStringList{QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("_x1")});

    // Copies share the tree and behave identically.
    const Expression copy = e;
    CHECK(copy.isValid());
    CHECK(copy.text() == e.text());
    CHECK(copy.variables() == e.variables());
}

TEST_CASE("non-finite results are errors")
{
    CHECK(evalError("1/0") == QStringLiteral("division by zero"));
    CHECK(evalError("1/(2-2)") == QStringLiteral("division by zero"));
    CHECK(evalError("sqrt(-1)") == QStringLiteral("sqrt of a negative number"));
    CHECK(evalError("ln(0)").contains(QStringLiteral("non-positive")));
    CHECK(evalError("log10(-1)").contains(QStringLiteral("non-positive")));
    CHECK(evalError("db10(0)").contains(QStringLiteral("non-positive")));
    CHECK(evalError("db20(-1)").contains(QStringLiteral("non-positive")));
    CHECK(evalError("asin(2)").contains(QStringLiteral("outside [-1, 1]")));
    CHECK(evalError("acos(-1.5)").contains(QStringLiteral("outside [-1, 1]")));
    CHECK(evalError("exp(1000)").contains(QStringLiteral("not a finite number")));
    CHECK(evalError("10^400").contains(QStringLiteral("not a finite number")));
    CHECK(evalError("(-8)^(1/3)").contains(QStringLiteral("not a finite number")));
    CHECK(evalError("0^-1").contains(QStringLiteral("not a finite number")));
    CHECK(evalError("1e300*1e300").contains(QStringLiteral("not a finite number")));
    CHECK(evalError("pow(0, -1)").contains(QStringLiteral("not a finite number")));
    CHECK(evalError("undb10(1e5)").contains(QStringLiteral("not a finite number")));
}

TEST_CASE("reserved and valid variable names")
{
    for (const char *name : {"sqrt", "exp", "ln", "log10", "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
                             "sinh", "cosh", "tanh", "abs", "min", "max", "pow", "floor", "ceil", "round", "db10",
                             "db20", "undb10", "undb20", "pi", "c0", "kB", "T0"}) {
        INFO(name);
        CHECK(isReservedName(q(name)));
        CHECK_FALSE(isValidVariableName(q(name)));
    }
    CHECK_FALSE(isReservedName(QStringLiteral("gain")));
    CHECK_FALSE(isReservedName(QStringLiteral("Pi")));
    CHECK(isValidVariableName(QStringLiteral("gain")));
    CHECK(isValidVariableName(QStringLiteral("gain_lna")));
    CHECK(isValidVariableName(QStringLiteral("_x")));
    CHECK(isValidVariableName(QStringLiteral("X9")));
    CHECK(isValidVariableName(QStringLiteral("Pi")));
    CHECK_FALSE(isValidVariableName(QString()));
    CHECK_FALSE(isValidVariableName(QStringLiteral("9x")));
    CHECK_FALSE(isValidVariableName(QStringLiteral("a-b")));
    CHECK_FALSE(isValidVariableName(QStringLiteral("a b")));
    CHECK_FALSE(isValidVariableName(QStringLiteral("µ")));
}

TEST_CASE("evaluateVariables: ordering and outer lookup")
{
    const VariableResults r = evaluateVariables({
        {QStringLiteral("total"), QStringLiteral("gain_lna + gain_mix")},
        {QStringLiteral("gain_lna"), QStringLiteral("15")},
        {QStringLiteral("gain_mix"), QStringLiteral("-7 + offset")},
        {QStringLiteral("f"), QStringLiteral("2.4GHz")},
        {QStringLiteral("lambda"), QStringLiteral("c0 / f")},
    }, [](const QString &name) -> std::optional<double> {
        if (name == QStringLiteral("offset"))
            return 1.0;
        return std::nullopt;
    });
    CHECK(r.errors.isEmpty());
    CHECK_THAT(r.values.value(QStringLiteral("total")), WithinRel(9.0));
    CHECK_THAT(r.values.value(QStringLiteral("gain_mix")), WithinRel(-6.0));
    CHECK_THAT(r.values.value(QStringLiteral("lambda")), WithinRel(299792458.0 / 2.4e9));
    CHECK(r.values.size() == 5);
}

TEST_CASE("evaluateVariables: shadowing and undefined names")
{
    const auto outer = [](const QString &name) -> std::optional<double> {
        if (name == QStringLiteral("x"))
            return 100.0;
        if (name == QStringLiteral("y"))
            return 7.0;
        return std::nullopt;
    };
    const VariableResults r = evaluateVariables({
        {QStringLiteral("x"), QStringLiteral("1")},
        {QStringLiteral("a"), QStringLiteral("x + y")},
        {QStringLiteral("b"), QStringLiteral("z")},
    }, outer);
    CHECK_THAT(r.values.value(QStringLiteral("a")), WithinRel(8.0));
    CHECK(r.errors.value(QStringLiteral("b")) == QStringLiteral("undefined variable 'z'"));

    const VariableResults noOuter = evaluateVariables({{QStringLiteral("a"), QStringLiteral("y")}});
    CHECK(noOuter.errors.value(QStringLiteral("a")) == QStringLiteral("undefined variable 'y'"));
    CHECK(noOuter.values.isEmpty());

    CHECK(evaluateVariables({}).values.isEmpty());
    CHECK(evaluateVariables({}).errors.isEmpty());
}

TEST_CASE("evaluateVariables: cycles")
{
    const VariableResults r = evaluateVariables({
        {QStringLiteral("a"), QStringLiteral("b + 1")},
        {QStringLiteral("b"), QStringLiteral("a * 2")},
        {QStringLiteral("c"), QStringLiteral("c")},
        {QStringLiteral("d"), QStringLiteral("a + 1")},
        {QStringLiteral("e"), QStringLiteral("5")},
        {QStringLiteral("p"), QStringLiteral("q")},
        {QStringLiteral("q"), QStringLiteral("r")},
        {QStringLiteral("r"), QStringLiteral("p + e")},
    });
    CHECK(r.errors.value(QStringLiteral("a")) == QStringLiteral("circular definition: a -> b -> a"));
    CHECK(r.errors.value(QStringLiteral("b")) == QStringLiteral("circular definition: b -> a -> b"));
    CHECK(r.errors.value(QStringLiteral("c")) == QStringLiteral("circular definition: c -> c"));
    CHECK(r.errors.value(QStringLiteral("d")) == QStringLiteral("depends on 'a', which has an error"));
    CHECK(r.errors.value(QStringLiteral("p")) == QStringLiteral("circular definition: p -> q -> r -> p"));
    CHECK(r.errors.value(QStringLiteral("q")) == QStringLiteral("circular definition: q -> r -> p -> q"));
    CHECK(r.errors.value(QStringLiteral("r")) == QStringLiteral("circular definition: r -> p -> q -> r"));
    CHECK_FALSE(r.errors.contains(QStringLiteral("e")));
    CHECK(r.values.size() == 1);
    CHECK(r.values.value(QStringLiteral("e")) == 5.0);
}

TEST_CASE("evaluateVariables: failure propagation")
{
    const VariableResults r = evaluateVariables({
        {QStringLiteral("x"), QStringLiteral("1 +")},
        {QStringLiteral("y"), QStringLiteral("x * 2")},
        {QStringLiteral("z"), QStringLiteral("y + 1")},
        {QStringLiteral("w"), QStringLiteral("1/0")},
        {QStringLiteral("v"), QStringLiteral("w")},
        {QStringLiteral("ok"), QStringLiteral("3")},
    });
    CHECK(r.errors.value(QStringLiteral("x")) == QStringLiteral("unexpected end of expression"));
    CHECK(r.errors.value(QStringLiteral("y")) == QStringLiteral("depends on 'x', which has an error"));
    CHECK(r.errors.value(QStringLiteral("z")) == QStringLiteral("depends on 'y', which has an error"));
    CHECK(r.errors.value(QStringLiteral("w")) == QStringLiteral("division by zero"));
    CHECK(r.errors.value(QStringLiteral("v")) == QStringLiteral("depends on 'w', which has an error"));
    CHECK(r.values.size() == 1);
    CHECK(r.values.value(QStringLiteral("ok")) == 3.0);
}

TEST_CASE("evaluateVariables: invalid and duplicate names")
{
    const VariableResults r = evaluateVariables({
        {QStringLiteral("a"), QStringLiteral("1")},
        {QStringLiteral("a"), QStringLiteral("2")},
        {QStringLiteral("pi"), QStringLiteral("3")},
        {QStringLiteral("9x"), QStringLiteral("4")},
        {QString(), QStringLiteral("5")},
        {QStringLiteral("b"), QStringLiteral("a * 10")},
        {QStringLiteral("sqrt"), QStringLiteral("1")},
    });
    CHECK(r.errors.value(QStringLiteral("a")).contains(QStringLiteral("defined more than once")));
    CHECK(r.values.value(QStringLiteral("a")) == 1.0); // the first definition is used
    CHECK_THAT(r.values.value(QStringLiteral("b")), WithinRel(10.0));
    CHECK(r.errors.value(QStringLiteral("pi")).contains(QStringLiteral("reserved")));
    CHECK(r.errors.value(QStringLiteral("sqrt")).contains(QStringLiteral("reserved")));
    CHECK(r.errors.value(QStringLiteral("9x")).contains(QStringLiteral("not a valid variable name")));
    CHECK(r.errors.contains(QString()));
    CHECK_FALSE(r.values.contains(QStringLiteral("pi")));
}

TEST_CASE("formatEngineering")
{
    CHECK(str(formatEngineering(2.4e9, QStringLiteral("Hz"))) == "2.4 GHz");
    CHECK(formatEngineering(4.7e-9, QStringLiteral("F")) == QStringLiteral("4.7 nF"));
    CHECK(formatEngineering(50, QStringLiteral("ohm")) == QStringLiteral("50 ohm"));
    CHECK(formatEngineering(2200) == QStringLiteral("2.2k"));
    CHECK(formatEngineering(1.5) == QStringLiteral("1.5"));
    CHECK(formatEngineering(0, QStringLiteral("V")) == QStringLiteral("0 V"));
    CHECK(formatEngineering(0) == QStringLiteral("0"));
    CHECK(formatEngineering(-3.3e-3, QStringLiteral("A")) == QStringLiteral("-3.3 mA"));
    CHECK(formatEngineering(1e-6, QStringLiteral("H")) == q("1 µH"));
    CHECK(formatEngineering(1e-15, QStringLiteral("F")) == QStringLiteral("1 fF"));
    CHECK(formatEngineering(1e-12, QStringLiteral("F")) == QStringLiteral("1 pF"));
    CHECK(formatEngineering(1e12, QStringLiteral("Hz")) == QStringLiteral("1 THz"));
    CHECK(formatEngineering(1e6, QStringLiteral("Hz")) == QStringLiteral("1 MHz"));
    CHECK(formatEngineering(1000) == QStringLiteral("1k"));
    CHECK(formatEngineering(999.99) == QStringLiteral("1k")); // rounding carries over
    CHECK(formatEngineering(999.4) == QStringLiteral("999.4"));
    CHECK(formatEngineering(0.001) == QStringLiteral("1m"));
    CHECK(formatEngineering(123456789, QStringLiteral("Hz")) == QStringLiteral("123.5 MHz"));
    CHECK(formatEngineering(123456789, QStringLiteral("Hz"), 6) == QStringLiteral("123.457 MHz"));
    CHECK(formatEngineering(123456789, QStringLiteral("Hz"), 1) == QStringLiteral("100 MHz"));
    CHECK(formatEngineering(1.23456, QString(), 0) == QStringLiteral("1"));

    CHECK(formatEngineering(15, QStringLiteral("dB")) == QStringLiteral("15 dB"));
    CHECK(formatEngineering(-3.01, QStringLiteral("dBm")) == QStringLiteral("-3.01 dBm"));
    CHECK(formatEngineering(2000, QStringLiteral("dBW")) == QStringLiteral("2000 dBW"));
    CHECK(formatEngineering(0.001, QStringLiteral("dBc")) == QStringLiteral("0.001 dBc"));
    CHECK(formatEngineering(5, QStringLiteral("dBi")) == QStringLiteral("5 dBi"));
    CHECK(formatEngineering(45, QStringLiteral("deg")) == QStringLiteral("45 deg"));
    CHECK(formatEngineering(12.5, QStringLiteral("%")) == QStringLiteral("12.5 %"));
    CHECK(formatEngineering(0, QStringLiteral("dB")) == QStringLiteral("0 dB"));
    CHECK(formatEngineering(12345.678, QStringLiteral("dB")) == QStringLiteral("12350 dB"));
    CHECK(formatEngineering(12345.678, QStringLiteral("dB"), 6) == QStringLiteral("12345.7 dB"));

    CHECK(formatEngineering(1e15, QStringLiteral("Hz")) == QStringLiteral("1e15 Hz"));
    CHECK(formatEngineering(2.5e-18, QStringLiteral("F")) == QStringLiteral("2.5e-18 F"));
    CHECK(formatEngineering(-1.234e20) == QStringLiteral("-1.234e20"));

    // Formatting and parsing round-trip.
    for (const double value : {2.4e9, 4.7e-9, 1.2e-12, 3.3e-6, 47e3, 1.5e6, 0.25, 999.0}) {
        INFO(value);
        const auto parsed = parseNumber(formatEngineering(value, QStringLiteral("F")));
        REQUIRE(parsed.has_value());
        CHECK_THAT(*parsed, WithinRel(value, 1e-9));
    }
}

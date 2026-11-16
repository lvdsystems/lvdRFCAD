#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "spice/Fourier.h"
#include "spice/VectorExpression.h"
#include "spice/WaveformMath.h"

#include <QHash>

#include <cmath>
#include <numbers>
#include <random>

using namespace lvd::spice;
using Catch::Approx;

namespace {

constexpr double Pi = std::numbers::pi;

// A non-uniform scale over [from, to]: steps vary randomly between 0.3 and
// 1.7 times the nominal step, as transient time steps do. Both ends are exact.
std::vector<double> jitteredScale(double from, double to, int nominalPoints, unsigned seed = 1)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> factor(0.3, 1.7);
    const double step = (to - from) / nominalPoints;
    std::vector<double> x{from};
    while (x.back() + 0.3 * step < to)
        x.push_back(std::min(to, x.back() + factor(random) * step));
    x.back() = to;
    return x;
}

std::vector<double> uniformScale(double from, double to, int intervals)
{
    std::vector<double> x(std::size_t(intervals) + 1);
    for (int i = 0; i <= intervals; ++i)
        x[std::size_t(i)] = from + (to - from) * i / intervals;
    return x;
}

template <typename Fn>
Waveform sampled(const std::vector<double> &x, Fn fn, const QString &unit = QString())
{
    std::vector<double> y(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        y[i] = fn(x[i]);
    return makeWaveform(x, y, unit);
}

template <typename Fn>
Waveform sampledComplex(const std::vector<double> &x, Fn fn)
{
    Waveform w;
    w.x = x;
    w.isComplex = true;
    for (const double v : x)
        w.y.push_back(fn(v));
    return w;
}

// Square wave of the given duty cycle with ideal steps (two samples at each
// edge) plus jittered samples on the flat parts.
Waveform squareWave(double period, double duty, int periods, double high = 1, double low = -1)
{
    Waveform w;
    std::mt19937 random(7);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const auto flat = [&](double from, double to, double value) {
        w.x.push_back(from);
        w.y.push_back(value);
        for (int k = 0; k < 20; ++k) {
            const double t = from + (to - from) * (k + u(random)) / 21.0;
            w.x.push_back(t);
            w.y.push_back(value);
        }
        w.x.push_back(to);
        w.y.push_back(value);
    };
    for (int p = 0; p < periods; ++p) {
        const double t0 = p * period;
        flat(t0, t0 + duty * period, high);
        flat(t0 + duty * period, t0 + period, low);
    }
    return w;
}

std::vector<Complex> naiveDft(const std::vector<Complex> &x)
{
    const std::size_t n = x.size();
    std::vector<Complex> result(n);
    for (std::size_t k = 0; k < n; ++k) {
        Complex sum(0, 0);
        for (std::size_t i = 0; i < n; ++i)
            sum += x[i] * std::polar(1.0, -2 * Pi * double((k * i) % n) / double(n));
        result[k] = sum;
    }
    return result;
}

struct Vectors {
    QHash<QString, Waveform> map;
    VectorExpression::Resolver resolver() const
    {
        return [this](const QString &name) -> std::optional<Waveform> {
            const auto it = map.find(name);
            if (it == map.end())
                return std::nullopt;
            return *it;
        };
    }
};

std::optional<Waveform> eval(const QString &text, const Vectors &vectors, QString *error = nullptr)
{
    QString parseError;
    const auto expression = VectorExpression::parse(text, &parseError);
    if (!expression) {
        if (error)
            *error = parseError;
        return std::nullopt;
    }
    return expression->evaluate(vectors.resolver(), error);
}

double scalar(const QString &text)
{
    QString error;
    const auto w = eval(text, Vectors{}, &error);
    INFO(text.toStdString() << ": " << error.toStdString());
    REQUIRE(w);
    REQUIRE(w->y.size() == 1);
    return w->y[0].real();
}

QString parseError(const QString &text)
{
    QString error;
    const auto expression = VectorExpression::parse(text, &error);
    CHECK_FALSE(expression);
    return error;
}

} // namespace

// ---------------------------------------------------------------------------
// Expressions

TEST_CASE("Expressions: arithmetic with SI suffixes", "[expression]")
{
    CHECK(scalar("1+2*3") == Approx(7));
    CHECK(scalar("(1+2)*3") == Approx(9));
    CHECK(scalar("2^3^2") == Approx(512));
    CHECK(scalar("-2^2") == Approx(-4));
    CHECK(scalar("2^-1") == Approx(0.5));
    CHECK(scalar("10/4") == Approx(2.5));
    CHECK(scalar("1k") == Approx(1e3));
    CHECK(scalar("2.2u") == Approx(2.2e-6));
    CHECK(scalar("1m") == Approx(1e-3));
    CHECK(scalar("1M") == Approx(1e6));
    CHECK(scalar("10meg") == Approx(1e7));
    CHECK(scalar("10MEG") == Approx(1e7));
    CHECK(scalar("4n7") == Approx(4.7e-9));
    CHECK(scalar("2k2") == Approx(2200));
    CHECK(scalar("1e-3") == Approx(1e-3));
    CHECK(scalar("1.5E+2") == Approx(150));
    CHECK(scalar("3p") == Approx(3e-12));
    CHECK(scalar("5f") == Approx(5e-15));
    CHECK(scalar("1G") == Approx(1e9));
    CHECK(scalar("2T") == Approx(2e12));
    CHECK(scalar(QString::fromUtf8("3µ")) == Approx(3e-6));
    CHECK(scalar("10mV") == Approx(0.01));
    CHECK(scalar("2.4GHz") == Approx(2.4e9));
    CHECK(scalar("pi") == Approx(Pi));
    CHECK(scalar("ln(e)") == Approx(1));
    CHECK(scalar("sqrt(16) + log10(1k)") == Approx(7));
    CHECK(scalar("mag(3 + 4*j)") == Approx(5));
    CHECK(scalar("imag(j*j + 2*j)") == Approx(2));
    CHECK(scalar("real(j*j)") == Approx(-1));
    CHECK(scalar("sin(pi/2) + cos(0) + tanh(0) + atan(1)*4/pi") == Approx(3));
}

TEST_CASE("Expressions: vectors, probes and broadcasting", "[expression]")
{
    Vectors vectors;
    const std::vector<double> t{0, 1, 2, 3};
    vectors.map[QStringLiteral("time")] = makeWaveform(t, t, QStringLiteral("s"));
    vectors.map[QStringLiteral("v(out)")] = makeWaveform(t, {1, 2, 3, 4}, QStringLiteral("V"));
    vectors.map[QStringLiteral("v(in)")] = makeWaveform(t, {0.5, 0.5, 0.5, 0.5}, QStringLiteral("V"));
    vectors.map[QStringLiteral("i(vin)")] = makeWaveform(t, {-1, -2, -3, -4}, QStringLiteral("A"));
    vectors.map[QStringLiteral("vout#branch")] = makeWaveform(t, {9, 9, 9, 9}, QStringLiteral("A"));

    SECTION("v(a,b) is a difference, names lower-cased")
    {
        const auto w = eval("V(OUT, In)", vectors);
        REQUIRE(w);
        CHECK(w->unit == "V");
        REQUIRE(w->y.size() == 4);
        CHECK(w->y[3].real() == Approx(3.5));
        CHECK(w->x == t);
        const auto e = VectorExpression::parse("V(OUT, In) * i(Vin) + vout#branch");
        REQUIRE(e);
        CHECK(e->referencedVectors() == QStringList({"v(out)", "v(in)", "i(vin)", "vout#branch"}));
    }
    SECTION("i() and units")
    {
        const auto w = eval("-i(vin)", vectors);
        REQUIRE(w);
        CHECK(w->unit == "A");
        CHECK(w->y[1].real() == Approx(2));
        CHECK(eval("v(out)*i(vin)", vectors)->unit == "");
        CHECK(eval("2*v(out) + 1", vectors)->unit == "V");
        CHECK(eval("v(out) - v(in)", vectors)->unit == "V");
        CHECK(eval("v(out) + i(vin)", vectors)->unit == "");
        CHECK(eval("db(v(out))", vectors)->unit == "dB");
    }
    SECTION("scalars broadcast")
    {
        const auto w = eval("v(out)*2 + 1k*1m", vectors);
        REQUIRE(w);
        REQUIRE(w->y.size() == 4);
        CHECK(w->y[0].real() == Approx(3));
        CHECK(w->y[3].real() == Approx(9));
        CHECK_FALSE(w->isComplex);
        const auto c = eval("v(out) * j", vectors);
        REQUIRE(c);
        CHECK(c->isComplex);
        CHECK(c->y[2].imag() == Approx(3));
    }
    SECTION("constant expressions have no scale")
    {
        const auto w = eval("2*pi", vectors);
        REQUIRE(w);
        CHECK(w->x.empty());
        REQUIRE(w->y.size() == 1);
    }
    SECTION("sqrt of a negative real becomes complex")
    {
        const auto w = eval("sqrt(-v(out))", vectors);
        REQUIRE(w);
        CHECK(w->isComplex);
        CHECK(w->y[3].imag() == Approx(2));
        CHECK_FALSE(eval("sqrt(v(out))", vectors)->isComplex);
    }
}

TEST_CASE("Expressions: complex functions", "[expression]")
{
    Vectors vectors;
    const std::vector<double> f{1, 2};
    vectors.map[QStringLiteral("v(a)")] = makeComplexWaveform(f, {0.1, 0}, {0, 1}, QStringLiteral("V"));

    const auto db = eval("db(v(a))", vectors);
    REQUIRE(db);
    CHECK_FALSE(db->isComplex);
    CHECK(db->y[0].real() == Approx(-20));
    CHECK(db->y[1].real() == Approx(0).margin(1e-12));
    CHECK(eval("db10(v(a))", vectors)->y[0].real() == Approx(-10));
    CHECK(eval("ph(j)", vectors)->y[0].real() == Approx(90));
    CHECK(eval("phase(v(a))", vectors)->y[1].real() == Approx(90));
    CHECK(eval("ph(-1)", vectors)->y[0].real() == Approx(180));
    const auto c = eval("conj(v(a))", vectors);
    CHECK(c->y[1].imag() == Approx(-1));
    CHECK(c->isComplex);
    CHECK(eval("abs(v(a))", vectors)->unit == "V");
}

TEST_CASE("Expressions: unwrapped phase and group delay", "[expression]")
{
    // A pure delay e^{-j 2 pi f tau}: the wrapped phase jumps across +-180,
    // the unwrapped one falls linearly and the group delay is tau.
    const double tau = 1e-9;
    const auto f = jitteredScale(1e6, 3e9, 400);
    Vectors vectors;
    vectors.map[QStringLiteral("frequency")] = makeWaveform(f, f, QStringLiteral("Hz"));
    vectors.map[QStringLiteral("v(out)")] = sampledComplex(f, [&](double x) { return std::polar(1.0, -2 * Pi * x * tau); });

    const auto ph = eval("ph(v(out))", vectors);
    const auto cph = eval("cph(v(out))", vectors);
    REQUIRE(ph);
    REQUIRE(cph);
    CHECK(cph->unit == "deg");
    double maxStep = 0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        CHECK(ph->y[i].real() > -180);
        CHECK(ph->y[i].real() <= 180);
        CHECK(cph->y[i].real() == Approx(-360 * f[i] * tau).margin(1e-6));
        if (i > 0)
            maxStep = std::max(maxStep, std::abs(ph->y[i].real() - ph->y[i - 1].real()));
    }
    CHECK(maxStep > 300); // the wrapped phase really wrapped

    const auto gd = eval("gd(v(out))", vectors);
    REQUIRE(gd);
    CHECK(gd->unit == "s");
    CHECK_FALSE(gd->isComplex);
    for (const auto &y : gd->y)
        CHECK(y.real() == Approx(tau).epsilon(1e-9));
    CHECK(eval("groupdelay(v(out))", vectors)->y[5].real() == Approx(tau).epsilon(1e-9));
}

TEST_CASE("Expressions: deriv, integ and statistics on a non-uniform scale", "[expression]")
{
    const auto t = jitteredScale(0, 2, 300);
    Vectors vectors;
    vectors.map[QStringLiteral("time")] = makeWaveform(t, t, QStringLiteral("s"));

    // Three-point non-uniform differences are exact for a quadratic.
    const auto d = eval("deriv(time^2)", vectors);
    REQUIRE(d);
    for (std::size_t i = 0; i < t.size(); ++i)
        CHECK(d->y[i].real() == Approx(2 * t[i]).margin(1e-9));

    // Trapezoids are exact for a constant and a straight line.
    const auto in = eval("integ(3 + 0*time)", vectors);
    REQUIRE(in);
    CHECK(in->y.front().real() == 0);
    for (std::size_t i = 0; i < t.size(); ++i)
        CHECK(in->y[i].real() == Approx(3 * t[i]).margin(1e-12));
    CHECK(eval("integ(time)", vectors)->y.back().real() == Approx(2.0));

    // A 5 Hz sine of amplitude 2 over whole periods: rms = A / sqrt 2. The
    // piecewise-linear trace of a sampled sine (about 300 samples per period,
    // steps up to 1.7x) loses roughly (w h)^2 / 12 ~ 1e-4 of its mean square.
    const auto ts = jitteredScale(0, 2, 3000, 2);
    vectors.map[QStringLiteral("v(s)")] = sampled(ts, [](double x) { return 2 * std::sin(2 * Pi * 5 * x); }, "V");
    const auto r = eval("rms(v(s))", vectors);
    REQUIRE(r);
    CHECK(r->unit == "V");
    REQUIRE(r->y.size() == ts.size());
    CHECK(r->y.front().real() == r->y.back().real());
    CHECK(r->y[0].real() == Approx(2 / std::sqrt(2.0)).epsilon(1e-3));
    CHECK(eval("mean(v(s))", vectors)->y[0].real() == Approx(0).margin(1e-3));
    CHECK(eval("avg(time)", vectors)->y[0].real() == Approx(1.0));
    CHECK(eval("max(time)", vectors)->y[3].real() == Approx(2.0));
    CHECK(eval("min(time)", vectors)->y[3].real() == Approx(0.0));
    CHECK(eval("pkpk(time*3)", vectors)->y[3].real() == Approx(6.0));
}

TEST_CASE("Expressions: errors", "[expression]")
{
    Vectors vectors;
    vectors.map[QStringLiteral("v(a)")] = makeWaveform({0, 1, 2}, {1, 2, 3});
    vectors.map[QStringLiteral("v(b)")] = makeWaveform({0, 1}, {1, 2});

    QString error;
    CHECK_FALSE(eval("v(nothere) + 1", vectors, &error));
    CHECK(error.contains("unknown vector 'v(nothere)'"));
    CHECK_FALSE(eval("speed * 2", vectors, &error));
    CHECK(error.contains("unknown vector 'speed'"));
    CHECK_FALSE(eval("v(a) + v(b)", vectors, &error));
    CHECK(error.contains("different lengths"));
    CHECK_FALSE(eval("integ(5)", vectors, &error));
    CHECK(error.contains("integ"));

    CHECK(parseError("foo(v(a))").contains("unknown function 'foo'"));
    CHECK(parseError("").contains("empty expression"));
    CHECK(parseError("   ").contains("empty expression"));
    CHECK(parseError("1 + ").contains("unexpected end"));
    CHECK(parseError("(1 + 2").contains("missing ')'"));
    CHECK(parseError("1 2").contains("unexpected '2'"));
    CHECK(parseError("v(a) )").contains("unexpected ')'"));
    CHECK(parseError("db(1, 2)").contains("one argument"));
    CHECK(parseError("db").contains("needs an argument"));
    CHECK(parseError("v(a,b,c)").contains("one or two"));
    CHECK(parseError("i(a,b)").contains("one device"));
    CHECK(parseError("v()").contains("missing name"));
    CHECK(parseError("2j").contains("unknown unit 'j'"));
    CHECK(parseError("1 $ 2").contains("unexpected '$'"));
}

// ---------------------------------------------------------------------------
// Measurements

TEST_CASE("Measurements: RC step rise time, settling and basics", "[measure]")
{
    const double tau = 1e-6;
    // Dense near the start where the trace moves, as a simulator would step.
    std::vector<double> t;
    for (double x = 0; x < 20 * tau; x += tau * (0.002 + 0.02 * x / tau))
        t.push_back(x);
    t.push_back(20 * tau);
    const auto w = sampled(t, [&](double x) { return 1 - std::exp(-x / tau); });

    // Linear interpolation of the exponential between steps of up to ~0.1 tau
    // moves the crossings by far less than 1e-3 tau.
    const auto rise = riseTime(w);
    REQUIRE(rise);
    CHECK(*rise == Approx(tau * std::log(9.0)).epsilon(1e-3));
    const auto rise2080 = riseTime(w, {}, 0.2, 0.8);
    REQUIRE(rise2080);
    CHECK(*rise2080 == Approx(tau * std::log(4.0)).epsilon(1e-3));

    QString error;
    CHECK_FALSE(fallTime(w, {}, 0.1, 0.9, &error));
    CHECK(error.contains("no falling edge"));
    CHECK_FALSE(riseTime(w, {}, 0.9, 0.1, &error));

    // Falling edge: 1 - rising.
    const auto fall = fallTime(sampled(t, [&](double x) { return std::exp(-x / tau); }));
    REQUIRE(fall);
    CHECK(*fall == Approx(tau * std::log(9.0)).epsilon(1e-3));

    const auto settle = settlingTime(w, 2);
    REQUIRE(settle);
    CHECK(*settle == Approx(tau * std::log(50.0)).epsilon(1e-3));
    CHECK(*settlingTime(w, 1) == Approx(tau * std::log(100.0)).epsilon(1e-3));

    CHECK(*valueAt(w, tau) == Approx(1 - std::exp(-1.0)).epsilon(1e-4));
    CHECK_FALSE(valueAt(w, -1, &error));
    CHECK(error.contains("outside"));
    // Mean over [0, 20 tau] of 1 - e^{-t/tau} is 1 - tau/T (1 - e^-20) = 0.95.
    CHECK(*mean(w) == Approx(0.95).epsilon(1e-4));
    CHECK(*valueAt(w, 20 * tau) == Approx(1).epsilon(1e-8));
    CHECK(*integral(w, {0, tau}) == Approx(tau * std::exp(-1.0)).epsilon(1e-4));
}

TEST_CASE("Measurements: sine frequency, period, extremes and crossings", "[measure]")
{
    const double f = 1e3;
    const auto t = jitteredScale(0, 10.25e-3, 3000, 3);
    const auto w = sampled(t, [&](double x) { return 0.5 + 2 * std::sin(2 * Pi * f * x); });

    CHECK(*frequency(w) == Approx(f).epsilon(1e-5));
    CHECK(*period(w) == Approx(1 / f).epsilon(1e-5));
    CHECK(*period(w, {2e-3, 6e-3}) == Approx(1 / f).epsilon(1e-5));
    CHECK(*dutyCycle(w) == Approx(0.5).epsilon(1e-4));

    const auto hi = maximum(w);
    const auto lo = minimum(w);
    REQUIRE(hi);
    REQUIRE(lo);
    // ~300 samples per period: the sampled peak is within (pi/300)^2/2 * 2 of 2.5.
    CHECK(hi->value == Approx(2.5).margin(2e-4));
    CHECK(std::remainder(hi->x, 1 / f) == Approx(0.25e-3).margin(5e-6));
    CHECK(lo->value == Approx(-1.5).margin(2e-4));
    CHECK(*peakToPeak(w) == Approx(4.0).margin(4e-4));
    // Over 10.25 periods the extra quarter adds 1 / (pi f T) to the mean square.
    CHECK(*rms(w) == Approx(std::sqrt(2.25 + 1 / (Pi * f * 10.25e-3))).epsilon(1e-4));

    // Crossings of the 0.5 level: rising at whole ms, falling at half ms. The start sits exactly on the level.
    CHECK(*crossing(w, 0.5, Edge::Rising, 1) == Approx(1e-3).epsilon(1e-6));
    CHECK(*crossing(w, 0.5, Edge::Rising, 3) == Approx(3e-3).epsilon(1e-6));
    CHECK(*crossing(w, 0.5, Edge::Falling, 2) == Approx(1.5e-3).epsilon(1e-6));
    CHECK(*crossing(w, 0.5, Edge::Either, 3) == Approx(1.5e-3).epsilon(1e-6));
    CHECK(*crossing(w, 0.5, Edge::Rising, 1, {2.2e-3, 1}) == Approx(3e-3).epsilon(1e-6));
    CHECK(crossings(w, 0.5, Edge::Rising).size() == 10);
    QString error;
    CHECK_FALSE(crossing(w, 0.5, Edge::Rising, 11, {}, &error));
    CHECK(error.contains("crossing"));
    CHECK_FALSE(crossing(w, 5.0, Edge::Either, 1, {}, &error));
    CHECK_FALSE(frequency(w, {0, 0.8e-3}, &error));
    CHECK(error.contains("period"));
}

TEST_CASE("Measurements: square wave duty cycle", "[measure]")
{
    const auto w = squareWave(1e-6, 0.3, 8, 3.3, 0);
    CHECK(*dutyCycle(w) == Approx(0.3).epsilon(1e-9));
    CHECK(*period(w) == Approx(1e-6).epsilon(1e-9));
    CHECK(*mean(w) == Approx(0.3 * 3.3).epsilon(1e-9));
    CHECK(*rms(w) == Approx(3.3 * std::sqrt(0.3)).epsilon(1e-9));
    // A step is a crossing at the step itself.
    CHECK(*crossing(w, 1.0, Edge::Falling, 1) == Approx(0.3e-6).epsilon(1e-9));
    CHECK(*crossing(w, 1.0, Edge::Rising, 2) == Approx(2e-6).epsilon(1e-9));
}

TEST_CASE("Measurements: underdamped step overshoot and settling", "[measure]")
{
    // Second-order step, zeta = 0.5, wn = 1: overshoot e^{-pi zeta / sqrt(1 - zeta^2)} = 16.30 %.
    const double zeta = 0.5;
    const double wd = std::sqrt(1 - zeta * zeta);
    const auto response = [&](double x) {
        return 1 - std::exp(-zeta * x) * (std::cos(wd * x) + zeta / wd * std::sin(wd * x));
    };
    const auto t = jitteredScale(0, 40, 4000, 5);
    const auto w = sampled(t, response);

    const double expected = 100 * std::exp(-Pi * zeta / wd);
    CHECK(*overshoot(w) == Approx(expected).margin(0.01));
    CHECK(expected == Approx(16.303).margin(1e-3));

    // Reference settling time: scan the analytic response finely from the end.
    double reference = 0;
    for (double x = 40; x > 0; x -= 1e-5) {
        if (std::abs(response(x) - 1) > 0.02) {
            reference = x;
            break;
        }
    }
    CHECK(*settlingTime(w, 2) == Approx(reference).margin(2e-3));

    // Negative step: the same numbers.
    const auto negative = sampled(t, [&](double x) { return 5 - response(x); });
    CHECK(*overshoot(negative) == Approx(expected).margin(0.01));
    CHECK(*settlingTime(negative, 2) == Approx(reference).margin(2e-3));

    QString error;
    CHECK_FALSE(overshoot(makeWaveform({0, 1, 2}, {1, 2, 1}), {}, &error));
    CHECK(error.contains("equal"));
}

TEST_CASE("Measurements: invalid traces and windows", "[measure]")
{
    QString error;
    CHECK_FALSE(mean(makeWaveform({0}, {1}), {}, &error));
    CHECK(error.contains("two points"));
    CHECK_FALSE(mean(makeWaveform({0, 1}, {1}), {}, &error));
    CHECK(error.contains("scale"));
    CHECK_FALSE(mean(makeWaveform({0, 2, 1}, {1, 1, 1}), {}, &error));
    CHECK(error.contains("monotonic"));
    CHECK_FALSE(mean(makeWaveform({0, 1}, {1, 1}), {5, 6}, &error));
    CHECK(error.contains("outside"));
    CHECK_FALSE(period(makeWaveform({0, 1}, {1, 1}), {}, &error));
    CHECK(error.contains("flat"));

    // Window edges are interpolated.
    const auto ramp = makeWaveform({0, 10}, {0, 10});
    CHECK(*mean(ramp, {2, 4}) == Approx(3));
    CHECK(maximum(ramp, {2, 4})->value == Approx(4));
    CHECK(minimum(ramp, {2, 4})->x == Approx(2));
    const auto part = slice(ramp, {2.5, 100});
    REQUIRE(part);
    CHECK(part->x == std::vector<double>{2.5, 10});
}

// ---------------------------------------------------------------------------
// Fourier

TEST_CASE("FFT matches a naive DFT for any length", "[fourier]")
{
    std::mt19937 random(11);
    std::normal_distribution<double> normal;
    for (const std::size_t n : {std::size_t(1), std::size_t(2), std::size_t(8), std::size_t(12), std::size_t(97),
                                std::size_t(256), std::size_t(1000)}) {
        std::vector<Complex> x(n);
        for (auto &v : x)
            v = Complex(normal(random), normal(random));
        const auto fast = fft(x);
        const auto slow = naiveDft(x);
        const auto back = fft(fast, true);
        REQUIRE(fast.size() == n);
        double error = 0;
        double roundTrip = 0;
        for (std::size_t k = 0; k < n; ++k) {
            error = std::max(error, std::abs(fast[k] - slow[k]));
            roundTrip = std::max(roundTrip, std::abs(back[k] - x[k]));
        }
        INFO("n = " << n);
        // Rounding grows like sqrt(N) log N times epsilon relative to sum |x| ~ N.
        CHECK(error < 1e-11 * double(n));
        CHECK(roundTrip < 1e-12 * double(n));
    }
    CHECK(fft({}).empty());
}

TEST_CASE("Window names", "[fourier]")
{
    for (const auto type : allWindows())
        CHECK(windowFromName(windowName(type)) == type);
    CHECK(windowFromName("blackmanharris") == WindowType::BlackmanHarris);
    CHECK(windowFromName("FLAT-TOP") == WindowType::FlatTop);
    CHECK(windowFromName("hanning") == WindowType::Hann);
    CHECK_FALSE(windowFromName("kaiser"));
}

TEST_CASE("Amplitude spectrum of a sine reads its amplitude", "[fourier]")
{
    const double fs = 1.024e6;
    const int n = 1024;
    const double f = 16 * fs / n; // bin 16
    const double width = n / fs;
    // Samples land exactly on the resampling grid (plus the end point).
    const auto t = uniformScale(0, width, n);
    const auto w = sampled(t, [&](double x) { return std::sin(2 * Pi * f * x); });

    for (const auto type : allWindows()) {
        SpectrumOptions options;
        options.window = type;
        options.points = n;
        const auto s = amplitudeSpectrum(w, options);
        REQUIRE(s);
        INFO(windowName(type).toStdString());
        REQUIRE(s->value.size() == std::size_t(n / 2 + 1));
        CHECK(s->resolution == Approx(fs / n));
        CHECK(s->sampleRate == Approx(fs));
        CHECK(s->frequency[16] == Approx(f));
        CHECK(std::abs(s->value[16]) == Approx(1.0).epsilon(1e-9));
        CHECK(std::arg(s->value[16]) * 180 / Pi == Approx(-90).margin(1e-6)); // sine = cosine - 90 deg
        CHECK(std::abs(s->value[100]) < 1e-9);
    }

    // Default points: next power of two of the 1025 samples in range = 2048.
    // Half the new samples fall between the 64 per period of the source, where
    // linear interpolation reads low by up to (w h)^2 / 8 ~ 1.2e-3.
    const auto s = amplitudeSpectrum(w);
    REQUIRE(s);
    CHECK(s->value.size() == 2048 / 2 + 1);
    CHECK(std::abs(s->value[16]) == Approx(1.0).epsilon(1e-3));

    // Half-way between bins (worst case): the flat top window still reads within 0.1 %.
    const double offBin = 16.5 * fs / n;
    const auto t2 = jitteredScale(0, width, 20000, 9);
    const auto w2 = sampled(t2, [&](double x) { return std::sin(2 * Pi * offBin * x); });
    SpectrumOptions flat;
    flat.window = WindowType::FlatTop;
    flat.points = n;
    const auto s2 = amplitudeSpectrum(w2, flat);
    REQUIRE(s2);
    double peak = 0;
    for (const auto &v : s2->value)
        peak = std::max(peak, std::abs(v));
    CHECK(peak == Approx(1.0).epsilon(1e-3));
}

TEST_CASE("Fourier analysis of a square wave", "[fourier]")
{
    // +-1 square wave of 1 kHz: b_n = 4 / (n pi) for odd n, 0 for even n.
    // The exact segment integration makes the result exact up to rounding.
    const auto w = squareWave(1e-3, 0.5, 5);
    const auto result = fourier(w, 1e3, 9);
    REQUIRE(result);
    CHECK(result->fundamental == 1e3);
    CHECK(result->dc == Approx(0).margin(1e-12));
    CHECK(result->from == Approx(4e-3));
    REQUIRE(result->harmonics.size() == 9);
    double sum = 0;
    for (const auto &h : result->harmonics) {
        INFO("n = " << h.n);
        CHECK(h.frequency == Approx(h.n * 1e3));
        if (h.n % 2) {
            CHECK(h.magnitude == Approx(4 / (h.n * Pi)).epsilon(1e-9));
            CHECK(h.normalizedMagnitude == Approx(1.0 / h.n).epsilon(1e-9));
            CHECK(h.phaseDeg == Approx(0).margin(1e-6)); // sine-like: SPICE reports 0 degrees
            if (h.n > 1)
                sum += 1.0 / (h.n * h.n);
        } else {
            CHECK(h.magnitude < 1e-12);
        }
    }
    CHECK(result->thdPercent == Approx(100 * std::sqrt(sum)).epsilon(1e-9));
    CHECK(result->thdPercent == Approx(42.88).margin(0.01));

    // Estimated fundamental, several periods.
    const auto estimated = fourier(w, 0, 9, 3);
    REQUIRE(estimated);
    CHECK(estimated->fundamental == Approx(1e3).epsilon(1e-9));
    CHECK(estimated->thdPercent == Approx(100 * std::sqrt(sum)).epsilon(1e-6));
}

TEST_CASE("Fourier THD of a sine with 1 % third harmonic on non-uniform steps", "[fourier]")
{
    const double f = 50;
    const auto t = jitteredScale(0, 0.1, 2000, 13); // 5 periods, ~400 samples each
    const auto w = sampled(t, [&](double x) {
        return 0.2 + 3 * std::sin(2 * Pi * f * x + 0.3) + 0.03 * std::sin(3 * 2 * Pi * f * x - 1.0);
    });
    const auto result = fourier(w, f, 9, 2);
    REQUIRE(result);
    // Linear interpolation at ~400 samples per period scales each harmonic by
    // about 1 - (w h)^2/12 with w h ~ 2 pi n / 400: ~2e-5 for the fundamental,
    // ~2e-4 for the 3rd, so 1e-3 relative is a safe bound.
    CHECK(result->dc == Approx(0.2).epsilon(1e-3));
    CHECK(result->harmonics[0].magnitude == Approx(3).epsilon(1e-3));
    CHECK(result->harmonics[0].phaseDeg == Approx(0.3 * 180 / Pi).margin(0.05));
    CHECK(result->harmonics[2].magnitude == Approx(0.03).epsilon(1e-3));
    CHECK(result->harmonics[2].normalizedPhaseDeg == Approx((-1.0 - 0.3) * 180 / Pi).margin(0.1));
    CHECK(result->harmonics[1].magnitude < 1e-5);
    CHECK(result->thdPercent == Approx(1.0).epsilon(2e-3));
    CHECK(*thd(w, f) == Approx(1.0).epsilon(2e-3));
    CHECK(*thd(w) == Approx(1.0).epsilon(2e-3)); // estimated fundamental

    QString error;
    CHECK_FALSE(fourier(w, 5, 9, 1, {}, &error)); // a 5 Hz period is longer than the trace
    CHECK(error.contains("shorter"));
    CHECK_FALSE(fourier(w, f, 0, 1, {}, &error));
}

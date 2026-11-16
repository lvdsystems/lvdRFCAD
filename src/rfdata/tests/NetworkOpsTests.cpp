#include "ReferenceData.h"

#include "rfdata/NetworkMath.h"
#include "rfdata/NetworkOps.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdio>

using namespace lvd::rf;
using namespace lvd::rf::test;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

NetworkData network(std::vector<double> frequencies, std::vector<CMatrix> s, std::vector<double> z0)
{
    NetworkData n;
    n.frequencies = std::move(frequencies);
    n.s = std::move(s);
    n.z0 = std::move(z0);
    return n;
}

NetworkData fromJson(const QJsonObject &object, const char *frequencies, const char *s, const char *z0)
{
    return network(toDoubles(object.value(QLatin1String(frequencies))), toMatrices(object.value(QLatin1String(s))),
                   toDoubles(object.value(QLatin1String(z0))));
}

double maxDifference(const std::vector<CMatrix> &a, const std::vector<CMatrix> &b)
{
    if (a.size() != b.size())
        return INFINITY;
    double worst = 0;
    for (size_t k = 0; k < a.size(); ++k)
        worst = std::max(worst, test::maxDifference(a[k], b[k]));
    return worst;
}

CMatrix twoPort(Complex s11, Complex s12, Complex s21, Complex s22)
{
    CMatrix s(2, 2);
    s << s11, s12, s21, s22;
    return s;
}

NetworkData attenuator(double loss, std::vector<double> frequencies)
{
    std::vector<CMatrix> s(frequencies.size(), twoPort(0, loss, loss, 0));
    return network(std::move(frequencies), std::move(s), {50, 50});
}

// Available gain of a two-port driven from a source with reflection gs.
double availableGain(const CMatrix &s, Complex gs)
{
    const Complex gOut = s(1, 1) + s(0, 1) * s(1, 0) * gs / (1.0 - s(0, 0) * gs);
    return std::norm(s(1, 0)) * (1.0 - std::norm(gs)) / (std::norm(1.0 - s(0, 0) * gs) * (1.0 - std::norm(gOut)));
}

} // namespace

// Both sides interpolate magnitude and unwrapped phase linearly: agreement to rounding.
TEST_CASE("Interpolation agrees with scikit-rf (polar coordinates)", "[rfdata][ops]")
{
    const QJsonObject data = loadJson(QStringLiteral("interpolation.json"));
    double worst = 0;
    for (const QJsonValue &item : data.value(QStringLiteral("cases")).toArray()) {
        const QJsonObject c = item.toObject();
        const NetworkData n = fromJson(c, "frequencies", "s", "z0");
        const std::vector<double> grid = toDoubles(c.value(QStringLiteral("newFrequencies")));
        QString error, warning;
        const auto result = interpolate(n, grid, Extrapolation::Error, &error, &warning);
        REQUIRE(result);
        CHECK(warning.isEmpty());
        CHECK(result->z0 == n.z0);
        const double difference = maxDifference(result->s, toMatrices(c.value(QStringLiteral("sInterpolated"))));
        worst = std::max(worst, difference);
        CHECK(difference < 1e-12);
    }
    std::printf("rfdata reference: largest interpolation deviation from scikit-rf %.2e\n", worst);
}

TEST_CASE("Interpolation edge cases", "[rfdata][ops]")
{
    const NetworkData n = network({1e9, 2e9, 3e9},
                                  {CMatrix::Constant(1, 1, std::polar(0.5, 0.0)),
                                   CMatrix::Constant(1, 1, std::polar(0.3, -3.0)),
                                   CMatrix::Constant(1, 1, std::polar(0.1, 3.0))},
                                  {50});
    QString error, warning;

    SECTION("phase is interpolated along the shortest path")
    {
        // -3 rad to 3 rad is a step of 6 - 2 pi = -0.283 rad, not +6 rad
        const auto r = interpolate(n, {2.5e9}, Extrapolation::Error, &error);
        REQUIRE(r);
        const Complex v = r->s.front()(0, 0);
        CHECK_THAT(std::abs(v), WithinRel(0.2, 1e-12));
        CHECK_THAT(std::remainder(std::arg(v) - (-3.0 + (6.0 - 2 * Pi) / 2), 2 * Pi), WithinAbs(0, 1e-12));
    }
    SECTION("passing through zero is interpolated in real and imaginary parts")
    {
        const NetworkData null = network({1e9, 2e9}, {CMatrix::Constant(1, 1, 0.01), CMatrix::Constant(1, 1, -0.01)}, {50});
        const auto r = interpolate(null, {1.5e9}, Extrapolation::Error, &error);
        REQUIRE(r);
        CHECK(std::abs(r->s.front()(0, 0)) < 1e-15);
        const NetworkData zero = network({1e9, 2e9}, {CMatrix::Constant(1, 1, 0), CMatrix::Constant(1, 1, Complex(0, 0.4))}, {50});
        const auto z = interpolate(zero, {1.25e9}, Extrapolation::Error, &error);
        REQUIRE(z);
        CHECK(std::abs(z->s.front()(0, 0) - Complex(0, 0.1)) < 1e-15);
    }
    SECTION("exact grid points are reproduced")
    {
        const auto r = interpolate(n, n.frequencies, Extrapolation::Error, &error);
        REQUIRE(r);
        CHECK(r->s == n.s);
    }
    SECTION("extrapolation fails or holds with a warning")
    {
        CHECK_FALSE(interpolate(n, {0.5e9, 1e9}, Extrapolation::Error, &error));
        CHECK_THAT(error.toStdString(), ContainsSubstring("outside the data range"));
        const auto r = interpolate(n, {0.5e9, 4e9}, Extrapolation::Hold, &error, &warning);
        REQUIRE(r);
        CHECK(r->s[0] == n.s[0]);
        CHECK(r->s[1] == n.s[2]);
        CHECK_THAT(warning.toStdString(), ContainsSubstring("2 frequencies outside"));
    }
    SECTION("invalid target grids")
    {
        CHECK_FALSE(interpolate(n, {}, Extrapolation::Hold, &error));
        CHECK_FALSE(interpolate(n, {2e9, 1e9}, Extrapolation::Hold, &error));
        CHECK_FALSE(interpolate(n, {-1.0}, Extrapolation::Hold, &error));
    }
    SECTION("a single frequency")
    {
        const NetworkData one = network({1e9}, {CMatrix::Constant(1, 1, 0.5)}, {50});
        CHECK(interpolate(one, {1e9}, Extrapolation::Error, &error));
        CHECK_FALSE(interpolate(one, {2e9}, Extrapolation::Error, &error));
        const auto r = interpolate(one, {0.5e9, 2e9}, Extrapolation::Hold, &error);
        REQUIRE(r);
        CHECK(r->s[1](0, 0) == Complex(0.5, 0));
    }
}

TEST_CASE("Noise data is interpolated", "[rfdata][ops]")
{
    NetworkData n = attenuator(0.5, {1e9, 2e9, 3e9, 4e9});
    n.noiseFrequencies = {2e9, 3e9};
    n.noise = {{1.0, std::polar(0.4, 0.2), 10.0}, {2.0, std::polar(0.2, 0.6), 20.0}};
    n.noiseCorrelation = {CMatrix::Identity(2, 2), 3.0 * CMatrix::Identity(2, 2), CMatrix::Identity(2, 2),
                          CMatrix::Identity(2, 2)};
    QString error, warning;
    const auto r = interpolate(n, {1.5e9, 2.5e9}, Extrapolation::Error, &error, &warning);
    REQUIRE(r);
    REQUIRE(r->noise.size() == 1); // 1.5 GHz is outside the noise data
    CHECK(r->noiseFrequencies.front() == 2.5e9);
    CHECK_THAT(r->noise.front().fminDb, WithinRel(1.5, 1e-14));
    CHECK_THAT(r->noise.front().rn, WithinRel(15.0, 1e-14));
    CHECK(std::abs(r->noise.front().gammaOpt - std::polar(0.3, 0.4)) < 1e-14);
    CHECK_THAT(r->noiseCorrelation[0](0, 0).real(), WithinRel(2.0, 1e-14));
    CHECK_THAT(warning.toStdString(), ContainsSubstring("no noise parameters"));
    const auto held = interpolate(n, {1.5e9, 2.5e9}, Extrapolation::Hold, &error, &warning);
    REQUIRE(held);
    CHECK(held->noise.size() == 2);
    CHECK(held->noise.front() == n.noise.front());
}

TEST_CASE("Renormalisation and port renumbering", "[rfdata][ops]")
{
    const QJsonObject data = loadJson(QStringLiteral("conversions.json"));
    for (const QJsonValue &item : data.value(QStringLiteral("cases")).toArray()) {
        const QJsonObject c = item.toObject();
        const NetworkData n = network({1e9}, {toMatrix(c.value(QStringLiteral("s")))}, toDoubles(c.value(QStringLiteral("z0"))));
        const NetworkData r = renormalized(n, toDoubles(c.value(QStringLiteral("z0New"))));
        CHECK(r.z0 == toDoubles(c.value(QStringLiteral("z0New"))));
        CHECK(test::maxDifference(r.s.front(), toMatrix(c.value(QStringLiteral("sRenormalized")))) < 1e-10);
    }

    CMatrix s3(3, 3);
    s3 << 0.11, 0.12, 0.13, 0.21, 0.22, 0.23, 0.31, 0.32, 0.33;
    NetworkData n = network({1e9}, {s3}, {10, 20, 30});
    n.noiseCorrelation = {s3};
    QString error;
    const auto r = renumberPorts(n, {2, 0, 1}, &error);
    REQUIRE(r);
    CHECK(r->z0 == std::vector<double>{30, 10, 20});
    CHECK(r->s.front()(0, 0) == Complex(0.33, 0));
    CHECK(r->s.front()(0, 1) == Complex(0.31, 0));
    CHECK(r->s.front()(1, 2) == Complex(0.12, 0));
    CHECK(r->noiseCorrelation.front() == r->s.front());
    CHECK_FALSE(renumberPorts(n, {0, 0, 1}, &error));
    CHECK_FALSE(renumberPorts(n, {0, 1}, &error));
    CHECK_FALSE(renumberPorts(n, {0, 1, 3}, &error));

    // Swapping a two-port's ports twice restores its noise parameters.
    NetworkData amp = network({1e9, 2e9}, {twoPort({0.5, -0.2}, {0.05, 0.02}, {3.0, 1.0}, {0.4, 0.1}),
                                           twoPort({0.4, -0.3}, {0.06, 0.03}, {2.5, 1.5}, {0.3, 0.2})},
                              {50, 50});
    amp.noiseFrequencies = {1e9, 2e9};
    amp.noise = {{1.2, std::polar(0.4, 0.8), 15.0}, {1.6, std::polar(0.35, 1.2), 12.0}};
    const auto swapped = renumberPorts(amp, {1, 0}, &error);
    REQUIRE(swapped);
    CHECK(std::abs(swapped->noise.front().fminDb - amp.noise.front().fminDb) > 0.1);
    const auto back = renumberPorts(*swapped, {1, 0}, &error);
    REQUIRE(back);
    for (size_t k = 0; k < amp.noise.size(); ++k) {
        CHECK_THAT(back->noise[k].fminDb, WithinRel(amp.noise[k].fminDb, 1e-9));
        CHECK_THAT(back->noise[k].rn, WithinRel(amp.noise[k].rn, 1e-9));
        CHECK(std::abs(back->noise[k].gammaOpt - amp.noise[k].gammaOpt) < 1e-9);
    }

    // gammaOpt follows the port 1 reference; Fmin and Rn do not change.
    const NetworkData r75 = renormalized(amp, {75, 50});
    CHECK(r75.noise.front().fminDb == amp.noise.front().fminDb);
    const Complex zOpt = impedanceFromReflection(amp.noise.front().gammaOpt, 50);
    CHECK(std::abs(impedanceFromReflection(r75.noise.front().gammaOpt, 75) - zOpt) < 1e-9);
    const NetworkData back50 = renormalized(r75, {50, 50});
    CHECK(test::maxDifference(back50.s.front(), amp.s.front()) < 1e-12);
}

TEST_CASE("Cascade and de-embedding agree with scikit-rf", "[rfdata][ops]")
{
    const QJsonObject data = loadJson(QStringLiteral("cascade.json"));
    double worst = 0;
    for (const QJsonValue &item : data.value(QStringLiteral("cascade")).toArray()) {
        const QJsonObject c = item.toObject();
        const NetworkData a = fromJson(c, "f", "a", "za");
        const NetworkData b = fromJson(c, "f", "b", "zb");
        QString error;
        const auto ab = cascade(a, b, &error);
        REQUIRE(ab);
        CHECK(ab->z0 == toDoubles(c.value(QStringLiteral("zab"))));
        const double difference = maxDifference(ab->s, toMatrices(c.value(QStringLiteral("ab"))));
        worst = std::max(worst, difference);
        CHECK(difference < 1e-12);
    }
    for (const QJsonValue &item : data.value(QStringLiteral("deembed")).toArray()) {
        const QJsonObject c = item.toObject();
        const NetworkData measured = fromJson(c, "f", "measured", "zm");
        const NetworkData left = fromJson(c, "f", "left", "zl");
        const NetworkData right = fromJson(c, "f", "right", "zr");
        QString error;
        const auto dut = deembed(measured, left, right, &error);
        REQUIRE(dut);
        CHECK(dut->z0 == toDoubles(c.value(QStringLiteral("zd"))));
        const double difference = maxDifference(dut->s, toMatrices(c.value(QStringLiteral("dut"))));
        worst = std::max(worst, difference);
        CHECK(difference < 1e-10);

        // One side at a time
        const auto leftOnly = deembed(measured, left, std::nullopt, &error);
        REQUIRE(leftOnly);
        const auto again = deembed(*leftOnly, std::nullopt, right, &error);
        REQUIRE(again);
        CHECK(maxDifference(again->s, dut->s) < 1e-10);
        const auto rebuilt = cascade(*cascade(left, *dut), right);
        REQUIRE(rebuilt);
        CHECK(maxDifference(rebuilt->s, measured.s) < 1e-10);
    }
    std::printf("rfdata reference: largest cascade/de-embedding deviation from scikit-rf %.2e\n", worst);
}

TEST_CASE("Cascade checks its inputs", "[rfdata][ops]")
{
    const NetworkData a = attenuator(0.5, {1e9, 2e9, 3e9});
    const NetworkData narrow = attenuator(0.5, {1.5e9, 2e9});
    QString error;
    CHECK_FALSE(cascade(a, narrow, &error));
    CHECK_THAT(error.toStdString(), ContainsSubstring("Second network"));
    const auto ok = cascade(narrow, a, &error); // b is interpolated onto a's grid
    REQUIRE(ok);
    CHECK(ok->frequencies == narrow.frequencies);
    CHECK_THAT(ok->s.front()(1, 0).real(), WithinRel(0.25, 1e-14));
    const NetworkData onePort = network({1e9}, {CMatrix::Constant(1, 1, 0.1)}, {50});
    CHECK_FALSE(cascade(onePort, a, &error));
    CHECK_FALSE(deembed(onePort, std::nullopt, std::nullopt, &error));
    const NetworkData isolation = network({1e9, 2e9, 3e9}, std::vector<CMatrix>(3, twoPort(0.2, 0, 0, 0.1)), {50, 50});
    CHECK_FALSE(deembed(a, isolation, std::nullopt, &error));
    const NetworkData shortCircuit = network({1e9, 2e9, 3e9}, std::vector<CMatrix>(3, twoPort(0, 0, 0, -1)), {50, 50});
    const NetworkData shortB = network({1e9, 2e9, 3e9}, std::vector<CMatrix>(3, twoPort(-1, 0, 0, 0)), {50, 50});
    CHECK_FALSE(cascade(shortCircuit, shortB, &error));
}

TEST_CASE("Passivity and reciprocity", "[rfdata][ops]")
{
    CHECK(isPassive(twoPort(0, 1, 1, 0)));
    CHECK(isPassive(twoPort(0.6, 0.8, 0.8, -0.6)));
    CHECK_FALSE(isPassive(twoPort(0.6, 0.8, 0.8, 0.6)));
    CHECK_FALSE(isPassive(twoPort(0.1, 0.01, 2.0, 0.2)));
    CHECK(isReciprocal(twoPort(0.1, 0.3, 0.3, 0.2)));
    CHECK_FALSE(isReciprocal(twoPort(0.1, 0.3, 0.31, 0.2)));
    CHECK(isReciprocal(twoPort(0.1, 0.3, 0.31, 0.2), 0.02));
    const NetworkData n = network({1, 2, 3}, {twoPort(0, 1, 1, 0), twoPort(0.1, 0.01, 2.0, 0.2), twoPort(0.1, 0.3, 0.31, 0.2)},
                                  {50, 50});
    CHECK(nonPassiveFrequencies(n) == std::vector<int>{1});
    CHECK(nonReciprocalFrequencies(n) == std::vector<int>{1, 2});
}

// K, Delta and MAG are closed-form in scikit-rf; mu and mu' are evaluated
// there from the Edwards-Sinsky formulas: agreement to rounding.
TEST_CASE("Stability factors and maximum gain agree with scikit-rf", "[rfdata][ops]")
{
    const QJsonObject data = loadJson(QStringLiteral("stability.json"));
    int stable = 0;
    for (const QJsonValue &item : data.value(QStringLiteral("cases")).toArray()) {
        const QJsonObject c = item.toObject();
        const CMatrix s = toMatrix(c.value(QStringLiteral("s")));
        const double k = c.value(QStringLiteral("k")).toDouble();
        CHECK_THAT(stabilityK(s), WithinRel(k, 1e-12));
        CHECK(std::abs(stabilityDelta(s) - toComplex(c.value(QStringLiteral("delta")))) < 1e-14);
        CHECK_THAT(stabilityMu(s), WithinRel(c.value(QStringLiteral("mu")).toDouble(), 1e-12));
        CHECK_THAT(stabilityMuPrime(s), WithinRel(c.value(QStringLiteral("muPrime")).toDouble(), 1e-12));
        CHECK_THAT(maximumAvailableGainDb(s), WithinAbs(c.value(QStringLiteral("magDb")).toDouble(), 1e-11));
        // mu > 1 exactly when the two-port is unconditionally stable (K > 1 and |Delta| < 1)
        const bool unconditional = k > 1 && std::abs(stabilityDelta(s)) < 1;
        CHECK((stabilityMu(s) > 1) == unconditional);
        CHECK((stabilityMuPrime(s) > 1) == unconditional);
        stable += unconditional ? 1 : 0;
    }
    CHECK(stable > 0);
    CHECK(stable < 12);

    // unilateral two-port
    const CMatrix u = twoPort(0.5, 0, 3.0, 0.4);
    CHECK(std::isinf(stabilityK(u)));
    CHECK_THAT(maximumAvailableGainDb(u), WithinRel(10 * std::log10(9.0 / (0.75 * 0.84)), 1e-12));
    CHECK(vswr(0.5) == 3.0);
    CHECK(std::isinf(vswr(1.0)));
}

TEST_CASE("Group delay agrees with scikit-rf", "[rfdata][ops]")
{
    const QJsonObject data = loadJson(QStringLiteral("groupdelay.json"));
    const std::vector<double> f = toDoubles(data.value(QStringLiteral("frequencies")));
    const NetworkData n = network(f, toMatrices(data.value(QStringLiteral("s"))), {50, 50});
    const auto check = [&](int row, int col, const char *phaseKey, const char *delayKey) {
        const std::vector<double> phase = unwrappedPhase(n, row, col);
        const std::vector<double> delay = groupDelay(n, row, col);
        const std::vector<double> phaseExpected = toDoubles(data.value(QLatin1String(phaseKey)));
        const std::vector<double> delayExpected = toDoubles(data.value(QLatin1String(delayKey)));
        REQUIRE(phase.size() == phaseExpected.size());
        REQUIRE(delay.size() == delayExpected.size());
        for (size_t k = 0; k < phase.size(); ++k) {
            CHECK_THAT(phase[k], WithinAbs(phaseExpected[k], 1e-12));
            CHECK_THAT(delay[k], WithinAbs(delayExpected[k], 1e-6 * std::abs(delayExpected[k]) + 1e-18));
        }
    };
    check(1, 0, "phase21", "delay21");
    check(0, 0, "phase11", "delay11");

    // A pure delay on an uneven grid
    std::vector<CMatrix> s;
    for (double x : f)
        s.push_back(twoPort(0, 0, std::polar(1.0, -2 * Pi * x * 1.234e-9), 0));
    const std::vector<double> delay = groupDelay(network(f, s, {50, 50}), 1, 0);
    for (double d : delay)
        CHECK_THAT(d, WithinRel(1.234e-9, 1e-9));
    CHECK(groupDelay(network({1e9}, {twoPort(0, 0, 1, 0)}, {50, 50}), 1, 0) == std::vector<double>{0.0});
}

// Noise figure: scikit-rf evaluates F = Fmin + Rn/Gs |Ys - Yopt|^2 from the
// same parameters (agreement to rounding). The cascade goes through the noise
// wave correlation here and through the ABCD correlation in scikit-rf, an
// independent check of the Hillbrand-Russer transformation.
TEST_CASE("Noise parameters, noise figure and noisy cascade agree with scikit-rf", "[rfdata][ops][noise]")
{
    const QJsonObject data = loadJson(QStringLiteral("noise.json"));
    const std::vector<double> f = toDoubles(data.value(QStringLiteral("frequencies")));
    const double z0 = data.value(QStringLiteral("z0")).toDouble();
    auto load = [&](const char *key) {
        const QJsonObject o = data.value(QLatin1String(key)).toObject();
        NetworkData n = network(f, toMatrices(o.value(QStringLiteral("s"))), {z0, z0});
        n.noiseFrequencies = f;
        const std::vector<double> fmin = toDoubles(o.value(QStringLiteral("fminDb")));
        const std::vector<Complex> gamma = toComplexes(o.value(QStringLiteral("gammaOpt")));
        const std::vector<double> rn = toDoubles(o.value(QStringLiteral("rn")));
        for (size_t k = 0; k < f.size(); ++k)
            n.noise.push_back({fmin[k], gamma[k], rn[k]});
        return n;
    };
    const NetworkData a = load("a"), b = load("b"), abExpected = load("ab");

    for (const QJsonValue &item : data.value(QStringLiteral("noiseFigure")).toArray()) {
        const QJsonObject c = item.toObject();
        const Complex gs = toComplex(c.value(QStringLiteral("gammaS")));
        const std::vector<double> nf = toDoubles(c.value(QStringLiteral("nfDb")));
        for (size_t k = 0; k < f.size(); ++k)
            CHECK_THAT(noiseFigureDb(a.noise[k], gs, z0), WithinAbs(nf[k], 1e-11));
    }
    for (size_t k = 0; k < f.size(); ++k) {
        CHECK_THAT(noiseFigureDb(a.noise[k], a.noise[k].gammaOpt, z0), WithinAbs(a.noise[k].fminDb, 1e-12));
        const CMatrix c = correlationFromNoiseParameters(a.s[k], a.noise[k], z0);
        CHECK((c - c.adjoint()).cwiseAbs().maxCoeff() < 1e-9 * c.cwiseAbs().maxCoeff());
        const NoiseParameters back = noiseParametersFromCorrelation(a.s[k], c, z0);
        CHECK_THAT(back.fminDb, WithinRel(a.noise[k].fminDb, 1e-12));
        CHECK_THAT(back.rn, WithinRel(a.noise[k].rn, 1e-12));
        CHECK(std::abs(back.gammaOpt - a.noise[k].gammaOpt) < 1e-12);
    }

    QString error;
    const auto ab = cascade(a, b, &error);
    REQUIRE(ab);
    CHECK(maxDifference(ab->s, abExpected.s) < 1e-12);
    REQUIRE(ab->hasNoiseCorrelation());
    REQUIRE(ab->noise.size() == f.size());
    double worst = 0;
    for (size_t k = 0; k < f.size(); ++k) {
        CHECK_THAT(ab->noise[k].fminDb, WithinRel(abExpected.noise[k].fminDb, 1e-9));
        CHECK_THAT(ab->noise[k].rn, WithinRel(abExpected.noise[k].rn, 1e-9));
        CHECK(std::abs(ab->noise[k].gammaOpt - abExpected.noise[k].gammaOpt) < 1e-9);
        worst = std::max({worst, relativeDifference(ab->noise[k].fminDb, abExpected.noise[k].fminDb),
                          relativeDifference(ab->noise[k].rn, abExpected.noise[k].rn)});
    }
    std::printf("rfdata reference: largest noise cascade deviation from scikit-rf %.2e\n", worst);
}

// A passive two-port at T0 has F = 1 / G_available for every source.
TEST_CASE("Thermal noise of passive networks", "[rfdata][ops][noise]")
{
    const QJsonObject data = loadJson(QStringLiteral("conversions.json"));
    for (const QJsonValue &item : data.value(QStringLiteral("cases")).toArray()) {
        const CMatrix s = toMatrix(item.toObject().value(QStringLiteral("s")));
        if (s.rows() != 2)
            continue;
        const CMatrix c = T0 * (CMatrix::Identity(2, 2) - s * s.adjoint());
        const NoiseParameters noise = noiseParametersFromCorrelation(s, c, 50);
        for (const Complex gs : {Complex(0, 0), Complex(0.3, -0.2), std::polar(0.8, 2.0)})
            CHECK_THAT(noiseFigureDb(noise, gs, 50), WithinAbs(-db10(availableGain(s, gs)), 1e-9));
        const CMatrix again = correlationFromNoiseParameters(s, noise, 50);
        CHECK(test::maxDifference(again, c, T0) < 1e-9);
    }

    // Friis: a matched attenuator without noise data is taken as thermal at T0.
    const double loss = 0.7;
    const NetworkData pad = attenuator(loss, {1e9, 2e9});
    NetworkData amp = network({1e9, 2e9}, std::vector<CMatrix>(2, twoPort({0.3, 0.1}, 0.05, {4.0, 1.0}, {0.2, -0.1})),
                              {50, 50});
    amp.noiseFrequencies = {1e9, 2e9};
    amp.noise = {{1.5, std::polar(0.3, 0.5), 20.0}, {1.5, std::polar(0.3, 0.5), 20.0}};
    QString error;
    const auto chain = cascade(pad, amp, &error);
    REQUIRE(chain);
    REQUIRE(chain->noise.size() == 2);
    const double fAmp = std::pow(10.0, noiseFigureDb(amp.noise[0], 0, 50) / 10);
    const double expected = 1 / (loss * loss) + (fAmp - 1) / (loss * loss);
    CHECK_THAT(noiseFigureDb(chain->noise[0], 0, 50), WithinAbs(db10(expected), 1e-9));
    // and the noise parameters survive a 75 ohm reference on port 2
    const auto chain75 = cascade(pad, renormalized(amp, {50, 75}), &error);
    REQUIRE(chain75);
    CHECK_THAT(chain75->noise[0].fminDb, WithinRel(chain->noise[0].fminDb, 1e-9));

    // Without noise data on either side there is no noise result.
    const auto plain = cascade(pad, pad, &error);
    REQUIRE(plain);
    CHECK_FALSE(plain->hasNoiseCorrelation());
    CHECK_FALSE(plain->hasNoiseParameters());
}

#include "ReferenceData.h"

#include "rfdata/NetworkMath.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdio>

using namespace lvd::rf;
using namespace lvd::rf::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<double> toVector(const QJsonValue &value)
{
    return toDoubles(value);
}

} // namespace

// Conversions are closed-form: agreement with scikit-rf to ~1e-12 relative
// (1e-10 allows for the conditioning of the random matrices).
TEST_CASE("S to Z, Y, ABCD, T and renormalisation agree with scikit-rf", "[rfdata][math]")
{
    const QJsonObject data = loadJson(QStringLiteral("conversions.json"));
    const QJsonArray cases = data.value(QStringLiteral("cases")).toArray();
    REQUIRE(cases.size() == 8);
    double worst = 0;
    for (const QJsonValue &item : cases) {
        const QJsonObject c = item.toObject();
        const std::vector<double> z0 = toVector(c.value(QStringLiteral("z0")));
        const CMatrix s = toMatrix(c.value(QStringLiteral("s")));
        INFO("ports " << s.rows());

        const auto z = sToZ(s, z0);
        REQUIRE(z);
        const CMatrix zExpected = toMatrix(c.value(QStringLiteral("z")));
        worst = std::max(worst, maxDifference(*z, zExpected, zExpected.cwiseAbs().maxCoeff()));
        CHECK(maxDifference(*z, zExpected, zExpected.cwiseAbs().maxCoeff()) < 1e-10);
        CHECK(maxDifference(zToS(*z, z0), s) < 1e-10);

        const auto y = sToY(s, z0);
        REQUIRE(y);
        const CMatrix yExpected = toMatrix(c.value(QStringLiteral("y")));
        CHECK(maxDifference(*y, yExpected, yExpected.cwiseAbs().maxCoeff()) < 1e-10);
        CHECK(maxDifference(yToS(*y, z0), s) < 1e-10);

        const std::vector<double> z0New = toVector(c.value(QStringLiteral("z0New")));
        const CMatrix sNew = renormalize(s, z0, z0New);
        CHECK(maxDifference(sNew, toMatrix(c.value(QStringLiteral("sRenormalized")))) < 1e-10);
        CHECK(maxDifference(renormalize(sNew, z0New, z0), s) < 1e-10);

        if (s.rows() == 2) {
            const auto abcd = sToAbcd(s, z0[0], z0[1]);
            REQUIRE(abcd);
            const CMatrix abcdExpected = toMatrix(c.value(QStringLiteral("abcd")));
            CHECK(maxDifference(*abcd, abcdExpected, abcdExpected.cwiseAbs().maxCoeff()) < 1e-10);
            CHECK(maxDifference(abcdToS(*abcd, z0[0], z0[1]), s) < 1e-10);
            const auto t = sToT(s);
            REQUIRE(t);
            const CMatrix tExpected = toMatrix(c.value(QStringLiteral("t")));
            CHECK(maxDifference(*t, tExpected, tExpected.cwiseAbs().maxCoeff()) < 1e-10);
            CHECK(maxDifference(tToS(*t), s) < 1e-10);
        }
    }
    std::printf("rfdata reference: largest relative deviation of Z from scikit-rf %.2e\n", worst);
}

TEST_CASE("Singular conversions return nullopt", "[rfdata][math]")
{
    CMatrix thru(2, 2);
    thru << 0, 1, 1, 0;
    CHECK_FALSE(sToZ(thru, {50, 50}));
    CHECK_FALSE(sToY(thru, {50, 50}));
    CMatrix open = CMatrix::Identity(1, 1);
    CHECK_FALSE(sToZ(open, {50}));
    CHECK(sToY(open, {50}));
    CMatrix isolator(2, 2);
    isolator << 0, 0.5, 0, 0;
    CHECK_FALSE(sToAbcd(isolator, 50, 50));
    CHECK_FALSE(sToT(isolator));
}

// A passive network in thermal equilibrium at T0 has the noise wave
// correlation T0 (I - S S^H) in every reference system, so the noise wave
// transformation must map one into the other.
TEST_CASE("Noise wave renormalisation preserves thermal equilibrium", "[rfdata][math]")
{
    const QJsonObject data = loadJson(QStringLiteral("conversions.json"));
    for (const QJsonValue &item : data.value(QStringLiteral("cases")).toArray()) {
        const QJsonObject c = item.toObject();
        const std::vector<double> z0 = toVector(c.value(QStringLiteral("z0")));
        const std::vector<double> z0New = toVector(c.value(QStringLiteral("z0New")));
        const CMatrix s = toMatrix(c.value(QStringLiteral("s")));
        const CMatrix identity = CMatrix::Identity(s.rows(), s.cols());
        const CMatrix cOld = T0 * (identity - s * s.adjoint());
        const CMatrix sNew = renormalize(s, z0, z0New);
        const CMatrix t = noiseRenormalization(s, z0, z0New);
        const CMatrix cNew = t * cOld * t.adjoint();
        CHECK(maxDifference(cNew, T0 * (identity - sNew * sNew.adjoint()), T0) < 1e-10);
    }
}

TEST_CASE("Reflection and dB helpers", "[rfdata][math]")
{
    CHECK_THAT(std::abs(reflectionFromImpedance(50, 50)), WithinAbs(0, 1e-15));
    CHECK_THAT(reflectionFromImpedance(150, 50).real(), WithinAbs(0.5, 1e-15));
    const Complex z(30, -20);
    CHECK_THAT(std::abs(impedanceFromReflection(reflectionFromImpedance(z, 75), 75) - z), WithinAbs(0, 1e-12));
    CHECK(std::isinf(impedanceFromReflection(1.0, 50).real()));
    CHECK_THAT(db20(Complex(0.1, 0)), WithinRel(-20.0, 1e-12));
    CHECK_THAT(db10(100.0), WithinRel(20.0, 1e-12));
    CHECK(std::isinf(db20(0.0)));
    const Complex v = fromDbPhase(-6.0, 45.0);
    CHECK_THAT(db20(v), WithinRel(-6.0, 1e-12));
    CHECK_THAT(std::arg(v) * 180 / Pi, WithinRel(45.0, 1e-12));
}

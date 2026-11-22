#include "ReferenceData.h"

#include "rfdata/LineModels.h"
#include "rfdata/NetworkMath.h"

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

struct Deviation {
    double z0 = 0, eeff = 0, alphaC = 0, alphaD = 0, beta = 0;
    void update(double &worst, double value, double reference) { worst = std::max(worst, relativeDifference(value, reference)); }
    void print(const char *name) const
    {
        std::printf("rfdata reference: %s largest relative deviation from scikit-rf: Z0 %.3e, eeff %.3e, alphaC %.3e, "
                    "alphaD %.3e, beta %.3e\n",
                    name, z0, eeff, alphaC, alphaD, beta);
    }
};

// Tolerances. The implementation uses the same formula set as scikit-rf
// (named in LineModels.cpp), so the quasi-static and dispersive Z0 and
// effective permittivity agree to rounding except for the CPW, where
// scikit-rf approximates K(k)/K'(k) by Hilberg's formula (2 ppm). The
// attenuation constants agree to rounding as well. The limits below are
// therefore far tighter than the 0.5 % / 5 % that legitimate formula variants
// would need, and catch any drift in the formulas.
// Exception: scikit-rf evaluates the quasi-static formulas with the complex
// permittivity er (1 - j tand) when tand > 0, which moves Re(eeff) and hence
// the dielectric loss by O(tand^2): up to 5e-5 for tand = 0.02.
// For the CPW conductor loss, the expected value corrects a scikit-rf 2.2 bug
// (scipy's ellipk called with the modulus instead of the parameter k^2),
// which makes scikit-rf's own value 20 % to 25 % too low; see
// tools/generate_rf_reference.py.
constexpr double ToleranceZ0 = 1e-5;
constexpr double ToleranceLoss = 1e-5;
constexpr double ToleranceDielectricLoss = 1e-4;

} // namespace

TEST_CASE("Microstrip agrees with scikit-rf MLine (Hammerstad-Jensen, Kirschning-Jansen)", "[rfdata][lines]")
{
    const QJsonObject data = loadJson(QStringLiteral("lines.json"));
    Deviation worst;
    for (const QJsonValue &item : data.value(QStringLiteral("microstrip")).toArray()) {
        const QJsonObject c = item.toObject();
        MicrostripGeometry g;
        g.w = c.value(QStringLiteral("w")).toDouble();
        g.h = c.value(QStringLiteral("h")).toDouble();
        g.t = c.value(QStringLiteral("t")).toDouble();
        g.er = c.value(QStringLiteral("er")).toDouble();
        const double tand = c.value(QStringLiteral("tand")).toDouble();
        const double sigma = c.value(QStringLiteral("sigma")).toDouble();
        const std::vector<double> f = toDoubles(c.value(QStringLiteral("frequencies")));
        const std::vector<double> z0 = toDoubles(c.value(QStringLiteral("z0")));
        const std::vector<double> eeff = toDoubles(c.value(QStringLiteral("eeff")));
        const std::vector<double> alphaC = toDoubles(c.value(QStringLiteral("alphaC")));
        const std::vector<double> alphaD = toDoubles(c.value(QStringLiteral("alphaD")));
        for (size_t k = 0; k < f.size(); ++k) {
            INFO("w " << g.w << " er " << g.er << " f " << f[k]);
            const auto lossless = microstrip(g, f[k]);
            REQUIRE(lossless);
            CHECK(lossless->zc.imag() == 0);
            CHECK_THAT(lossless->zc.real(), WithinRel(z0[k], ToleranceZ0));
            CHECK_THAT(lossless->epsilonEffective, WithinRel(eeff[k], ToleranceZ0));
            worst.update(worst.z0, lossless->zc.real(), z0[k]);
            worst.update(worst.eeff, lossless->epsilonEffective, eeff[k]);
            worst.update(worst.beta, lossless->gamma.imag(), 2 * Pi * f[k] * std::sqrt(eeff[k]) / SpeedOfLight);

            MicrostripGeometry lossy = g;
            lossy.sigma = sigma;
            lossy.tand = tand;
            const auto p = microstrip(lossy, f[k]);
            REQUIRE(p);
            CHECK_THAT(p->alphaConductor, WithinRel(alphaC[k], ToleranceLoss));
            CHECK_THAT(p->alphaDielectric, WithinRel(alphaD[k], ToleranceDielectricLoss));
            worst.update(worst.alphaC, p->alphaConductor, alphaC[k]);
            worst.update(worst.alphaD, p->alphaDielectric, alphaD[k]);
            // gamma = alpha + j beta up to second order in alpha/beta
            const double beta = 2 * Pi * f[k] * std::sqrt(eeff[k]) / SpeedOfLight;
            CHECK_THAT(p->gamma.imag(), WithinRel(beta, 1e-3));
            CHECK_THAT(p->gamma.real(), WithinRel(alphaC[k] + alphaD[k], 1e-3));
        }
    }
    worst.print("microstrip");
}

// Both use the same per-unit-length model with Schelkunoff's conductor
// impedances (exact Bessel functions in scipy, a continued fraction and an
// asymptotic series here), so zc and gamma agree to rounding. With only the
// many-skin-depths limit (1 + j) Rs the attenuation would deviate by up to
// 1.7 % (din = 0.5 mm, sigma = 1.5e7 S/m at 100 MHz).
TEST_CASE("Coax agrees with scikit-rf Coaxial", "[rfdata][lines]")
{
    const QJsonObject data = loadJson(QStringLiteral("lines.json"));
    Deviation worst;
    for (const QJsonValue &item : data.value(QStringLiteral("coax")).toArray()) {
        const QJsonObject c = item.toObject();
        CoaxGeometry g;
        g.din = c.value(QStringLiteral("din")).toDouble();
        g.dout = c.value(QStringLiteral("dout")).toDouble();
        g.er = c.value(QStringLiteral("er")).toDouble();
        g.tand = c.value(QStringLiteral("tand")).toDouble();
        g.sigma = c.value(QStringLiteral("sigma")).toDouble();
        const std::vector<double> f = toDoubles(c.value(QStringLiteral("frequencies")));
        const std::vector<Complex> zc = toComplexes(c.value(QStringLiteral("zc")));
        const std::vector<Complex> gamma = toComplexes(c.value(QStringLiteral("gamma")));
        for (size_t k = 0; k < f.size(); ++k) {
            INFO("din " << g.din << " f " << f[k]);
            const auto p = coax(g, f[k]);
            REQUIRE(p);
            CHECK(std::abs(p->zc - zc[k]) / std::abs(zc[k]) < 1e-6);
            CHECK_THAT(p->gamma.imag(), WithinRel(gamma[k].imag(), 1e-6));
            CHECK_THAT(p->gamma.real(), WithinRel(gamma[k].real(), 1e-6));
            worst.update(worst.z0, std::abs(p->zc), std::abs(zc[k]));
            worst.update(worst.beta, p->gamma.imag(), gamma[k].imag());
            worst.update(worst.alphaC, p->gamma.real(), gamma[k].real());
        }
    }
    worst.print("coax (alphaC column: total alpha)");

    CoaxGeometry g{1e-3, 3.3e-3, 2.1, 0, 0};
    const auto p = coax(g, 1e9);
    REQUIRE(p);
    CHECK_THAT(p->zc.real(), WithinRel(59.9585 / std::sqrt(2.1) * std::log(3.3), 1e-4));
    CHECK(p->gamma.real() == 0);
    const auto dc = coax({1e-3, 3.3e-3, 2.1, 0.01, 5.8e7}, 0);
    REQUIRE(dc);
    CHECK(dc->gamma == Complex(0, 0));
}

TEST_CASE("CPW agrees with scikit-rf CPW (with and without backing)", "[rfdata][lines]")
{
    const QJsonObject data = loadJson(QStringLiteral("lines.json"));
    Deviation worst;
    for (const QJsonValue &item : data.value(QStringLiteral("cpw")).toArray()) {
        const QJsonObject c = item.toObject();
        CpwGeometry g;
        g.w = c.value(QStringLiteral("w")).toDouble();
        g.s = c.value(QStringLiteral("s")).toDouble();
        g.h = c.value(QStringLiteral("h")).toDouble();
        g.t = c.value(QStringLiteral("t")).toDouble();
        g.er = c.value(QStringLiteral("er")).toDouble();
        g.backed = c.value(QStringLiteral("backed")).toBool();
        const double tand = c.value(QStringLiteral("tand")).toDouble();
        const double sigma = c.value(QStringLiteral("sigma")).toDouble();
        const std::vector<double> f = toDoubles(c.value(QStringLiteral("frequencies")));
        const std::vector<double> z0 = toDoubles(c.value(QStringLiteral("z0")));
        const std::vector<double> eeff = toDoubles(c.value(QStringLiteral("eeff")));
        const std::vector<double> alphaC = toDoubles(c.value(QStringLiteral("alphaC")));
        const std::vector<double> alphaD = toDoubles(c.value(QStringLiteral("alphaD")));
        for (size_t k = 0; k < f.size(); ++k) {
            INFO("w " << g.w << " backed " << g.backed << " f " << f[k]);
            const auto lossless = cpw(g, f[k]);
            REQUIRE(lossless);
            CHECK_THAT(lossless->zc.real(), WithinRel(z0[k], ToleranceZ0));
            CHECK_THAT(lossless->epsilonEffective, WithinRel(eeff[k], ToleranceZ0));
            worst.update(worst.z0, lossless->zc.real(), z0[k]);
            worst.update(worst.eeff, lossless->epsilonEffective, eeff[k]);
            worst.update(worst.beta, lossless->gamma.imag(), 2 * Pi * f[k] * std::sqrt(eeff[k]) / SpeedOfLight);
            CpwGeometry lossy = g;
            lossy.sigma = sigma;
            lossy.tand = tand;
            const auto p = cpw(lossy, f[k]);
            REQUIRE(p);
            CHECK_THAT(p->alphaConductor, WithinRel(alphaC[k], ToleranceLoss));
            CHECK_THAT(p->alphaDielectric, WithinRel(alphaD[k], ToleranceDielectricLoss));
            worst.update(worst.alphaC, p->alphaConductor, alphaC[k]);
            worst.update(worst.alphaD, p->alphaDielectric, alphaD[k]);
        }
    }
    worst.print("CPW");
}

// Zero-thickness stripline has an exact conformal-mapping solution,
// Z0 = eta0/(4 sqrt(er)) K(k)/K(k') with k = sech(pi w / 2b) (S. B. Cohn 1954); Wheeler's formula
// is stated to be within 0.5 % of it.
TEST_CASE("Stripline", "[rfdata][lines]")
{
    auto exact = [](double w, double b, double er) {
        const double k = 1.0 / std::cosh(Pi * w / (2 * b));
        auto ellipticK = [](double m) {
            double a = 1, g = std::sqrt(1 - m * m);
            for (int i = 0; i < 40; ++i) {
                const double next = (a + g) / 2;
                g = std::sqrt(a * g);
                a = next;
            }
            return Pi / (2 * a);
        };
        return 376.730313412 / (4 * std::sqrt(er)) * ellipticK(k) / ellipticK(std::sqrt(1 - k * k));
    };
    for (const double ratio : {0.1, 0.3, 0.8, 1.5, 3.0}) {
        const auto p = stripline({ratio * 1e-3, 1e-3, 0, 2.2, 0, 0}, 1e9);
        REQUIRE(p);
        CHECK_THAT(p->zc.real(), WithinRel(exact(ratio * 1e-3, 1e-3, 2.2), 5e-3));
        CHECK(p->epsilonEffective == 2.2);
    }

    // Thickness lowers the impedance.
    const auto thin = stripline({0.8e-3, 1.6e-3, 0, 4.4, 0, 0}, 1e9);
    const auto thick = stripline({0.8e-3, 1.6e-3, 35e-6, 4.4, 0, 0}, 1e9);
    REQUIRE(thin);
    REQUIRE(thick);
    CHECK(thick->zc.real() < thin->zc.real());

    // Conductor loss by incremental inductance against Pozar's closed-form
    // approximation (Microwave Engineering, eq. 3.199, within about 10 %).
    const double b = 3.2e-3, t = 0.01e-3, w = 2.66e-3, er = 2.2, sigma = 5.8e7, f = 10e9;
    const auto p = stripline({w, b, t, er, 0.001, sigma}, f);
    REQUIRE(p);
    const double rs = std::sqrt(Pi * f * 4e-7 * Pi / sigma);
    const double z0 = p->zc.real();
    REQUIRE(std::sqrt(er) * z0 < 120);
    const double a = 1 + 2 * w / (b - t) + (b + t) / (Pi * (b - t)) * std::log((2 * b - t) / t);
    const double pozar = 2.7e-3 * rs * er * z0 / (30 * Pi * (b - t)) * a;
    CHECK_THAT(p->alphaConductor, WithinRel(pozar, 0.1));
    CHECK_THAT(p->alphaDielectric, WithinRel(Pi * f * std::sqrt(er) * 0.001 / SpeedOfLight, 1e-12));
    std::printf("rfdata stripline: alphaC %.4g Np/m, Pozar approximation %.4g Np/m\n", p->alphaConductor, pozar);
}

TEST_CASE("Line models reject invalid geometry", "[rfdata][lines]")
{
    QString error;
    CHECK_FALSE(microstrip({0, 1e-3, 0, 4.4, 0, 0}, 1e9, &error));
    CHECK_THAT(error.toStdString(), ContainsSubstring("Microstrip"));
    CHECK_FALSE(microstrip({1e-3, 1e-3, -1e-6, 4.4, 0, 0}, 1e9, &error));
    CHECK_FALSE(microstrip({1e-3, 1e-3, 0, 0.5, 0, 0}, 1e9, &error));
    CHECK_FALSE(microstrip({1e-3, 1e-3, 0, 4.4, 0, 0}, -1, &error));
    CHECK_FALSE(stripline({1e-3, 1e-3, 1e-3, 4.4, 0, 0}, 1e9, &error));
    CHECK_FALSE(coax({2e-3, 1e-3, 1, 0, 0}, 1e9, &error));
    CHECK_FALSE(coax({1e-3, 2e-3, 1, -0.1, 0}, 1e9, &error));
    CHECK_FALSE(cpw({1e-3, 0, 1e-3, 0, 4.4, 0, 0}, 1e9, &error));
    CHECK_FALSE(cpw({1e-3, 0.1e-3, 1e-3, 0.2e-3, 4.4, 0, 0}, 1e9, &error));
    CHECK_THAT(error.toStdString(), ContainsSubstring("thickness"));

    // DC is allowed and lossless.
    const auto dc = microstrip({1e-3, 1e-3, 35e-6, 4.4, 0.02, 5.8e7}, 0);
    REQUIRE(dc);
    CHECK(dc->gamma == Complex(0, 0));
    CHECK(dc->zc.imag() == 0);
    const auto air = microstrip({1e-3, 1e-3, 0, 1.0, 0.01, 0}, 1e9);
    REQUIRE(air);
    CHECK_THAT(air->epsilonEffective, WithinRel(1.0, 1e-12));
}

TEST_CASE("Line ABCD matrix", "[rfdata][lines]")
{
    const double z0 = 50, f = 1e9;
    const double beta = 2 * Pi * f / SpeedOfLight;
    const double quarter = Pi / 2 / beta;
    const CMatrix abcd = lineAbcd(z0, Complex(0, beta), quarter);
    CHECK(std::abs(abcd(0, 0)) < 1e-12);
    CHECK(std::abs(abcd(0, 1) - Complex(0, z0)) < 1e-9);
    CHECK(std::abs(abcd(1, 0) - Complex(0, 1 / z0)) < 1e-12);
    CHECK(std::abs(abcd.determinant() - 1.0) < 1e-12);
    // matched lossy line: S21 = exp(-gamma l)
    const Complex gamma(0.5, beta);
    const CMatrix s = abcdToS(lineAbcd(z0, gamma, 0.3), z0, z0);
    CHECK(std::abs(s(0, 0)) < 1e-12);
    CHECK(std::abs(s(1, 0) - std::exp(-gamma * 0.3)) < 1e-12);
}

// The defaults of the line components in libraries/rf-circuit.lvdlib.
TEST_CASE("The library's default line geometries are close to 50 ohm", "[rfdata][lines]")
{
    const double f = 1e9;
    const auto ms = microstrip({1.5e-3, 0.8e-3, 35e-6, 4.4, 0.02, 58e6}, f);
    const auto co = coax({0.51e-3, 1.68e-3, 2.1, 0.0002, 58e6}, f);
    REQUIRE(ms);
    REQUIRE(co);
    const auto sl = stripline({0.65e-3, 1.6e-3, 35e-6, 4.4, 0.02, 58e6}, f);
    REQUIRE(sl);
    CHECK_THAT(ms->zc.real(), WithinAbs(50, 2));
    CHECK_THAT(sl->zc.real(), WithinAbs(50, 2));
    CHECK_THAT(co->zc.real(), WithinAbs(50, 2));
}

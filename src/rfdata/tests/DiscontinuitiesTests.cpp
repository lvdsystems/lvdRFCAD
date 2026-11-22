#include "ReferenceData.h"

#include "rfdata/Discontinuities.h"
#include "rfdata/LineModels.h"
#include "rfdata/NetworkMath.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdio>

using namespace lvd::rf;
using namespace lvd::rf::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Surface roughness: the conductor loss of scikit-rf's MLine with rough > 0
// (same Hammerstad-Jensen roughness factor), to rounding.
TEST_CASE("Microstrip roughness agrees with scikit-rf MLine", "[rfdata][lines]")
{
    const QJsonObject data = loadJson(QStringLiteral("roughness.json"));
    double worst = 0;
    for (const QJsonValue &item : data.value(QStringLiteral("microstrip")).toArray()) {
        const QJsonObject c = item.toObject();
        MicrostripGeometry g;
        g.w = c.value(QStringLiteral("w")).toDouble();
        g.h = c.value(QStringLiteral("h")).toDouble();
        g.t = c.value(QStringLiteral("t")).toDouble();
        g.er = c.value(QStringLiteral("er")).toDouble();
        g.sigma = c.value(QStringLiteral("sigma")).toDouble();
        g.rough = c.value(QStringLiteral("rough")).toDouble();
        const std::vector<double> f = toDoubles(c.value(QStringLiteral("frequencies")));
        const std::vector<double> alphaC = toDoubles(c.value(QStringLiteral("alphaC")));
        for (size_t k = 0; k < f.size(); ++k) {
            const auto p = microstrip(g, f[k]);
            REQUIRE(p);
            CHECK_THAT(p->alphaConductor, WithinRel(alphaC[k], 1e-5));
            worst = std::max(worst, relativeDifference(p->alphaConductor, alphaC[k]));
        }
    }
    std::printf("rfdata reference: rough microstrip alphaC largest relative deviation from scikit-rf %.3e\n", worst);

    // The factor itself: 1 for smooth, about 2 for very rough, (1 + 2/pi atan 1.4) at rough = delta.
    CHECK(roughnessFactor(0, 1e9, 5.8e7) == 1.0);
    CHECK(roughnessFactor(1e-6, 1e9, 0) == 1.0);
    const double delta = 1 / std::sqrt(Pi * 1e9 * 1.25663706127e-6 * 5.8e7);
    CHECK_THAT(roughnessFactor(delta, 1e9, 5.8e7), WithinRel(1 + 2 / Pi * std::atan(1.4), 1e-12));
    CHECK_THAT(roughnessFactor(100e-6, 10e9, 5.8e7), WithinAbs(2.0, 1e-3));
    // Stripline and CPW scale their conductor loss by the same factor.
    StriplineGeometry sl{0.65e-3, 1.6e-3, 35e-6, 4.4, 0.02, 5.8e7, 0};
    StriplineGeometry slRough = sl;
    slRough.rough = 2e-6;
    CHECK_THAT(stripline(slRough, 5e9)->alphaConductor,
               WithinRel(stripline(sl, 5e9)->alphaConductor * roughnessFactor(2e-6, 5e9, 5.8e7), 1e-12));
    CpwGeometry cp{1e-3, 0.2e-3, 0.8e-3, 35e-6, 4.4, 0.02, 5.8e7};
    CpwGeometry cpRough = cp;
    cpRough.rough = 2e-6;
    CHECK_THAT(cpw(cpRough, 5e9)->alphaConductor,
               WithinRel(cpw(cp, 5e9)->alphaConductor * roughnessFactor(2e-6, 5e9, 5.8e7), 1e-12));
    QString error;
    MicrostripGeometry bad{1e-3, 1e-3, 0, 4.4, 0, 5.8e7, -1e-6};
    CHECK_FALSE(microstrip(bad, 1e9, &error));
}

// Kirschning-Jansen against a 2D finite-difference Laplace solution
// (coupled_microstrip.json, quasi-static, zero thickness). Kirschning and
// Jansen state about 1 % for the impedances and permittivities in
// 0.1 <= u <= 10, 0.1 <= g <= 10, er <= 18; the observed deviations are
// printed (largest 0.57 % on Z, 0.32 % on eeff when written). The
// tolerances (1 % on Z0e/Z0o, 0.75 % on the permittivities) leave a margin
// over that and the solver's estimated 0.1 % error.
TEST_CASE("Coupled microstrip agrees with a finite-difference field solution", "[rfdata][lines]")
{
    const QJsonObject data = loadJson(QStringLiteral("coupled_microstrip.json"));
    double worstZ = 0, worstE = 0;
    for (const QJsonValue &item : data.value(QStringLiteral("coupled")).toArray()) {
        const QJsonObject c = item.toObject();
        CoupledMicrostripGeometry g;
        g.w = c.value(QStringLiteral("w")).toDouble();
        g.s = c.value(QStringLiteral("s")).toDouble();
        g.h = c.value(QStringLiteral("h")).toDouble();
        g.er = c.value(QStringLiteral("er")).toDouble();
        INFO("u " << g.w / g.h << " g " << g.s / g.h << " er " << g.er);
        const auto p = coupledMicrostrip(g, 1e3);
        REQUIRE(p);
        const double z0e = c.value(QStringLiteral("z0e")).toDouble(), z0o = c.value(QStringLiteral("z0o")).toDouble();
        const double ee = c.value(QStringLiteral("eeffEven")).toDouble(), eo = c.value(QStringLiteral("eeffOdd")).toDouble();
        CHECK_THAT(p->even.zc.real(), WithinRel(z0e, 0.01));
        CHECK_THAT(p->odd.zc.real(), WithinRel(z0o, 0.01));
        CHECK_THAT(p->even.epsilonEffective, WithinRel(ee, 0.0075));
        CHECK_THAT(p->odd.epsilonEffective, WithinRel(eo, 0.0075));
        std::printf("rfdata coupled microstrip u %.2g g %.2g er %.3g: Z0e %.3f (FD %.3f), Z0o %.3f (FD %.3f), "
                    "ee %.4f (FD %.4f), eo %.4f (FD %.4f)\n",
                    g.w / g.h, g.s / g.h, g.er, p->even.zc.real(), z0e, p->odd.zc.real(), z0o,
                    p->even.epsilonEffective, ee, p->odd.epsilonEffective, eo);
        worstZ = std::max({worstZ, relativeDifference(p->even.zc.real(), z0e), relativeDifference(p->odd.zc.real(), z0o)});
        worstE = std::max({worstE, relativeDifference(p->even.epsilonEffective, ee),
                           relativeDifference(p->odd.epsilonEffective, eo)});
    }
    std::printf("rfdata reference: coupled microstrip largest relative deviation from the FD solution: Z %.3e, eeff %.3e\n",
                worstZ, worstE);
    // The same solver for a single strip agrees with Hammerstad-Jensen.
    for (const QJsonValue &item : data.value(QStringLiteral("single")).toArray()) {
        const QJsonObject c = item.toObject();
        const auto p = microstrip({c.value(QStringLiteral("w")).toDouble(), c.value(QStringLiteral("h")).toDouble(), 0,
                                   c.value(QStringLiteral("er")).toDouble(), 0, 0},
                                  1e3);
        REQUIRE(p);
        CHECK_THAT(p->zc.real(), WithinRel(c.value(QStringLiteral("z0")).toDouble(), 3e-3));
        CHECK_THAT(p->epsilonEffective, WithinRel(c.value(QStringLiteral("eeff")).toDouble(), 3e-3));
    }

    // Widely spaced lines approach the single line; dispersion raises both
    // permittivities towards er; losses are positive.
    CoupledMicrostripGeometry far{0.6e-3, 6e-3, 0.635e-3, 0, 9.8, 0, 0};
    const auto single = microstrip({0.6e-3, 0.635e-3, 0, 9.8, 0, 0}, 1e3);
    const auto apart = coupledMicrostrip(far, 1e3);
    REQUIRE(apart);
    CHECK_THAT(apart->even.zc.real(), WithinRel(single->zc.real(), 0.01));
    CHECK_THAT(apart->odd.zc.real(), WithinRel(single->zc.real(), 0.01));
    CoupledMicrostripGeometry g{0.6e-3, 0.25e-3, 0.635e-3, 5e-6, 9.8, 0.0005, 5.8e7, 0};
    const auto low = coupledMicrostrip(g, 1e9), high = coupledMicrostrip(g, 20e9);
    REQUIRE(low);
    REQUIRE(high);
    CHECK(high->even.epsilonEffective > low->even.epsilonEffective);
    CHECK(high->odd.epsilonEffective > low->odd.epsilonEffective);
    CHECK(high->even.epsilonEffective < 9.8);
    CHECK(low->even.zc.real() > low->odd.zc.real());
    CHECK(low->even.alphaConductor > 0);
    CHECK(low->odd.alphaDielectric > 0);
    QString error;
    CHECK_FALSE(coupledMicrostrip({0.6e-3, 0, 0.635e-3, 0, 9.8, 0, 0}, 1e9, &error));
}

namespace {

MicrostripSubstrate discontinuitySubstrate(const QJsonObject &data)
{
    MicrostripSubstrate s;
    s.h = data.value(QStringLiteral("h")).toDouble();
    s.t = data.value(QStringLiteral("t")).toDouble();
    s.er = data.value(QStringLiteral("er")).toDouble();
    return s;
}

} // namespace

// The discontinuity formulas evaluated independently in Python
// (discontinuities.json, line data from scikit-rf). The C++ microstrip model
// agrees with scikit-rf to 1e-5, so 1e-4 covers the propagated difference.
TEST_CASE("Microstrip discontinuities agree with the published formulas", "[rfdata][discontinuities]")
{
    const QJsonObject data = loadJson(QStringLiteral("discontinuities.json"));
    const MicrostripSubstrate sub = discontinuitySubstrate(data);
    const std::vector<double> f = toDoubles(data.value(QStringLiteral("frequencies")));
    constexpr double Tol = 1e-4;

    for (const QJsonValue &item : data.value(QStringLiteral("open")).toArray()) {
        const QJsonObject c = item.toObject();
        const double w = c.value(QStringLiteral("w")).toDouble();
        const auto expected = toDoubles(c.value(QStringLiteral("c")));
        for (size_t k = 0; k < f.size(); ++k) {
            const auto cap = openEndCapacitance(sub, w, f[k]);
            REQUIRE(cap);
            CHECK_THAT(*cap, WithinRel(expected[k], Tol));
        }
    }
    for (const QJsonValue &item : data.value(QStringLiteral("gap")).toArray()) {
        const QJsonObject c = item.toObject();
        const auto c1 = toDoubles(c.value(QStringLiteral("c1"))), c2 = toDoubles(c.value(QStringLiteral("c2")));
        for (size_t k = 0; k < f.size(); ++k) {
            const auto gap = microstripGap(sub, c.value(QStringLiteral("w1")).toDouble(), c.value(QStringLiteral("w2")).toDouble(),
                                           c.value(QStringLiteral("s")).toDouble(), f[k]);
            REQUIRE(gap);
            CHECK_THAT(gap->cs, WithinRel(c.value(QStringLiteral("cs")).toDouble(), 1e-12));
            CHECK_THAT(gap->c1, WithinRel(c1[k], Tol));
            CHECK_THAT(gap->c2, WithinRel(c2[k], Tol));
        }
    }
    for (const QJsonValue &item : data.value(QStringLiteral("step")).toArray()) {
        const QJsonObject c = item.toObject();
        const auto l1 = toDoubles(c.value(QStringLiteral("l1"))), l2 = toDoubles(c.value(QStringLiteral("l2")));
        for (size_t k = 0; k < f.size(); ++k) {
            const auto step = microstripStep(sub, c.value(QStringLiteral("w1")).toDouble(), c.value(QStringLiteral("w2")).toDouble(), f[k]);
            REQUIRE(step);
            CHECK_THAT(step->c, WithinRel(c.value(QStringLiteral("c")).toDouble(), 1e-12));
            CHECK_THAT(step->l1, WithinRel(l1[k], Tol));
            CHECK_THAT(step->l2, WithinRel(l2[k], Tol));
        }
    }
    for (const QJsonValue &item : data.value(QStringLiteral("bend")).toArray()) {
        const QJsonObject c = item.toObject();
        const double w = c.value(QStringLiteral("w")).toDouble();
        const auto plain = microstripBend(sub, w, false), mitred = microstripBend(sub, w, true);
        REQUIRE(plain);
        REQUIRE(mitred);
        CHECK_THAT(plain->c, WithinRel(c.value(QStringLiteral("c")).toDouble(), 1e-12));
        CHECK_THAT(plain->l, WithinRel(c.value(QStringLiteral("l")).toDouble(), 1e-12));
        CHECK_THAT(mitred->c, WithinRel(c.value(QStringLiteral("cMitred")).toDouble(), 1e-12));
        CHECK_THAT(mitred->l, WithinRel(c.value(QStringLiteral("lMitred")).toDouble(), 1e-12));
    }
    double worstTee = 0;
    for (const QJsonValue &item : data.value(QStringLiteral("tee")).toArray()) {
        const QJsonObject c = item.toObject();
        const std::vector<CMatrix> expected = toMatrices(c.value(QStringLiteral("s")));
        for (size_t k = 0; k < f.size(); ++k) {
            const auto s = microstripTee(sub, c.value(QStringLiteral("w1")).toDouble(), c.value(QStringLiteral("w2")).toDouble(),
                                         c.value(QStringLiteral("w3")).toDouble(), f[k], 50.0);
            REQUIRE(s);
            const double d = maxDifference(*s, expected[k]);
            worstTee = std::max(worstTee, d);
            CHECK(d < 1e-4);
            // lossless substrate: lossless junction
            CHECK((*s * s->adjoint() - CMatrix::Identity(3, 3)).norm() < 1e-9);
            CHECK((*s - s->transpose()).norm() < 1e-12);
        }
    }
    std::printf("rfdata reference: microstrip tee largest S deviation from the Python evaluation %.3e\n", worstTee);
    for (const QJsonValue &item : data.value(QStringLiteral("via")).toArray()) {
        const QJsonObject c = item.toObject();
        const auto z = toComplexes(c.value(QStringLiteral("z")));
        for (size_t k = 0; k < f.size(); ++k) {
            const auto v = viaImpedance(c.value(QStringLiteral("d")).toDouble(), c.value(QStringLiteral("h")).toDouble(),
                                        c.value(QStringLiteral("t")).toDouble(), c.value(QStringLiteral("sigma")).toDouble(), f[k]);
            REQUIRE(v);
            CHECK(std::abs(*v - z[k]) < 1e-9 * std::abs(z[k]));
        }
    }
}

// Physical sanity checks and limits with published or hand-checked values.
TEST_CASE("Microstrip discontinuity limits", "[rfdata][discontinuities]")
{
    const MicrostripSubstrate alumina{0.635e-3, 0, 9.8, 0, 0, 0};

    // Open end on alumina, w/h = 1: Kirschning-Jansen-Koster gives
    // dl/h = 0.317; Hammerstad's older formula (Hammerstad and Bekkadal,
    // 1975) gives 0.315. Two independent fits agree to a few per cent.
    const double eeff = microstrip(alumina.line(0.635e-3), 1e9)->epsilonEffective;
    const double dl = openEndLengthExtension(0.635e-3, 0.635e-3, 9.8, eeff);
    const double u = 1.0;
    const double hammerstad = 0.102 * (u + 0.106) / (u + 0.264) * (1.166 + (9.8 + 1) / 9.8 * (0.9 + std::log(u + 2.475)));
    CHECK_THAT(dl / 0.635e-3, WithinRel(hammerstad, 0.06));
    std::printf("rfdata open end w/h = 1, er 9.8: dl/h %.4f (Hammerstad 1975 formula %.4f)\n", dl / 0.635e-3, hammerstad);

    // A wide gap leaves the open ends; a step between equal widths vanishes.
    const auto wide = microstripGap(alumina, 0.6e-3, 0.6e-3, 5e-3, 2e9);
    REQUIRE(wide);
    CHECK(wide->cs < 1e-6 * wide->c1);
    CHECK_THAT(wide->c1, WithinRel(*openEndCapacitance(alumina, 0.6e-3, 2e9), 2e-3));
    const auto narrowGap = microstripGap(alumina, 0.6e-3, 0.6e-3, 0.05e-3, 2e9);
    CHECK(narrowGap->cs > 3 * narrowGap->c1);
    const auto flat = microstripStep(alumina, 0.6e-3, 0.6e-3, 2e9);
    REQUIRE(flat);
    CHECK(flat->c == 0);
    CHECK(std::abs(flat->l1) < 1e-20);
    // The step is symmetric in its ports.
    const auto up = microstripStep(alumina, 0.3e-3, 0.9e-3, 2e9), down = microstripStep(alumina, 0.9e-3, 0.3e-3, 2e9);
    CHECK_THAT(up->l1, WithinRel(down->l2, 1e-12));
    CHECK_THAT(up->c, WithinRel(down->c, 1e-12));

    // Via: Goldfarb-Pucel inductance for d = 0.4 mm through h = 0.635 mm,
    // L = mu0/(2 pi) [h ln((h + a)/r) + 1.5 (r - a)] = 0.0981 nH by hand
    // (r = 0.2 mm, a = 0.66575 mm).
    const auto via = viaImpedance(0.4e-3, 0.635e-3, 0, 0, 1e9);
    REQUIRE(via);
    CHECK(via->real() == 0);
    CHECK_THAT(via->imag() / (2 * Pi * 1e9), WithinRel(2e-7 * (0.635e-3 * std::log((0.635e-3 + 0.66575e-3) / 0.2e-3)
                                                               + 1.5 * (0.2e-3 - 0.66575e-3)),
                                                       1e-4));
    CHECK_THAT(via->imag() / (2 * Pi * 1e9), WithinRel(0.0981e-9, 0.002));
    QString error;
    CHECK_FALSE(viaImpedance(0.4e-3, 0.635e-3, 0.3e-3, 5.8e7, 1e9, &error));

    // T-junction: at low frequency the transformers tend to 1, BT and the
    // arm lines to nothing, leaving an ideal three-way node:
    // S11 = -1/3, S21 = S31 = 2/3.
    const auto tee = microstripTee(alumina, 0.6e-3, 0.6e-3, 0.6e-3, 1e6, 50.0);
    REQUIRE(tee);
    CHECK(std::abs((*tee)(0, 0) - Complex(-1.0 / 3, 0)) < 1e-3);
    CHECK(std::abs((*tee)(1, 0) - Complex(2.0 / 3, 0)) < 1e-3);
    CHECK(std::abs((*tee)(2, 0) - Complex(2.0 / 3, 0)) < 1e-3);
    // At 10 GHz the junction reactance shows: the through path is no longer ideal.
    const auto teeHigh = microstripTee(alumina, 0.6e-3, 0.6e-3, 0.6e-3, 10e9, 50.0);
    CHECK(std::abs(std::abs((*teeHigh)(1, 0)) - 2.0 / 3) > 1e-3);
}

TEST_CASE("connectToPort cascades a line onto one port", "[rfdata][discontinuities]")
{
    // A matched 90 degree line on port 2 of a two-port only shifts the phase.
    CMatrix s(2, 2);
    s << Complex(0.2, 0.1), Complex(0.5, -0.3), Complex(0.5, -0.3), Complex(-0.1, 0.4);
    CMatrix line(2, 2);
    line << 0.0, Complex(0, -1), Complex(0, -1), 0.0;
    const CMatrix out = connectToPort(s, 1, line);
    CHECK(std::abs(out(0, 0) - s(0, 0)) < 1e-15);
    CHECK(std::abs(out(1, 0) - s(1, 0) * Complex(0, -1)) < 1e-15);
    CHECK(std::abs(out(1, 1) - s(1, 1) * Complex(-1, 0)) < 1e-15);
}

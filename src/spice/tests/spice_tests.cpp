#include "spice/NgspiceEngine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include <atomic>
#include <numbers>
#include <thread>

using namespace lvd::spice;
using Catch::Approx;

namespace {

NgspiceEngine &engine()
{
    NgspiceEngine::setRuntimeFolder(QStringLiteral(LVD_NGSPICE_RUNTIME));
    return NgspiceEngine::instance();
}

} // namespace

TEST_CASE("ngspice loads and runs an operating point")
{
    QString error;
    REQUIRE(engine().isAvailable(&error));
    INFO(error.toStdString());

    const SpiceRunResult result = engine().run(QStringLiteral("divider\nV1 in 0 10\nR1 in out 1k\nR2 out 0 3k\n.op\n.end\n"));
    INFO(result.errors.join(QLatin1Char('\n')).toStdString());
    INFO(result.log.join(QLatin1Char('\n')).toStdString());
    REQUIRE(result.ok);
    const SpicePlot *op = result.plot(QStringLiteral("op"));
    REQUIRE(op);
    INFO(op->vectorNames().join(QLatin1Char(' ')).toStdString());
    const SpiceVector *out = op->vector(QStringLiteral("v(out)"));
    REQUIRE(out);
    CHECK(out->real.at(0) == Approx(7.5));
    const SpiceVector *source = op->vector(QStringLiteral("i(v1)"));
    REQUIRE(source);
    CHECK(source->real.at(0) == Approx(-2.5e-3));
}

TEST_CASE("ngspice transient and AC analyses give the expected RC response")
{
    SECTION("transient")
    {
        const SpiceRunResult result = engine().run(QStringLiteral(
            "rc\nV1 in 0 PULSE(0 1 0 1n 1n 1 2)\nR1 in out 1k\nC1 out 0 1u\n.tran 10u 5m\n.end\n"));
        INFO(result.errors.join(QLatin1Char('\n')).toStdString());
        REQUIRE(result.ok);
        const SpicePlot *tran = result.plot(QStringLiteral("tran"));
        REQUIRE(tran);
        REQUIRE(tran->scaleName == QLatin1String("time"));
        const SpiceVector *time = tran->scale();
        const SpiceVector *out = tran->vector(QStringLiteral("v(out)"));
        REQUIRE(time);
        REQUIRE(out);
        // v(out) at t = tau
        std::size_t i = 0;
        while (i + 1 < time->real.size() && time->real[i + 1] < 1e-3)
            ++i;
        const double t0 = time->real[i], t1 = time->real[i + 1];
        const double v = out->real[i] + (out->real[i + 1] - out->real[i]) * (1e-3 - t0) / (t1 - t0);
        CHECK(v == Approx(1.0 - std::exp(-1.0)).margin(5e-3));
    }
    SECTION("AC")
    {
        const SpiceRunResult result = engine().run(QStringLiteral(
            "rc\nV1 in 0 AC 1\nR1 in out 1k\nC1 out 0 1u\n.ac dec 10 10 100k\n.end\n"));
        REQUIRE(result.ok);
        const SpicePlot *ac = result.plot(QStringLiteral("ac"));
        REQUIRE(ac);
        const SpiceVector *f = ac->scale();
        const SpiceVector *out = ac->vector(QStringLiteral("v(out)"));
        REQUIRE(f);
        REQUIRE(out);
        REQUIRE(out->isComplex);
        for (std::size_t k = 0; k < f->size(); ++k) {
            const std::complex<double> expected = 1.0 / (1.0 + std::complex<double>(0, 2 * std::numbers::pi * f->real[k] * 1e-3));
            CHECK(std::abs(out->complex[k] - expected) < 1e-9);
        }
    }
}

TEST_CASE("a netlist error is reported and the engine stays usable")
{
    const SpiceRunResult bad = engine().run(QStringLiteral("bad\nV1 in 0 1\nQ1 a b c nosuchmodel\n.op\n.end\n"));
    CHECK_FALSE(bad.ok);
    CHECK_FALSE(bad.errors.isEmpty());
    const SpiceRunResult good = engine().run(QStringLiteral("good\nV1 in 0 1\nR1 in 0 1k\n.op\n.end\n"));
    INFO(good.errors.join(QLatin1Char('\n')).toStdString());
    CHECK(good.ok);
}

TEST_CASE("a long transient can be aborted")
{
    std::atomic_bool progressed = false;
    SpiceRunOptions options;
    options.progress = [&](const SpiceProgress &p) {
        if (p.fraction > 0.01)
            progressed = true;
    };
    std::thread stopper([&] {
        for (int i = 0; i < 400 && !progressed; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        engine().abort();
    });
    const SpiceRunResult result = engine().run(
        QStringLiteral("long\nV1 in 0 SIN(0 1 1meg)\nR1 in out 1k\nC1 out 0 1n\n.tran 1n 100m\n.end\n"), options);
    stopper.join();
    CHECK(result.cancelled);
    CHECK_FALSE(result.ok);
    CHECK(result.seconds < 10);
    const SpiceRunResult after = engine().run(QStringLiteral("after\nV1 in 0 1\nR1 in 0 1k\n.op\n.end\n"));
    CHECK(after.ok);
}

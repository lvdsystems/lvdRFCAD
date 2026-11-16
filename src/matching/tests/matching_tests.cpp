#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/Expression.h"
#include "core/Netlist.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "core/StandardValues.h"
#include "matching/MatchingInsert.h"
#include "matching/MatchingSynthesis.h"
#include "rfsim/LinearSolver.h"

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

using namespace lvd;
using namespace lvd::rf;
using Catch::Approx;

namespace {

using Z = std::complex<double>;

struct Pair {
    const char *name;
    Z zs;
    Z zl;
};

const std::vector<Pair> &pairs()
{
    static const std::vector<Pair> list = {
        {"real 50 -> 200", {50, 0}, {200, 0}},
        {"real 50 -> 12", {50, 0}, {12, 0}},
        {"real high ratio 50 -> 2000", {50, 0}, {2000, 0}},
        {"real low ratio 75 -> 2", {75, 0}, {2, 0}},
        {"complex load 50 -> 12-j30", {50, 0}, {12, -30}},
        {"complex both 25+j15 -> 12-j30", {25, 15}, {12, -30}},
        {"complex both 10-j5 -> 150+j80", {10, -5}, {150, 80}},
        {"complex both 300+j200 -> 3-j1", {300, 200}, {3, -1}},
    };
    return list;
}

double relativeError(const MatchSolution &s)
{
    const Z target = std::conj(s.zs);
    return std::abs(inputImpedance(s, s.frequency) - target) / std::abs(target);
}

void checkSolutions(const MatchResult &result)
{
    INFO(result.error.toStdString());
    REQUIRE(result.ok());
    REQUIRE_FALSE(result.solutions.empty());
    for (const MatchSolution &s : result.solutions) {
        INFO(s.description.toStdString());
        CHECK(relativeError(s) < 1e-9);
        CHECK(s.mismatch < 1e-9);
        CHECK(std::isfinite(s.loadedQ));
        for (const MatchElement &e : s.elements) {
            if (e.isLumped()) {
                CHECK(e.value > 0);
            } else {
                CHECK(e.lengthDeg > 0);
                CHECK(e.lengthDeg < 180);
                CHECK(e.z0 > 0);
            }
        }
    }
}

MatchRequest request(const Pair &p, MatchTopology topology, double f0 = 2.4e9)
{
    MatchRequest r;
    r.zs = p.zs;
    r.zl = p.zl;
    r.frequency = f0;
    r.topology = topology;
    return r;
}

bool isSeriesValue(double value, ESeries series)
{
    const double exponent = std::floor(std::log10(value));
    const double mantissa = value / std::pow(10.0, exponent);
    for (const double m : seriesMantissas(series)) {
        if (std::abs(mantissa - m) < 1e-9 * m || std::abs(mantissa - 10 * m) < 1e-8 * m)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("L-networks give every valid two-element conjugate match")
{
    for (const Pair &p : pairs()) {
        INFO(p.name);
        const MatchResult result = synthesizeMatch(request(p, MatchTopology::LNetwork));
        checkSolutions(result);
        CHECK(result.solutions.size() <= 4);
        for (const MatchSolution &s : result.solutions)
            CHECK(s.elements.size() <= 2);
    }
    // Real to real with a ratio: exactly two solutions (low-pass and high-pass) of one orientation.
    const MatchResult real = synthesizeMatch(request(pairs()[0], MatchTopology::LNetwork));
    CHECK(real.solutions.size() == 2);
    // Complex both sides where both orientations exist: four.
    MatchRequest both;
    both.zs = {50, 0};
    both.zl = {30, 40};
    both.frequency = 1e9;
    const MatchResult four = synthesizeMatch(both);
    checkSolutions(four);
    CHECK(four.solutions.size() == 4);
    // The L-network Q of a real match is sqrt(Rhigh/Rlow - 1).
    CHECK(real.solutions.front().loadedQ == Approx(std::sqrt(200.0 / 50 - 1)).epsilon(1e-9));
}

TEST_CASE("Pi and T networks honour the loaded Q and reject a Q that is too low")
{
    for (const Pair &p : pairs()) {
        INFO(p.name);
        for (const MatchTopology t : {MatchTopology::PiNetwork, MatchTopology::TNetwork}) {
            const double qmin = t == MatchTopology::PiNetwork ? minimumPiQ(p.zs, p.zl) : minimumTQ(p.zs, p.zl);
            MatchRequest r = request(p, t);
            r.q = qmin * 1.5 + 1;
            const MatchResult result = synthesizeMatch(r);
            checkSolutions(result);
            CHECK(result.solutions.size() <= 4);
            if (p.zs.imag() == 0 && p.zl.imag() == 0) {
                CHECK(result.solutions.size() == 4);
                for (const MatchSolution &s : result.solutions)
                    CHECK(s.loadedQ == Approx(r.q).epsilon(1e-6));
            }
            r.q = qmin * 0.9;
            const MatchResult low = synthesizeMatch(r);
            CHECK_FALSE(low.ok());
            CHECK(low.error.contains(QStringLiteral("minimum")));
            CHECK(low.solutions.empty());
        }
    }
    CHECK(minimumPiQ(Z(50, 0), Z(200, 0)) == Approx(std::sqrt(3.0)));
    CHECK(minimumTQ(Z(50, 0), Z(5, 0)) == Approx(3.0));
}

TEST_CASE("multi-section ladders step geometrically between real impedances")
{
    for (const Pair &p : pairs()) {
        INFO(p.name);
        for (const int n : {1, 2, 3, 5}) {
            MatchRequest r = request(p, MatchTopology::MultiSection);
            r.sections = n;
            const MatchResult result = synthesizeMatch(r);
            if (p.zs.imag() != 0 || p.zl.imag() != 0) {
                CHECK_FALSE(result.ok());
                continue;
            }
            checkSolutions(result);
            CHECK(result.solutions.size() == 2);
            for (const MatchSolution &s : result.solutions) {
                CHECK(s.elements.size() == static_cast<size_t>(2 * n));
                const double ratio = std::max(p.zs.real(), p.zl.real()) / std::min(p.zs.real(), p.zl.real());
                CHECK(s.loadedQ == Approx(std::sqrt(std::pow(ratio, 1.0 / n) - 1)).epsilon(1e-6));
            }
        }
    }
    // More sections widen the band.
    MatchRequest r = request(pairs()[2], MatchTopology::MultiSection);
    r.sections = 1;
    const double bw1 = synthesizeMatch(r).solutions.front().bandwidth3dB;
    r.sections = 3;
    const double bw3 = synthesizeMatch(r).solutions.front().bandwidth3dB;
    CHECK(bw3 > bw1);
}

TEST_CASE("distributed networks: quarter-wave, single stub and double stub")
{
    for (const Pair &p : pairs()) {
        INFO(p.name);
        checkSolutions(synthesizeMatch(request(p, MatchTopology::QuarterWave)));
        const MatchResult single = synthesizeMatch(request(p, MatchTopology::SingleStub));
        checkSolutions(single);
        // Two line positions, each with an open and a short stub.
        CHECK(single.solutions.size() == 4);

        MatchRequest r = request(p, MatchTopology::DoubleStub);
        r.stubSpacingDeg = 45;
        const MatchResult dbl = synthesizeMatch(r);
        if (dbl.ok())
            checkSolutions(dbl);
        else
            CHECK(dbl.error.contains(QStringLiteral("forbidden")));
    }

    // A real load: the quarter-wave transformer alone, Z = sqrt(Rs RL).
    const MatchResult qw = synthesizeMatch(request(pairs()[0], MatchTopology::QuarterWave));
    REQUIRE(qw.solutions.size() == 1);
    REQUIRE(qw.solutions.front().elements.size() == 1);
    CHECK(qw.solutions.front().elements.front().z0 == Approx(100.0));
    CHECK(qw.solutions.front().elements.front().lengthDeg == Approx(90.0));

    // A complex load: a line to Rmax or Rmin, or a stub cancelling the susceptance.
    const MatchResult qwc = synthesizeMatch(request(pairs()[4], MatchTopology::QuarterWave));
    CHECK(qwc.solutions.size() == 4);

    // A load inside the forbidden region of a double-stub tuner with 1/8 wave spacing:
    // normalised conductance above 1 / sin^2(45 deg) = 2.
    MatchRequest forbidden;
    forbidden.zs = {50, 0};
    forbidden.zl = {10, 0};
    forbidden.frequency = 1e9;
    forbidden.topology = MatchTopology::DoubleStub;
    forbidden.stubSpacingDeg = 45;
    CHECK_FALSE(synthesizeMatch(forbidden).ok());
    forbidden.zl = {200, 50};
    checkSolutions(synthesizeMatch(forbidden));
}

TEST_CASE("the reflection is smallest at the design frequency")
{
    for (const MatchTopology t : {MatchTopology::LNetwork, MatchTopology::PiNetwork, MatchTopology::TNetwork,
                                  MatchTopology::QuarterWave, MatchTopology::SingleStub}) {
        MatchRequest r = request(pairs()[4], t);
        r.q = 5;
        const MatchResult result = synthesizeMatch(r);
        REQUIRE(result.ok());
        std::vector<double> f;
        for (int i = 0; i <= 200; ++i)
            f.push_back(r.frequency * (0.5 + i / 200.0));
        for (const MatchSolution &s : result.solutions) {
            INFO(s.description.toStdString());
            const std::vector<double> m = reflectionMagnitude(s, f);
            const auto minimum = std::min_element(m.begin(), m.end());
            CHECK(minimum - m.begin() == 100);
            CHECK(m[100] < 1e-9);
            CHECK(m.front() > 1e-3);
            CHECK(std::isfinite(s.bandwidth3dB) == (s.bandwidth3dB > 0));
        }
    }
}

TEST_CASE("snapping to a standard series uses series values and reports the mismatch")
{
    MatchRequest r = request(pairs()[4], MatchTopology::LNetwork);
    const MatchResult exact = synthesizeMatch(r);
    r.series = ESeries::E12;
    const MatchResult snapped = synthesizeMatch(r);
    REQUIRE(snapped.ok());
    REQUIRE(snapped.solutions.size() == exact.solutions.size());
    for (size_t i = 0; i < snapped.solutions.size(); ++i) {
        const MatchSolution &s = snapped.solutions[i];
        CHECK(s.snapped);
        CHECK(s.series == ESeries::E12);
        CHECK(s.mismatch > 0);
        CHECK(s.mismatch < 0.5);
        CHECK(s.mismatch == Approx(std::abs(inputReflection(s, s.frequency))));
        bool changed = false;
        for (size_t k = 0; k < s.elements.size(); ++k) {
            const MatchElement &e = s.elements[k];
            CHECK(isSeriesValue(e.value, ESeries::E12));
            CHECK(e.idealValue == Approx(exact.solutions[i].elements[k].value).epsilon(1e-12));
            changed = changed || e.value != e.idealValue;
        }
        CHECK(changed);
    }
    // E96 snaps closer than E12 on average.
    r.series = ESeries::E96;
    const MatchResult fine = synthesizeMatch(r);
    double coarse = 0, close = 0;
    for (size_t i = 0; i < fine.solutions.size(); ++i) {
        coarse += snapped.solutions[i].mismatch;
        close += fine.solutions[i].mismatch;
    }
    CHECK(close < coarse);
}

TEST_CASE("invalid requests give clear errors")
{
    MatchRequest r;
    r.zs = {0, 10};
    CHECK(synthesizeMatch(r).error.contains(QStringLiteral("source resistance")));
    r.zs = {50, 0};
    r.zl = {-5, 3};
    CHECK(synthesizeMatch(r).error.contains(QStringLiteral("load resistance")));
    r.zl = {10, 0};
    r.frequency = 0;
    CHECK(synthesizeMatch(r).error.contains(QStringLiteral("frequency")));
    r.frequency = 1e9;
    r.topology = MatchTopology::DoubleStub;
    r.stubSpacingDeg = 180;
    CHECK_FALSE(synthesizeMatch(r).ok());
    // Already matched: a direct connection.
    r.topology = MatchTopology::LNetwork;
    r.zl = {50, 0};
    const MatchResult direct = synthesizeMatch(r);
    REQUIRE(direct.ok());
    REQUIRE(direct.solutions.size() == 1);
    CHECK(direct.solutions.front().elements.empty());
}

TEST_CASE("the impedance trajectory runs from the load to the conjugate source")
{
    const MatchResult result = synthesizeMatch(request(pairs()[5], MatchTopology::PiNetwork));
    REQUIRE(result.ok());
    const MatchSolution &s = result.solutions.front();
    const std::vector<TrajectorySegment> path = impedanceTrajectory(s, 32);
    REQUIRE(path.size() == s.elements.size());
    CHECK(std::abs(path.front().impedances.front() - s.zl) < 1e-9);
    CHECK(path.front().elementIndex == static_cast<int>(s.elements.size()) - 1);
    for (size_t i = 1; i < path.size(); ++i)
        CHECK(std::abs(path[i].impedances.front() - path[i - 1].impedances.back()) < 1e-9 * std::abs(s.zl));
    CHECK(std::abs(path.back().impedances.back() - std::conj(s.zs)) < 1e-9 * std::abs(s.zs));
    CHECK(path.front().impedances.size() == 33);
}

// ---------------------------------------------------------------------------
// Schematic insertion, verified with the linear simulator.

namespace {

struct Bench {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;

    Bench()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("dut"), &error);
        REQUIRE(document);
        sheet = document->schematic();
    }

    DefinitionPtr definition(const QString &key) { return project->resolve(*sheet, key).definition; }

    // Places a part so that the given pin lands on `pinAt`.
    QString placeAt(const char *key, const char *pinName, QPoint pinAt, const QMap<QString, QString> &parameters = {})
    {
        const QString k = QString::fromLatin1(key);
        const DefinitionPtr d = definition(k);
        REQUIRE(d);
        PlacedComponent probe;
        const QPoint offset = pinPosition(probe, *d->symbol.pin(QString::fromLatin1(pinName)));
        const QString id = sheet->addComponent(k, d.get(), pinAt - offset);
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            sheet->setParameter(id, it.key(), it.value());
        return id;
    }

    QPoint pin(const QString &id, const char *name)
    {
        const PlacedComponent *component = sheet->component(id);
        return pinPosition(*component, *definition(component->definition)->symbol.pin(QString::fromLatin1(name)));
    }

    // Port 1 (z0 = Rs) at the input; ZL = R + jX as a series R and L or C to ground at the output.
    void terminate(const InsertedMatch &m, double rs, Z zl, double f0)
    {
        const QPoint portPin = m.input - QPoint(60, 0);
        placeAt("rf-system:port", "p", portPin, {{QStringLiteral("num"), QStringLiteral("1")}, {QStringLiteral("z0"), QString::number(rs, 'g', 17)}});
        sheet->addWire({portPin, m.input});
        const QString r = placeAt("basic:resistor", "p", m.output + QPoint(40, 0), {{QStringLiteral("r"), QString::number(zl.real(), 'g', 17)}});
        sheet->addWire({m.output, pin(r, "p")});
        const double omega = 2 * 3.14159265358979323846 * f0;
        QString x;
        if (zl.imag() < 0)
            x = placeAt("basic:capacitor", "p", pin(r, "n") + QPoint(40, 0), {{QStringLiteral("c"), QString::number(-1 / (omega * zl.imag()), 'g', 17)}});
        else
            x = placeAt("basic:inductor", "p", pin(r, "n") + QPoint(40, 0), {{QStringLiteral("l"), QString::number(zl.imag() / omega, 'g', 17)}});
        sheet->addWire({pin(r, "n"), pin(x, "p")});
        const QPoint groundPin = pin(x, "n") + QPoint(30, 0);
        placeAt("basic:ground", "gnd", groundPin);
        sheet->addWire({pin(x, "n"), groundPin});
    }

    Complex s11(double f0)
    {
        const Netlist netlist = buildNetlist(*project, *document);
        INFO(netlist.errors.join(QLatin1Char('\n')).toStdString());
        REQUIRE(netlist.errors.isEmpty());
        LinearOptions options;
        options.noise = false;
        options.projectDirectory = project->directory();
        const LinearResult result = simulateLinear(netlist, {f0}, options);
        INFO(result.errors.join(QLatin1Char('\n')).toStdString());
        REQUIRE(result.ok());
        return result.network.s.front()(0, 0);
    }
};

} // namespace

TEST_CASE("inserted networks simulate to a match")
{
    const double f0 = 2.4e9;
    const Z zl(12, -30);
    for (const MatchTopology t : {MatchTopology::LNetwork, MatchTopology::SingleStub, MatchTopology::PiNetwork,
                                  MatchTopology::QuarterWave, MatchTopology::DoubleStub}) {
        MatchRequest r;
        r.zs = {50, 0};
        r.zl = zl;
        r.frequency = f0;
        r.topology = t;
        r.q = 4;
        const MatchResult result = synthesizeMatch(r);
        REQUIRE(result.ok());
        for (const MatchSolution &s : result.solutions) {
            INFO(s.description.toStdString());
            Bench b;
            QString error;
            const auto inserted = insertMatchingNetwork(*b.project, *b.document, s, QPoint(100, 100), &error);
            INFO(error.toStdString());
            REQUIRE(inserted);
            CHECK(inserted->inputLabel == QStringLiteral("MATCH_IN"));
            CHECK(inserted->outputLabel == QStringLiteral("MATCH_OUT"));
            b.terminate(*inserted, 50, zl, f0);
            CHECK(std::abs(b.s11(f0)) < 1e-6);
        }
    }
}

TEST_CASE("insertion is one undo step with unique, renamable labels")
{
    MatchRequest r;
    r.zs = {50, 0};
    r.zl = {12, -30};
    r.frequency = 2.4e9;
    r.topology = MatchTopology::PiNetwork;
    r.q = 4;
    const MatchResult result = synthesizeMatch(r);
    REQUIRE(result.ok());

    Bench b;
    b.sheet->addLabel(QStringLiteral("MATCH_IN"), QPoint(-500, -500));
    const QStringList before = b.sheet->itemIds();
    const int undoCount = b.sheet->undoStack()->count();

    QString error;
    const auto inserted = insertMatchingNetwork(*b.project, *b.document, result.solutions.front(), QPoint(0, 0), &error);
    REQUIRE(inserted);
    CHECK(inserted->inputLabel == QStringLiteral("MATCH_IN_2"));
    CHECK(inserted->outputLabel == QStringLiteral("MATCH_OUT_2"));
    CHECK(b.sheet->undoStack()->count() == undoCount + 1);
    CHECK(b.sheet->itemIds().size() > before.size());
    for (const QString &id : inserted->itemIds)
        CHECK(b.sheet->contains(id));

    // Three elements and two grounds (shunt C/L) or none (series), wired as one ladder.
    int components = 0;
    for (const PlacedComponent &c : b.sheet->components())
        components += c.definition != QStringLiteral("basic:ground") ? 1 : 0;
    CHECK(components == static_cast<int>(result.solutions.front().elements.size()));
    const Connectivity connectivity = computeConnectivity(*b.sheet, b.project->lookupFor(*b.sheet));
    CHECK(connectivity.net(QStringLiteral("MATCH_IN_2")) != nullptr);
    CHECK(connectivity.net(QStringLiteral("MATCH_OUT_2")) != nullptr);
    CHECK(connectivity.unresolvedComponents.isEmpty());

    // Parameter text is engineering notation that evaluates to the value.
    for (const PlacedComponent &c : b.sheet->components()) {
        if (c.definition == QStringLiteral("basic:capacitor")) {
            const QString text = c.parameters.value(QStringLiteral("c"));
            CHECK(text.endsWith(QStringLiteral("F")));
            CHECK(Expression::parse(text).evaluate().has_value());
        }
    }

    b.sheet->undoStack()->undo();
    CHECK(b.sheet->itemIds() == before);
    b.sheet->undoStack()->redo();
    CHECK(b.sheet->itemIds().size() == before.size() + inserted->itemIds.size());

    // A missing library part fails without changing anything.
    Bench c;
    c.project->setLibraryManager(nullptr);
    const QStringList empty = c.sheet->itemIds();
    CHECK_FALSE(insertMatchingNetwork(*c.project, *c.document, result.solutions.front(), QPoint(0, 0), &error));
    CHECK(error.contains(QStringLiteral("basic:")));
    CHECK(c.sheet->itemIds() == empty);
}

TEST_CASE("snapped values are written exactly")
{
    MatchRequest r;
    r.zs = {50, 0};
    r.zl = {12, -30};
    r.frequency = 2.4e9;
    r.series = ESeries::E24;
    const MatchResult result = synthesizeMatch(r);
    REQUIRE(result.ok());
    Bench b;
    REQUIRE(insertMatchingNetwork(*b.project, *b.document, result.solutions.front(), QPoint(0, 0)));
    for (const PlacedComponent &c : b.sheet->components()) {
        for (const QString &name : {QStringLiteral("c"), QStringLiteral("l")}) {
            if (!c.parameters.contains(name))
                continue;
            const auto value = Expression::parse(c.parameters.value(name)).evaluate();
            REQUIRE(value);
            CHECK(isSeriesValue(*value, ESeries::E24));
            CHECK(c.parameters.value(name).size() <= 8); // "2.2 nH"
        }
    }
}

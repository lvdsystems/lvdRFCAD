#include "ReferenceData.h"

#include "rfdata/NetworkMath.h"
#include "rfdata/Touchstone.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QTemporaryDir>

#include <random>

using namespace lvd::rf;
using namespace lvd::rf::test;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

NetworkData randomNetwork(int ports, int frequencies, std::vector<double> z0, unsigned seed)
{
    std::mt19937 generator(seed);
    std::uniform_real_distribution<double> uniform(-0.6, 0.6);
    NetworkData network;
    network.z0 = std::move(z0);
    for (int k = 0; k < frequencies; ++k) {
        network.frequencies.push_back(1e9 + 0.37e9 * k);
        CMatrix s(ports, ports);
        for (int i = 0; i < ports; ++i)
            for (int j = 0; j < ports; ++j)
                s(i, j) = Complex(uniform(generator), uniform(generator));
        network.s.push_back(s);
    }
    return network;
}

void addNoise(NetworkData &network)
{
    for (size_t k = 0; k < network.frequencies.size(); k += 2) {
        network.noiseFrequencies.push_back(network.frequencies[k]);
        network.noise.push_back({0.5 + 0.1 * static_cast<double>(k), std::polar(0.4, 0.3 * static_cast<double>(k)),
                                 10.0 + static_cast<double>(k)});
    }
}

double maxDifference(const NetworkData &a, const NetworkData &b)
{
    double worst = 0;
    for (size_t k = 0; k < a.s.size(); ++k)
        worst = std::max(worst, (a.s[k] - b.s[k]).cwiseAbs().maxCoeff());
    return worst;
}

TouchstoneReadResult parse(const char *text, int ports = 0)
{
    return parseTouchstone(QByteArray(text), ports);
}

} // namespace

// The files are written with 10 significant digits and read by both
// implementations from the same text, so they must agree to rounding (1e-9).
TEST_CASE("Touchstone files read like scikit-rf reads them", "[rfdata][touchstone]")
{
    const QJsonObject expected = loadJson(QStringLiteral("touchstone.json"));
    REQUIRE(expected.size() == 6);
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        INFO(it.key().toStdString());
        const TouchstoneReadResult result = readTouchstone(dataPath(it.key()));
        REQUIRE(result.network);
        CHECK(result.warnings.isEmpty());
        const NetworkData &network = *result.network;
        const QJsonObject e = it.value().toObject();
        const std::vector<double> frequencies = toDoubles(e.value(QStringLiteral("frequencies")));
        const std::vector<double> z0 = toDoubles(e.value(QStringLiteral("z0")));
        const std::vector<CMatrix> s = toMatrices(e.value(QStringLiteral("s")));
        REQUIRE(network.frequencyCount() == static_cast<int>(frequencies.size()));
        CHECK(network.z0 == z0);
        for (size_t k = 0; k < frequencies.size(); ++k) {
            CHECK_THAT(network.frequencies[k], WithinRel(frequencies[k], 1e-14));
            CHECK(test::maxDifference(network.s[k], s[k]) < 1e-9);
        }
        const QJsonArray noise = e.value(QStringLiteral("noise")).toArray();
        REQUIRE(network.noise.size() == static_cast<size_t>(noise.size()));
        for (qsizetype k = 0; k < noise.size(); ++k) {
            const QJsonObject n = noise.at(k).toObject();
            const auto index = static_cast<size_t>(k);
            CHECK_THAT(network.noiseFrequencies[index], WithinRel(n.value(QStringLiteral("frequency")).toDouble(), 1e-14));
            CHECK_THAT(network.noise[index].fminDb, WithinRel(n.value(QStringLiteral("fminDb")).toDouble(), 1e-12));
            CHECK(std::abs(network.noise[index].gammaOpt - toComplex(n.value(QStringLiteral("gammaOpt")))) < 1e-12);
            CHECK_THAT(network.noise[index].rn, WithinRel(n.value(QStringLiteral("rn")).toDouble(), 1e-12));
        }
    }
}

TEST_CASE("Touchstone comments and port count inference", "[rfdata][touchstone]")
{
    const auto onePort = readTouchstone(dataPath(QStringLiteral("one_port.s1p")));
    REQUIRE(onePort.network);
    CHECK(onePort.network->comment == QStringLiteral("one-port test file\n second comment line"));

    for (const char *name : {"one_port.s1p", "amp_noise.s2p", "three_port.s3p", "four_port.s4p"}) {
        INFO(name);
        QFile file(dataPath(QString::fromLatin1(name)));
        REQUIRE(file.open(QIODevice::ReadOnly));
        const auto fromText = parseTouchstone(file.readAll());
        const auto fromFile = readTouchstone(dataPath(QString::fromLatin1(name)));
        REQUIRE(fromText.network);
        REQUIRE(fromFile.network);
        CHECK(fromText.network->portCount() == fromFile.network->portCount());
        CHECK(fromText.network->noise.size() == fromFile.network->noise.size());
    }
}

TEST_CASE("Touchstone round trips", "[rfdata][touchstone]")
{
    const int ports = GENERATE(1, 2, 3, 4, 5);
    const int version = GENERATE(1, 2);
    const auto format = GENERATE(TouchstoneFormat::RealImaginary, TouchstoneFormat::MagnitudeAngle,
                                 TouchstoneFormat::DecibelAngle);
    const QString unit = GENERATE(QStringLiteral("Hz"), QStringLiteral("kHz"), QStringLiteral("MHz"), QStringLiteral("GHz"));
    INFO("ports " << ports << " version " << version << " format " << static_cast<int>(format) << " unit "
                  << unit.toStdString());

    std::vector<double> z0(static_cast<size_t>(ports), 50.0);
    if (version == 2)
        for (int i = 0; i < ports; ++i)
            z0[static_cast<size_t>(i)] = 25.0 + 12.5 * i;
    NetworkData network = randomNetwork(ports, 7, z0, static_cast<unsigned>(ports * 10 + version));
    network.s[3](0, 0) = 0; // exact zero survives every format
    network.comment = QStringLiteral("round trip\n\nthird line");
    if (ports == 2)
        addNoise(network);

    TouchstoneWriteOptions options;
    options.format = format;
    options.frequencyUnit = unit;
    options.version = version;
    options.precision = 12;
    QString error;
    const QByteArray text = formatTouchstone(network, options, &error);
    REQUIRE(error.isEmpty());
    const auto result = parseTouchstone(text, version == 1 ? ports : 0);
    INFO(text.toStdString());
    REQUIRE(result.network);
    CHECK(result.warnings.isEmpty());
    const NetworkData &read = *result.network;
    CHECK(read.z0 == network.z0);
    CHECK(read.comment == network.comment);
    REQUIRE(read.frequencyCount() == network.frequencyCount());
    for (int k = 0; k < network.frequencyCount(); ++k)
        CHECK_THAT(read.frequencies[static_cast<size_t>(k)], WithinRel(network.frequencies[static_cast<size_t>(k)], 1e-15));
    CHECK(maxDifference(read, network) < 1e-11);
    CHECK(read.s[3](0, 0) == Complex(0, 0));
    REQUIRE(read.noise.size() == network.noise.size());
    for (size_t k = 0; k < network.noise.size(); ++k) {
        CHECK_THAT(read.noiseFrequencies[k], WithinRel(network.noiseFrequencies[k], 1e-15));
        CHECK_THAT(read.noise[k].fminDb, WithinRel(network.noise[k].fminDb, 1e-11));
        CHECK(std::abs(read.noise[k].gammaOpt - network.noise[k].gammaOpt) < 1e-11);
        CHECK_THAT(read.noise[k].rn, WithinRel(network.noise[k].rn, 1e-11));
    }
}

TEST_CASE("Touchstone round trip with 17 digits is exact", "[rfdata][touchstone]")
{
    NetworkData network = randomNetwork(3, 5, {50, 50, 50}, 7);
    TouchstoneWriteOptions options;
    options.precision = 17;
    options.frequencyUnit = QStringLiteral("Hz");
    const auto result = parseTouchstone(formatTouchstone(network, options), 3);
    REQUIRE(result.network);
    CHECK(result.network->s == network.s);
    CHECK(result.network->frequencies == network.frequencies);
}

TEST_CASE("Touchstone files are written and read from disk", "[rfdata][touchstone]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    NetworkData network = randomNetwork(2, 4, {50, 50}, 3);
    addNoise(network);
    const QString path = dir.filePath(QStringLiteral("written.s2p"));
    QString error;
    REQUIRE(writeTouchstone(path, network, {}, &error));
    const auto result = readTouchstone(path);
    REQUIRE(result.network);
    CHECK(maxDifference(*result.network, network) < 1e-11);
    CHECK(result.network->noise.size() == network.noise.size());

    CHECK_FALSE(writeTouchstone(dir.filePath(QStringLiteral("missing/x.s2p")), network, {}, &error));
    CHECK_FALSE(error.isEmpty());
    const auto missing = readTouchstone(dir.filePath(QStringLiteral("nothing.s2p")));
    CHECK_FALSE(missing.network);
    CHECK_THAT(missing.error.toStdString(), ContainsSubstring("Cannot open"));
}

TEST_CASE("Touchstone writer rejects what it cannot express", "[rfdata][touchstone]")
{
    QString error;
    NetworkData mixed = randomNetwork(2, 3, {50, 75}, 1);
    CHECK(formatTouchstone(mixed, {}, &error).isEmpty());
    CHECK_THAT(error.toStdString(), ContainsSubstring("version 2"));

    TouchstoneWriteOptions v2;
    v2.version = 2;
    const QByteArray text = formatTouchstone(mixed, v2, &error);
    CHECK(text.contains("[Reference] 50 75"));

    TouchstoneWriteOptions badUnit;
    badUnit.frequencyUnit = QStringLiteral("THz");
    CHECK(formatTouchstone(randomNetwork(1, 2, {50}, 1), badUnit, &error).isEmpty());

    NetworkData lateNoise = randomNetwork(2, 3, {50, 50}, 2);
    lateNoise.noiseFrequencies = {10e9};
    lateNoise.noise = {{1.0, 0.2, 10.0}};
    CHECK(formatTouchstone(lateNoise, {}, &error).isEmpty());
    CHECK_FALSE(formatTouchstone(lateNoise, v2, &error).isEmpty());

    NetworkData invalid;
    CHECK(formatTouchstone(invalid, {}, &error).isEmpty());
    CHECK_THAT(error.toStdString(), ContainsSubstring("Invalid network"));
}

TEST_CASE("Touchstone Z, Y, H and G data in version 1 are normalised to R", "[rfdata][touchstone]")
{
    const double r = 75;
    CMatrix s(2, 2);
    s << Complex(0.2, -0.1), Complex(0.05, 0.3), Complex(0.6, -0.2), Complex(-0.3, 0.1);
    const CMatrix z = *sToZ(s, {r, r});
    const CMatrix y = *sToY(s, {r, r});
    CMatrix h(2, 2);
    h << z.determinant() / z(1, 1), z(0, 1) / z(1, 1), -z(1, 0) / z(1, 1), 1.0 / z(1, 1);
    const CMatrix g = h.inverse();

    auto file = [&](const char *type, CMatrix m) {
        QString text = QStringLiteral("# MHz %1 RI R 75\n100").arg(QLatin1String(type));
        for (const auto &[i, j] : {std::pair(0, 0), std::pair(1, 0), std::pair(0, 1), std::pair(1, 1)})
            text += QStringLiteral(" %1 %2").arg(m(i, j).real(), 0, 'g', 17).arg(m(i, j).imag(), 0, 'g', 17);
        return text.toUtf8() + '\n';
    };
    CMatrix zn = z / r, yn = y * r, hn = h, gn = g;
    hn(0, 0) /= r;
    hn(1, 1) *= r;
    gn(0, 0) *= r;
    gn(1, 1) /= r;
    for (const auto &[type, m] : {std::pair("Z", zn), std::pair("Y", yn), std::pair("H", hn), std::pair("G", gn)}) {
        INFO(type);
        const auto result = parseTouchstone(file(type, m), 2);
        REQUIRE(result.network);
        CHECK(test::maxDifference(result.network->s.front(), s) < 1e-12);
        CHECK(result.network->z0 == std::vector<double>{r, r});
    }

    // version 2: not normalised
    QString v2 = QStringLiteral("[Version] 2.0\n# MHz H RI R 75\n[Number of Ports] 2\n[Two-Port Data Order] 12_21\n"
                                "[Number of Frequencies] 1\n[Network Data]\n100");
    for (const auto &[i, j] : {std::pair(0, 0), std::pair(0, 1), std::pair(1, 0), std::pair(1, 1)})
        v2 += QStringLiteral(" %1 %2").arg(h(i, j).real(), 0, 'g', 17).arg(h(i, j).imag(), 0, 'g', 17);
    const auto result = parseTouchstone(v2.toUtf8());
    REQUIRE(result.network);
    CHECK(test::maxDifference(result.network->s.front(), s) < 1e-12);
}

TEST_CASE("Touchstone version 2 matrix formats", "[rfdata][touchstone]")
{
    const char *lower = "[Version] 2.0\n"
                        "# GHz S RI R 50\n"
                        "[Number of Ports] 3\n"
                        "[Number of Frequencies] 1\n"
                        "[Matrix Format] Lower\n"
                        "[Network Data]\n"
                        "1 0.11 0 0.21 0.01 0.22 0 0.31 0.02 0.32 0.03 0.33 0\n"
                        "[End]\n"
                        "this is ignored\n";
    const auto result = parse(lower);
    REQUIRE(result.network);
    const CMatrix &s = result.network->s.front();
    CHECK(s(0, 0) == Complex(0.11, 0));
    CHECK(s(1, 0) == Complex(0.21, 0.01));
    CHECK(s(0, 1) == Complex(0.21, 0.01));
    CHECK(s(2, 1) == Complex(0.32, 0.03));
    CHECK(s(1, 2) == Complex(0.32, 0.03));
    CHECK(s(2, 2) == Complex(0.33, 0));

    const char *information = "[Version] 2.0\n# Hz S MA R 50\n[Number of Ports] 1\n[Begin Information]\n"
                              "anything [Network Data] here\n[End Information]\n[Number of Frequencies] 2\n"
                              "[Network Data]\n1 0.5 0\n2 0.5 90\n";
    const auto info = parse(information);
    REQUIRE(info.network);
    CHECK(info.network->frequencyCount() == 2);
}

TEST_CASE("Touchstone warnings for recoverable oddities", "[rfdata][touchstone]")
{
    SECTION("duplicate frequency")
    {
        const auto r = parse("# Hz S RI\n1 0.1 0\n2 0.2 0\n2 0.3 0\n3 0.4 0\n", 1);
        REQUIRE(r.network);
        CHECK(r.network->frequencyCount() == 3);
        CHECK(r.network->s[1](0, 0) == Complex(0.2, 0));
        REQUIRE(r.warnings.size() == 1);
        CHECK_THAT(r.warnings.front().toStdString(), ContainsSubstring("line 4") && ContainsSubstring("duplicate"));
    }
    SECTION("unsorted frequencies")
    {
        const auto r = parse("# Hz S RI\n3 0.3 0\n1 0.1 0\n2 0.2 0\n", 1);
        REQUIRE(r.network);
        CHECK(r.network->frequencies == std::vector<double>{1, 2, 3});
        CHECK(r.network->s[2](0, 0) == Complex(0.3, 0));
        CHECK(r.warnings.size() == 1);
    }
    SECTION("option line oddities and defaults")
    {
        const auto r = parse("# MHz S RI R 50 XYZ\n# GHz\n1 0.1 0\n", 1);
        REQUIRE(r.network);
        CHECK(r.network->frequencies.front() == 1e6);
        CHECK(r.warnings.size() == 2);
        const auto defaults = parse("1 0.5 90\n", 1);
        REQUIRE(defaults.network);
        CHECK(defaults.network->frequencies.front() == 1e9);
        CHECK(std::abs(defaults.network->s.front()(0, 0) - Complex(0, 0.5)) < 1e-15);
        CHECK(defaults.network->z0.front() == 50);
        CHECK(defaults.warnings.size() == 1);
    }
    SECTION("frequency count mismatch")
    {
        const auto r = parse("[Version] 2.0\n# Hz S RI\n[Number of Ports] 1\n[Number of Frequencies] 3\n"
                             "[Network Data]\n1 0.1 0\n2 0.1 0\n");
        REQUIRE(r.network);
        CHECK(r.warnings.size() == 1);
    }
    SECTION("two-port data order missing")
    {
        const auto r = parse("[Version] 2.0\n# Hz S RI\n[Number of Ports] 2\n[Number of Frequencies] 1\n"
                             "[Network Data]\n1 0.1 0 0.2 0 0.3 0 0.4 0\n");
        REQUIRE(r.network);
        CHECK(r.network->s.front()(0, 1) == Complex(0.2, 0));
        CHECK(r.warnings.size() == 1);
    }
}

TEST_CASE("Touchstone errors name the line", "[rfdata][touchstone]")
{
    struct Case {
        const char *text;
        int ports;
        const char *message;
    };
    const Case cases[] = {
        {"# GHz S RI R 50\n1 0.1 0.2\n2 0.1 x\n", 1, "line 3: 'x' is not a number"},
        {"# GHz S RI R 50\n1 0.1 0.2 0.3 0.4 0.5 0.6 0.7\n", 2, "line 2: incomplete data"},
        {"# GHz S RI R 50\n1 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8\n2 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8\n"
         "3 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8\n", 3, "line 4: a new frequency must start a line"},
        {"# GHz S RI R 50\n[Number of Ports] 2\n", 2, "line 2: keyword [number of ports] in a version 1 file"},
        {"[Version] 3.0\n", 0, "line 1: unsupported Touchstone version"},
        {"[Version] 2.0\n# GHz S RI\n[Network Data]\n1 0 0\n", 0, "line 3: [network data] before [Number of Ports]"},
        {"[Version] 2.0\n# GHz S RI\n[Number of Ports] 1\n1 0 0\n", 0, "line 4: data outside [Network Data]"},
        {"# GHz G RI R 50\n1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", 3, "G and H parameters are defined for two-ports only"},
        {"# GHz S RI R\n1 0 0\n", 1, "line 1: R without a value"},
        {"# GHz S RI R -5\n1 0 0\n", 1, "line 1: invalid reference resistance"},
        {"[Version 2.0\n", 0, "line 1: unterminated keyword"},
        {"! only a comment\n", 1, "no network data"},
        {"1 0 0\n# GHz S RI\n", 1, "line 2: option line after the data"},
        {"[Version] 2.0\n# GHz S RI\n[Number of Ports] 2\n[Reference] 50\n[Network Data]\n", 0,
         "line 4: [Reference] has fewer values than ports"},
        {"[Version] 2.0\n# GHz S RI\n[Number of Ports] 1\n[Reference] 50 50\n", 0, "line 4: [Reference] has more values"},
        {"[Version] 2.0\n# GHz S RI\n[Number of Ports] 2\n[Two-Port Data Order] 12_21\n[Network Data]\n"
         "1 0 0 0 0 0 0 0 0\n[Noise Data]\n1 2 3 4\n", 0, "line 8: noise data lines need 5 values"},
        {"[Version] 2.0\n# GHz S RI\n[Number of Ports] 2\n[Mixed-Mode Order] D2,1 C2,1\n", 0, "mixed-mode data is not supported"},
        {"1 0 0 0\n", 0, "cannot determine the number of ports"},
        {"# Hz S RI\n-1 0 0\n", 1, "line 2: negative frequency"},
        {"# Hz Z RI\n1 -1 0 0 0 0 0 -1 0\n", 2, "line 2: the parameters cannot be converted"},
    };
    for (const Case &c : cases) {
        INFO(c.text);
        const auto result = parse(c.text, c.ports);
        CHECK_FALSE(result.network);
        CHECK_THAT(result.error.toStdString(), ContainsSubstring(c.message));
    }
}

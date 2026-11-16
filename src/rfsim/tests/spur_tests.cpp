#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Connectivity.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "rfsim/Budget.h"
#include "rfsim/Spurs.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>

using namespace lvd;
using namespace lvd::rf;
using Catch::Approx;

namespace {

// Places parts and wires them (as in budget_tests.cpp).
struct SpurChain {
    QTemporaryDir dir;
    Session session;
    Project *project = nullptr;
    Document *document = nullptr;
    Schematic *sheet = nullptr;

    SpurChain()
    {
        QString error;
        project = session.newProject(dir.filePath(QStringLiteral("p")), QStringLiteral("p"), &error);
        REQUIRE(project);
        document = project->createDocument(DocumentType::RfSchematic, QStringLiteral("rx"), &error);
        REQUIRE(document);
        sheet = document->schematic();
    }

    QString place(const char *key, QPoint at, const QMap<QString, QString> &parameters = {})
    {
        const QString definitionKey = QString::fromLatin1(key);
        const DefinitionPtr definition = project->resolve(*sheet, definitionKey).definition;
        REQUIRE(definition);
        const QString id = sheet->addComponent(definitionKey, definition.get(), at);
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            sheet->setParameter(id, it.key(), it.value());
        return id;
    }

    QPoint pin(const QString &id, const QString &name)
    {
        const PlacedComponent *component = sheet->component(id);
        const DefinitionPtr definition = project->resolve(*sheet, component->definition).definition;
        const SymbolPin *symbolPin = definition->symbol.pin(name);
        REQUIRE(symbolPin);
        return pinPosition(*component, *symbolPin);
    }

    void wire(const QString &a, const char *pinA, const QString &b, const char *pinB)
    {
        const QPoint start = pin(a, QString::fromLatin1(pinA)), end = pin(b, QString::fromLatin1(pinB));
        sheet->addWire({start, QPoint(end.x(), start.y()), end});
    }

    BudgetRun run(const BudgetAnalysisSetup &setup = {})
    {
        BudgetRun result = runBudgetAnalysis(*project, *document, setup);
        INFO((result.errors + result.chain.errors + result.budget.errors).join(QLatin1Char('\n')).toStdString());
        return result;
    }

    QString writeFile(const QString &name, const QByteArray &content)
    {
        const QString path = QDir(project->directory()).absoluteFilePath(name);
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(content);
        return path;
    }
};

const QByteArray TestTable = "! test mixer, levels at -10 dBm RF\n"
                             "reference_power -10\n"
                             "2 1 60   # 2RF - LO\n"
                             "2 2 50\n";

const Spur *find(const SpurResult &result, Spur::Kind kind, int m, int n, const QString &mixer = {})
{
    for (const Spur &spur : result.spurs) {
        if (spur.kind == kind && spur.m == m && spur.n == n && (mixer.isEmpty() || spur.mixer == mixer))
            return &spur;
    }
    return nullptr;
}

const Spur *findResponse(const SpurResult &result, double inputFrequency, const QString &mixer = {})
{
    for (const Spur &spur : result.spurs) {
        if (spur.kind != Spur::Kind::Product && spur.kind != Spur::Kind::LoLeakage
            && std::abs(spur.inputFrequency - inputFrequency) < 1 && (mixer.isEmpty() || spur.mixer == mixer))
            return &spur;
    }
    return nullptr;
}

const Spur *findLeakage(const SpurResult &result, bool atInput)
{
    for (const Spur &spur : result.spurs) {
        if (spur.kind == Spur::Kind::LoLeakage && std::isfinite(spur.inputFrequency) == atInput)
            return &spur;
    }
    return nullptr;
}

QMap<QString, QString> mixerParameters(const QString &table = {})
{
    return {{QStringLiteral("gain"), QStringLiteral("-7")},       {QStringLiteral("iip3"), QStringLiteral("15")},
            {QStringLiteral("lo_rf_isolation"), QStringLiteral("35")}, {QStringLiteral("lo_if_isolation"), QStringLiteral("30")},
            {QStringLiteral("rf_if_isolation"), QStringLiteral("25")}, {QStringLiteral("spur_table"), table}};
}

// The ideal filter's stopband: `rejection` plus the insertion loss `il` (as
// implemented in ElementModels.cpp).
constexpr double StopbandLoss = 60 + 1;

// Source 2.45 GHz -30 dBm, ideal band-pass (il 1, rejection 60), mixer with LO
// 2.38 GHz, 3 dB attenuator, termination.
struct Receiver : SpurChain {
    QString mixerId;

    explicit Receiver(const QString &table)
    {
        const QString src = place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("2.45GHz")},
                                                               {QStringLiteral("power"), QStringLiteral("-30")}});
        const QString bpf = place("rf-system:filter", {200, 0},
                                  {{QStringLiteral("fc"), QStringLiteral("2.45GHz")}, {QStringLiteral("bw"), QStringLiteral("100MHz")},
                                   {QStringLiteral("response"), QStringLiteral("ideal")}, {QStringLiteral("il"), QStringLiteral("1")},
                                   {QStringLiteral("rejection"), QStringLiteral("60")}});
        mixerId = place("rf-system:mixer", {400, 0}, mixerParameters(table));
        const QString &mixer = mixerId;
        const QString lo = place("rf-system:lo", {400, 200}, {{QStringLiteral("freq"), QStringLiteral("2.38GHz")},
                                                              {QStringLiteral("power"), QStringLiteral("13")}});
        const QString att = place("rf-system:attenuator", {600, 0}, {{QStringLiteral("il"), QStringLiteral("3")}});
        const QString term = place("rf-system:termination", {800, 0});
        wire(src, "out", bpf, "in");
        wire(bpf, "out", mixer, "rf");
        wire(lo, "out", mixer, "lo");
        wire(mixer, "if", att, "in");
        wire(att, "out", term, "p");
    }
};

} // namespace

TEST_CASE("spur tables are read as rows or as a matrix")
{
    QString error;
    const auto rows = parseSpurTable(TestTable, &error);
    REQUIRE(rows);
    CHECK(rows->referencePowerDbm == -10);
    REQUIRE(rows->entries.size() == 2);
    CHECK(rows->levelDbc(2, 1, -10) == Approx(60));
    CHECK(rows->levelDbc(-2, -1, -10) == Approx(60)); // signs do not matter
    CHECK_FALSE(rows->levelDbc(3, 1, -10));

    const QByteArray matrix = "# Datasheet layout: RF harmonics down, LO harmonics across\n"
                              "reference_power -10 dBm\n"
                              "lo_harmonics 0 1 2 3\n"
                              "0  -   20  30  25\n"
                              "1  25  0   35  15\n"
                              "2  70  60  65  --\n";
    const auto table = parseSpurTable(matrix, &error);
    REQUIRE(table);
    CHECK(table->levelDbc(0, 2, -10) == Approx(30));
    CHECK(table->levelDbc(1, 3, -10) == Approx(15));
    CHECK(table->levelDbc(2, 2, -10) == Approx(65));
    CHECK_FALSE(table->levelDbc(2, 3, -10));
    CHECK_FALSE(table->levelDbc(0, 0, -10));
}

TEST_CASE("spur table errors name the line")
{
    QString error;
    CHECK_FALSE(parseSpurTable("! levels\n2 1\n", &error));
    CHECK(error.startsWith(QStringLiteral("line 2:")));
    CHECK_FALSE(parseSpurTable("reference_power -10\nfoo 3\n", &error));
    CHECK(error.contains(QStringLiteral("line 2")));
    CHECK(error.contains(QStringLiteral("foo")));
    CHECK_FALSE(parseSpurTable("lo_harmonics 0 1\n1 20 0\n2 70\n", &error));
    CHECK(error.startsWith(QStringLiteral("line 3:")));
    CHECK_FALSE(parseSpurTable("2 1 abc\n", &error));
    CHECK(error.startsWith(QStringLiteral("line 1:")));
    CHECK_FALSE(parseSpurTable("reference_power\n", &error));
    CHECK(error.startsWith(QStringLiteral("line 1:")));
    CHECK_FALSE(parseSpurTable("# nothing\n", &error));
    CHECK_FALSE(error.isEmpty());
    CHECK_FALSE(readSpurTable(QStringLiteral("no/such/file.spt"), &error));
    CHECK(error.contains(QStringLiteral("cannot read")));
}

TEST_CASE("spur table levels scale with the input power by |m| - 1")
{
    const auto table = parseSpurTable("reference_power -10\n1 3 15\n2 1 60\n3 1 70\n");
    REQUIRE(table);
    CHECK(table->levelDbc(1, 3, -30) == Approx(15));       // first order in RF: no change
    CHECK(table->levelDbc(2, 1, -20) == Approx(70));       // 1 dB per dB
    CHECK(table->levelDbc(2, 1, 0) == Approx(50));
    CHECK(table->levelDbc(3, 1, -20) == Approx(90));       // 2 dB per dB
}

TEST_CASE("estimated spur levels follow the documented rule")
{
    const double iip3 = 15, p = -10; // 25 dB below IIP3
    CHECK(estimatedSpurDbc(1, 1, p, iip3) == 0);
    CHECK(estimatedSpurDbc(1, -1, p, iip3) == 0);
    CHECK(estimatedSpurDbc(2, 1, p, iip3) == Approx(25 + 10));
    CHECK(estimatedSpurDbc(3, 1, p, iip3) == Approx(50));
    CHECK(estimatedSpurDbc(1, 3, p, iip3) == Approx(20 * std::log10(3.0)));
    CHECK(estimatedSpurDbc(1, 2, p, iip3) == Approx(20 * std::log10(2.0) + 25));
    CHECK(estimatedSpurDbc(2, 0, p, iip3) == Approx(25 + 10 + 25));

    // More suppression with higher order, even harmonics more than odd ones.
    for (int n = 0; n <= 4; ++n) {
        for (int m = 1; m <= 4; ++m)
            CHECK(estimatedSpurDbc(m + 1, n, p, iip3) > estimatedSpurDbc(m, n, p, iip3));
    }
    for (int m = 1; m <= 4; ++m) {
        CHECK(estimatedSpurDbc(m, 3, p, iip3) > estimatedSpurDbc(m, 1, p, iip3));
        CHECK(estimatedSpurDbc(m, 5, p, iip3) > estimatedSpurDbc(m, 3, p, iip3));
        CHECK(estimatedSpurDbc(m, 2, p, iip3) > estimatedSpurDbc(m, 3, p, iip3));
        CHECK(estimatedSpurDbc(m, 4, p, iip3) > estimatedSpurDbc(m, 5, p, iip3));
    }
    // Scaling with the input like a table.
    CHECK(estimatedSpurDbc(3, 1, p - 5, iip3) - estimatedSpurDbc(3, 1, p, iip3) == Approx(10));
    // Above IIP3 the RF term does not become negative.
    CHECK(estimatedSpurDbc(3, 1, 20, iip3) == Approx(0));
}

TEST_CASE("a single-conversion receiver: image, half-IF, products and LO leakage")
{
    Receiver c(QString{});
    const QString table = c.writeFile(QStringLiteral("mixer.spt"), TestTable);
    c.sheet->setParameter(c.mixerId, QStringLiteral("spur_table"), table); // absolute path
    const BudgetRun run = c.run();
    REQUIRE(run.ok());
    const SpurResult &s = run.spurs;
    INFO(s.errors.join(QLatin1Char('\n')).toStdString());
    INFO(s.warnings.join(QLatin1Char('\n')).toStdString());
    REQUIRE(s.ok());
    CHECK(s.wantedOutputFrequency == Approx(70e6));
    const double wanted = -30 - 1 - 7 - 3;
    CHECK(s.wantedOutputDbm == Approx(wanted).margin(1e-9));
    const double mixerInput = -31;

    // Image at f_LO - f_IF, outside the filter: the stopband loss against 1 dB passband loss.
    const Spur *image = findResponse(s, 2.31e9);
    REQUIRE(image);
    CHECK(image->kind == Spur::Kind::Image);
    CHECK(image->m == 1);
    CHECK(std::abs(image->n) == 1);
    CHECK(image->rejectionDb == Approx(StopbandLoss - 1).margin(1e-6));
    CHECK(image->frequency == Approx(70e6));
    CHECK(image->inBand);
    CHECK_FALSE(image->estimated);

    // Half-IF at f_LO + f_IF/2, inside the filter: only the 2x2 table level.
    const Spur *halfIf = findResponse(s, 2.38e9 + 35e6);
    REQUIRE(halfIf);
    CHECK(halfIf->kind == Spur::Kind::HalfIf);
    CHECK(halfIf->rejectionDb == Approx(50 + (-10 - mixerInput)).margin(1e-6));

    // 2RF - LO from the table, scaled to -31 dBm, through the flat attenuator.
    const Spur *product = find(s, Spur::Kind::Product, 2, -1);
    REQUIRE(product);
    CHECK(product->frequency == Approx(2 * 2.45e9 - 2.38e9));
    CHECK(product->levelDbc == Approx(60 + 21).margin(1e-6));
    CHECK(product->levelDbm == Approx(wanted - 81).margin(1e-6));
    CHECK_FALSE(product->inBand);
    CHECK_FALSE(product->estimated);

    // A product missing from the table is estimated, with a warning.
    const Spur *estimated = find(s, Spur::Kind::Product, 3, -2);
    REQUIRE(estimated);
    CHECK(estimated->estimated);
    CHECK(estimated->levelDbc == Approx(estimatedSpurDbc(3, 2, mixerInput, 15)).margin(1e-6));
    CHECK(s.warnings.join(QString()).contains(QStringLiteral("estimated")));

    // RF feedthrough from rf_if_isolation: dBc = isolation + conversion gain.
    const Spur *feedthrough = find(s, Spur::Kind::Product, 1, 0);
    REQUIRE(feedthrough);
    CHECK(feedthrough->levelDbc == Approx(25 - 7).margin(1e-6));

    // LO leakage: 13 dBm - 30 dB isolation - 3 dB to the output; 13 - 35 - stopband loss to the input.
    const Spur *out = findLeakage(s, false);
    REQUIRE(out);
    CHECK(out->frequency == Approx(2.38e9));
    CHECK(out->levelDbm == Approx(13 - 30 - 3).margin(1e-6));
    CHECK(out->levelDbc == Approx(wanted - (13 - 30 - 3)).margin(1e-6));
    const Spur *in = findLeakage(s, true);
    REQUIRE(in);
    CHECK(in->levelDbm == Approx(13 - 35 - StopbandLoss).margin(1e-6));
    CHECK(in->frequency == Approx(2.38e9));

    // Sorted: in-band first, then strongest first.
    REQUIRE(!s.spurs.empty());
    CHECK(s.spurs.front().inBand);
    bool seenOutOfBand = false;
    for (size_t i = 0; i < s.spurs.size(); ++i) {
        if (!s.spurs[i].inBand)
            seenOutOfBand = true;
        CHECK(!(seenOutOfBand && s.spurs[i].inBand));
        if (i > 0 && s.spurs[i].inBand == s.spurs[i - 1].inBand)
            CHECK(s.spurs[i].levelDbc >= s.spurs[i - 1].levelDbc);
    }

    // Only spurs above the limit are listed.
    ChainEvaluator evaluator(run.chain, c.project->directory());
    SpurOptions options;
    options.minimumLevelDbc = 60;
    const SpurResult limited = analyseSpurs(evaluator, options);
    for (const Spur &spur : limited.spurs)
        CHECK(spur.levelDbc <= 60);
    CHECK(limited.spurs.size() < s.spurs.size());
}

TEST_CASE("relative spur tables resolve against the project; unreadable ones fall back to estimates")
{
    Receiver c(QStringLiteral("tables/mixer.spt"));
    QDir(c.project->directory()).mkpath(QStringLiteral("tables"));
    c.writeFile(QStringLiteral("tables/mixer.spt"), TestTable);
    BudgetAnalysisSetup setup;
    setup.spurs = false;
    const BudgetRun run = c.run(setup);
    REQUIRE(run.ok());

    ChainEvaluator evaluator(run.chain, c.project->directory());
    SpurOptions options;
    options.projectDirectory = c.project->directory();
    const SpurResult resolved = analyseSpurs(evaluator, options);
    REQUIRE(resolved.ok());
    const Spur *product = find(resolved, Spur::Kind::Product, 2, -1);
    REQUIRE(product);
    CHECK_FALSE(product->estimated);
    CHECK(product->levelDbc == Approx(81).margin(1e-6));

    options.projectDirectory = c.dir.filePath(QStringLiteral("elsewhere"));
    const SpurResult missing = analyseSpurs(evaluator, options);
    REQUIRE(missing.ok());
    CHECK(missing.warnings.join(QString()).contains(QStringLiteral("not used")));
    const Spur *estimated = find(missing, Spur::Kind::Product, 2, -1);
    REQUIRE(estimated);
    CHECK(estimated->estimated);
    CHECK(estimated->levelDbc == Approx(estimatedSpurDbc(2, 1, -31, 15)).margin(1e-6));
}

TEST_CASE("a product close to the wanted output is in band")
{
    SpurChain c;
    const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("2GHz")}});
    const QString mixer = c.place("rf-system:mixer", {200, 0}, mixerParameters());
    const QString lo = c.place("rf-system:lo", {200, 200}, {{QStringLiteral("freq"), QStringLiteral("1.001GHz")}});
    const QString term = c.place("rf-system:termination", {400, 0});
    c.wire(src, "out", mixer, "rf");
    c.wire(lo, "out", mixer, "lo");
    c.wire(mixer, "if", term, "p");
    BudgetAnalysisSetup setup;
    setup.spurs = false;
    const BudgetRun run = c.run(setup);
    REQUIRE(run.ok());

    // 2 RF - 3 LO = 0.997 GHz, 2 MHz from the IF of 0.999 GHz.
    ChainEvaluator evaluator(run.chain, c.project->directory());
    SpurOptions options;
    options.bandwidth = 10e6;
    const SpurResult wide = analyseSpurs(evaluator, options);
    REQUIRE(wide.ok());
    const Spur *product = find(wide, Spur::Kind::Product, 2, -3);
    REQUIRE(product);
    CHECK(product->frequency == Approx(0.997e9));
    CHECK(product->inBand);
    CHECK(product->estimated);
    options.bandwidth = 1e6;
    const Spur *narrow = find(analyseSpurs(evaluator, options), Spur::Kind::Product, 2, -3);
    REQUIRE(narrow);
    CHECK_FALSE(narrow->inBand);
}

TEST_CASE("double conversion: products of the first mixer reach the output translated")
{
    SpurChain c;
    const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("2.45GHz")},
                                                             {QStringLiteral("power"), QStringLiteral("-30")}});
    const QString mx1 = c.place("rf-system:mixer", {200, 0}, mixerParameters());
    const QString lo1 = c.place("rf-system:lo", {200, 200}, {{QStringLiteral("freq"), QStringLiteral("2GHz")}});
    const QString att = c.place("rf-system:attenuator", {400, 0}, {{QStringLiteral("il"), QStringLiteral("3")}});
    const QString mx2 = c.place("rf-system:mixer", {600, 0}, mixerParameters());
    const QString lo2 = c.place("rf-system:lo", {600, 200}, {{QStringLiteral("freq"), QStringLiteral("380MHz")}});
    const QString term = c.place("rf-system:termination", {800, 0});
    c.wire(src, "out", mx1, "rf");
    c.wire(lo1, "out", mx1, "lo");
    c.wire(mx1, "if", att, "in");
    c.wire(att, "out", mx2, "rf");
    c.wire(lo2, "out", mx2, "lo");
    c.wire(mx2, "if", term, "p");

    const BudgetRun run = c.run();
    REQUIRE(run.ok());
    const SpurResult &s = run.spurs;
    REQUIRE(s.ok());
    CHECK(s.wantedOutputFrequency == Approx(70e6));
    const QString first = run.chain.stages[0].path;
    const QString second = run.chain.stages[2].path;

    // The sum product of mixer 1 (4.45 GHz) leaves mixer 2 at 4.45 - 0.38 GHz.
    const Spur *sum = find(s, Spur::Kind::Product, 1, 1, first);
    REQUIRE(sum);
    CHECK(sum->frequency == Approx(4.07e9));
    CHECK(sum->levelDbc == Approx(0).margin(1e-6)); // the same conversion gain as the wanted signal
    const Spur *product = find(s, Spur::Kind::Product, 2, -1, first);
    REQUIRE(product);
    CHECK(product->frequency == Approx(2.9e9 - 0.38e9));

    // Images: mixer 1 at 2.0 - 0.45 GHz; mixer 2 at 0.31 GHz, mapped back through mixer 1 to 2.31 GHz.
    const Spur *image1 = findResponse(s, 1.55e9, first);
    REQUIRE(image1);
    CHECK(image1->kind == Spur::Kind::Image);
    const Spur *image2 = findResponse(s, 2.31e9, second);
    REQUIRE(image2);
    CHECK(image2->kind == Spur::Kind::Image);
    CHECK(image2->rejectionDb == Approx(0).margin(1e-6)); // no filtering in this chain

    // LO 1 leaks through mixer 2, which translates it to 2.0 - 0.38 GHz.
    bool found = false;
    for (const Spur &spur : s.spurs) {
        if (spur.kind == Spur::Kind::LoLeakage && spur.mixer == first && !std::isfinite(spur.inputFrequency)) {
            found = true;
            CHECK(spur.frequency == Approx(1.62e9));
            CHECK(spur.levelDbm == Approx(13 - 30 - 3 - 7).margin(1e-6));
        }
    }
    CHECK(found);
}

TEST_CASE("a chain without mixers has no spurs")
{
    SpurChain c;
    const QString src = c.place("rf-system:source", {0, 0}, {{QStringLiteral("freq"), QStringLiteral("1GHz")}});
    const QString amp = c.place("rf-system:amplifier", {200, 0});
    const QString term = c.place("rf-system:termination", {400, 0});
    c.wire(src, "out", amp, "in");
    c.wire(amp, "out", term, "p");
    const BudgetRun run = c.run();
    REQUIRE(run.ok());
    ChainEvaluator evaluator(run.chain, c.project->directory());
    const SpurResult result = analyseSpurs(evaluator, SpurOptions{});
    CHECK(result.ok());
    CHECK(result.spurs.empty());
    CHECK(result.warnings.isEmpty());
    CHECK(result.wantedOutputFrequency == Approx(1e9));
}

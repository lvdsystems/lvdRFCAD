#include "rfsim/SignalChain.h"

#include "rfdata/NetworkMath.h"

#include <QHash>
#include <QMap>
#include <QSet>

#include <cmath>

namespace lvd::rf {

namespace {

const QString GroundNet = QStringLiteral("0");

double undb20(double db)
{
    return std::pow(10.0, db / 20.0);
}

// The model type of a component: block name, element name, "sparameters" or empty.
QString kindOf(const FlatComponent &component)
{
    const ComponentDefinition &definition = *component.definition;
    if (const ModelDefinition *model = definition.model(QStringLiteral("rf-behavioural")))
        return model->data.value(QStringLiteral("block")).toString();
    if (const ModelDefinition *model = definition.model(QStringLiteral("rf-element")))
        return model->data.value(QStringLiteral("element")).toString();
    if (definition.model(QStringLiteral("sparameters")))
        return QStringLiteral("sparameters");
    return {};
}

bool isEndKind(const QString &kind)
{
    static const QStringList ends = {QStringLiteral("termination"), QStringLiteral("adc"), QStringLiteral("port"),
                                     QStringLiteral("antenna")};
    return ends.contains(kind);
}

bool isOnePortKind(const QString &kind)
{
    static const QStringList oneport = {QStringLiteral("stub_open"), QStringLiteral("stub_short"),
                                        QStringLiteral("source"), QStringLiteral("lo")};
    return oneport.contains(kind);
}

// Blocks with a preferred direction; entering at their output is suspicious.
bool isDirectional(const QString &kind)
{
    return kind == QLatin1String("amplifier") || kind == QLatin1String("isolator") || kind == QLatin1String("adc");
}

double parameter(const FlatComponent &component, const char *name, double fallback = NotSet)
{
    return component.parameterValues.value(QString::fromLatin1(name), fallback);
}

int pinIndex(const FlatComponent &component, const QString &pin)
{
    return static_cast<int>(component.definition->pins.indexOf(pin));
}

// Possible exit pins when entering a component at `entry`; the first is the default.
QList<int> exitPins(const FlatComponent &component, const QString &kind, int entry)
{
    const QStringList &pins = component.definition->pins;
    const auto index = [&](const char *name) { return pinIndex(component, QString::fromLatin1(name)); };
    const QString in = pins.value(entry);
    if (kind == QLatin1String("mixer")) {
        if (in == QLatin1String("rf"))
            return {index("if")};
        if (in == QLatin1String("if"))
            return {index("rf")};
        return {}; // entering at the LO pin is not a signal path
    }
    if (kind == QLatin1String("splitter")) {
        if (in == QLatin1String("in"))
            return {index("out1"), index("out2")};
        return {index("in")};
    }
    if (kind == QLatin1String("combiner")) {
        if (in == QLatin1String("out"))
            return {index("in1"), index("in2")};
        return {index("out")};
    }
    if (kind == QLatin1String("coupler")) {
        if (in == QLatin1String("in"))
            return {index("out"), index("cpl")};
        if (in == QLatin1String("out"))
            return {index("in"), index("iso")};
        if (in == QLatin1String("cpl"))
            return {index("iso"), index("in")};
        return {index("cpl"), index("out")};
    }
    if (kind == QLatin1String("switch_spdt")) {
        const QString state = component.parameterText.value(QStringLiteral("state"));
        if (in == QLatin1String("com"))
            return state == QLatin1String("t2") ? QList<int>{index("t2"), index("t1")} : QList<int>{index("t1"), index("t2")};
        return {index("com")};
    }
    if (kind == QLatin1String("circulator")) {
        return {static_cast<int>((entry + 1) % 3)};
    }
    if (pins.size() == 2)
        return {1 - entry};
    // Generic N-ports: every other pin, the next one first.
    QList<int> result;
    for (int k = 1; k < pins.size(); ++k)
        result.append(static_cast<int>((entry + k) % pins.size()));
    return result;
}

struct Attachment {
    int component;
    int pin;
};

class Tracer
{
public:
    Tracer(SignalChain &chain) : m_chain(chain)
    {
        const auto &components = m_chain.netlist.components;
        for (int c = 0; c < static_cast<int>(components.size()); ++c) {
            const FlatComponent &component = components[static_cast<size_t>(c)];
            m_kinds.append(kindOf(component));
            for (int p = 0; p < component.definition->pins.size(); ++p)
                m_nets[component.pinNets.value(component.definition->pins.at(p))].append(Attachment{c, p});
        }
    }

    const FlatComponent &component(int index) const { return m_chain.netlist.components[static_cast<size_t>(index)]; }
    QString kind(int index) const { return m_kinds.at(index); }

    int find(const QString &path) const
    {
        for (int c = 0; c < static_cast<int>(m_chain.netlist.components.size()); ++c) {
            if (component(c).path == path)
                return c;
        }
        return -1;
    }

    int findStart(const QString &requested)
    {
        if (!requested.isEmpty()) {
            const int c = find(requested);
            if (c < 0)
                m_chain.errors.append(QStringLiteral("start component %1 not found").arg(requested));
            return c;
        }
        int port1 = -1, antenna = -1;
        QList<int> sources;
        for (int c = 0; c < m_kinds.size(); ++c) {
            if (m_kinds.at(c) == QLatin1String("source"))
                sources.append(c);
            else if (m_kinds.at(c) == QLatin1String("port") && parameter(component(c), "num") == 1)
                port1 = c;
            else if (m_kinds.at(c) == QLatin1String("antenna") && antenna < 0)
                antenna = c;
        }
        if (!sources.isEmpty()) {
            if (sources.size() > 1)
                m_chain.warnings.append(QStringLiteral("several sources; the chain starts at %1").arg(component(sources.first()).path));
            return sources.first();
        }
        if (port1 >= 0)
            return port1;
        if (antenna >= 0)
            return antenna;
        m_chain.errors.append(QStringLiteral("no start for the signal chain: place a source, port 1 or an antenna"));
        return -1;
    }

    // Walks from the start; `explicitPath` (without the start) selects stages and branches.
    void walk(int start, QStringList explicitPath)
    {
        QSet<int> visited{start};
        QString net = component(start).pinNets.value(component(start).definition->pins.first());
        int previous = start;
        for (int step = 0; step < 1000; ++step) {
            if (net == GroundNet || net.isEmpty()) {
                m_chain.errors.append(QStringLiteral("the signal path reaches ground after %1").arg(component(previous).path));
                return;
            }
            QList<Attachment> candidates;
            for (const Attachment &a : m_nets.value(net)) {
                if (a.component == previous || visited.contains(a.component))
                    continue;
                const QString k = kind(a.component);
                const FlatComponent &c = component(a.component);
                // Shunt parts: one-pin elements and two-pin parts with the other pin on ground.
                const bool otherPinGrounded = c.definition->pins.size() == 2
                                              && c.pinNets.value(c.definition->pins.at(1 - a.pin)) == GroundNet;
                if (isOnePortKind(k) || otherPinGrounded || k.isEmpty()) {
                    if (!k.isEmpty())
                        m_chain.warnings.append(QStringLiteral("%1 is a shunt part on the signal path; the budget "
                                                               "ignores it").arg(c.path));
                    continue;
                }
                if (k == QLatin1String("mixer") && c.definition->pins.value(a.pin) == QLatin1String("lo"))
                    continue;
                candidates.append(a);
            }

            // Choose the next component.
            Attachment next{-1, -1};
            QString wantedExit;
            if (!explicitPath.isEmpty()) {
                const QString entry = explicitPath.takeFirst();
                const QString reference = entry.section(QLatin1Char('.'), 0, 0);
                wantedExit = entry.section(QLatin1Char('.'), 1);
                for (const Attachment &a : candidates) {
                    if (component(a.component).path == reference)
                        next = a;
                }
                if (next.component < 0) {
                    m_chain.errors.append(QStringLiteral("%1 in the path is not connected to %2")
                                              .arg(reference, component(previous).path));
                    return;
                }
            } else {
                if (candidates.isEmpty()) {
                    m_chain.warnings.append(QStringLiteral("the signal path ends open after %1").arg(component(previous).path));
                    return;
                }
                // Ends win over stages only if nothing else continues; prefer a stage.
                QList<Attachment> stages, ends;
                for (const Attachment &a : candidates)
                    (isEndKind(kind(a.component)) ? ends : stages).append(a);
                const QList<Attachment> &pool = stages.isEmpty() ? ends : stages;
                next = pool.first();
                for (const Attachment &a : pool) {
                    if (component(a.component).path < component(next.component).path)
                        next = a;
                }
                if (pool.size() > 1)
                    m_chain.warnings.append(QStringLiteral("the path branches after %1; it continues with %2 (give an "
                                                           "explicit path to choose)")
                                                .arg(component(previous).path, component(next.component).path));
            }

            const QString k = kind(next.component);
            const FlatComponent &c = component(next.component);
            if (isEndKind(k)) {
                m_chain.end = c.path;
                m_chain.endKind = k;
                return;
            }
            const QList<int> exits = exitPins(c, k, next.pin);
            if (exits.isEmpty() || exits.first() < 0) {
                m_chain.errors.append(QStringLiteral("the signal cannot pass %1 from pin %2")
                                          .arg(c.path, c.definition->pins.value(next.pin)));
                return;
            }
            int exit = exits.first();
            if (!wantedExit.isEmpty()) {
                exit = pinIndex(c, wantedExit);
                if (exit < 0 || exit == next.pin) {
                    m_chain.errors.append(QStringLiteral("%1 has no exit pin %2").arg(c.path, wantedExit));
                    return;
                }
            } else if (exits.size() > 1 && k != QLatin1String("switch_spdt") && k != QLatin1String("coupler")) {
                m_chain.warnings.append(QStringLiteral("%1: the path continues at pin %2 (give \"%1.<pin>\" to choose)")
                                            .arg(c.path, c.definition->pins.value(exit)));
            }
            if (isDirectional(k) && c.definition->pins.value(next.pin) != QLatin1String("in"))
                m_chain.warnings.append(QStringLiteral("%1 is passed backwards").arg(c.path));

            ChainStage stage;
            stage.component = &c;
            stage.path = c.path;
            stage.name = c.definition->name;
            stage.kind = k;
            stage.inputPin = next.pin;
            stage.outputPin = exit;
            if (k == QLatin1String("mixer")) {
                stage.translation = ChainStage::Translation::Mixer;
                stage.upConversion = c.definition->pins.value(next.pin) == QLatin1String("if");
                traceLo(next.component, stage);
            } else if (k == QLatin1String("multiplier")) {
                stage.translation = ChainStage::Translation::Multiplier;
                stage.factor = std::max(1, static_cast<int>(std::lround(parameter(c, "factor", 1))));
            }
            m_chain.stages.push_back(stage);
            visited.insert(next.component);
            previous = next.component;
            net = c.pinNets.value(c.definition->pins.at(exit));
        }
        m_chain.errors.append(QStringLiteral("the signal path does not end"));
    }

    // Follows the LO pin back to an LO (or source) block through two-port
    // blocks, multiplying the frequency and adding gains in dB.
    void traceLo(int mixer, ChainStage &stage)
    {
        const FlatComponent &m = component(mixer);
        QString net = m.pinNets.value(QStringLiteral("lo"));
        QSet<int> visited{mixer};
        double gainDb = 0;
        int factor = 1;
        for (int step = 0; step < 100; ++step) {
            int found = -1, foundPin = -1;
            for (const Attachment &a : m_nets.value(net)) {
                if (visited.contains(a.component))
                    continue;
                const QString k = kind(a.component);
                if (k == QLatin1String("lo") || k == QLatin1String("source")) {
                    const FlatComponent &lo = component(a.component);
                    stage.loSource = lo.path;
                    stage.loFrequency = parameter(lo, "freq") * factor;
                    stage.loPowerDbm = parameter(lo, "power") + gainDb;
                    return;
                }
                if (component(a.component).definition->pins.size() == 2 && !isEndKind(k) && !isOnePortKind(k)
                    && !k.isEmpty()) {
                    found = a.component;
                    foundPin = a.pin;
                }
            }
            if (found < 0)
                break;
            const FlatComponent &c = component(found);
            const QString k = kind(found);
            if (k == QLatin1String("multiplier")) {
                factor *= std::max(1, static_cast<int>(std::lround(parameter(c, "factor", 1))));
                gainDb += parameter(c, "gain", 0);
            } else if (k == QLatin1String("amplifier")) {
                gainDb += parameter(c, "gain", 0);
            } else if (k == QLatin1String("attenuator") || k == QLatin1String("filter") || k == QLatin1String("isolator")
                       || k == QLatin1String("phase_shifter") || k == QLatin1String("cable")) {
                gainDb -= parameter(c, "il", 0); // passband loss; good enough for the LO drive level
            }
            visited.insert(found);
            net = c.pinNets.value(c.definition->pins.at(1 - foundPin));
        }
        m_chain.errors.append(QStringLiteral("%1: no LO block drives the lo pin").arg(m.path));
    }

private:
    SignalChain &m_chain;
    QStringList m_kinds;
    QHash<QString, QList<Attachment>> m_nets;
};

// Translated frequency for a stage; the same rule for the wanted signal and for spurs.
double translate(const ChainStage &stage, double f)
{
    switch (stage.translation) {
    case ChainStage::Translation::None:
        return f;
    case ChainStage::Translation::Multiplier:
        return f * stage.factor;
    case ChainStage::Translation::Mixer:
        if (stage.sumProduct)
            return f + stage.loFrequency;
        return std::abs(f - stage.loFrequency);
    }
    return f;
}

} // namespace

int SignalChain::stageIndex(const QString &path) const
{
    for (size_t i = 0; i < stages.size(); ++i) {
        if (stages[i].path == path)
            return static_cast<int>(i);
    }
    return -1;
}

SignalChain traceChain(const Netlist &netlist, const ChainOptions &options)
{
    SignalChain chain;
    chain.netlist = netlist;
    for (const QString &message : netlist.errors)
        chain.errors.append(message);
    if (!chain.errors.isEmpty())
        return chain;

    Tracer tracer(chain);
    // The first path entry is the start component; with `start` also given,
    // a leading entry naming the same component is not repeated.
    QStringList explicitPath = options.path;
    QString requestedStart = options.start;
    if (!explicitPath.isEmpty()) {
        const QString first = explicitPath.first().section(QLatin1Char('.'), 0, 0);
        if (requestedStart.isEmpty() || first == requestedStart) {
            requestedStart = first;
            explicitPath.removeFirst();
        }
    }
    const int start = tracer.findStart(requestedStart);
    if (start < 0)
        return chain;
    const FlatComponent &startComponent = tracer.component(start);
    chain.start = startComponent.path;
    chain.startKind = tracer.kind(start);
    if (chain.startKind != QLatin1String("source") && chain.startKind != QLatin1String("port")
        && chain.startKind != QLatin1String("antenna"))
        chain.warnings.append(QStringLiteral("%1 is not a source, port or antenna").arg(chain.start));

    chain.inputFrequency = std::isfinite(options.frequency) ? options.frequency : parameter(startComponent, "freq");
    chain.inputPowerDbm = std::isfinite(options.powerDbm) ? options.powerDbm : parameter(startComponent, "power");
    chain.sourceZ0 = parameter(startComponent, "z0", SolverZ0);
    if (chain.startKind == QLatin1String("antenna"))
        chain.sourceTemperature = parameter(startComponent, "temp", T0);
    if (!std::isfinite(chain.inputFrequency))
        chain.errors.append(QStringLiteral("set the input frequency: %1 has none").arg(chain.start));
    if (!std::isfinite(chain.inputPowerDbm)) {
        chain.inputPowerDbm = -30;
        chain.warnings.append(QStringLiteral("%1 has no power; -30 dBm is used").arg(chain.start));
    }

    tracer.walk(start, explicitPath);
    if (chain.stages.empty() && chain.errors.isEmpty())
        chain.errors.append(QStringLiteral("the signal chain has no stages"));

    // Frequency plan.
    double f = chain.inputFrequency;
    for (ChainStage &stage : chain.stages) {
        stage.inputFrequency = f;
        if (stage.translation == ChainStage::Translation::Mixer && std::isfinite(stage.loFrequency)) {
            const QString side = stage.component->parameterText.value(QStringLiteral("side"));
            // The wanted product: explicit, or by wiring direction ("auto", and files
            // made before the parameter existed).
            const QString product = stage.component->parameterText.value(QStringLiteral("product"));
            if (product == QLatin1String("sum")) {
                stage.sumProduct = true;
            } else if (product == QLatin1String("difference")) {
                stage.sumProduct = false;
            } else if (stage.upConversion) {
                stage.sumProduct = side != QLatin1String("high");
            } else {
                stage.sumProduct = false;
            }
            if (!stage.upConversion && !stage.sumProduct) {
                const bool lowSide = stage.loFrequency < f;
                if ((side == QLatin1String("low")) != lowSide)
                    chain.warnings.append(QStringLiteral("%1: the LO at %2 Hz is on the %3 side, but the mixer is set to "
                                                         "%4-side")
                                              .arg(stage.path)
                                              .arg(stage.loFrequency)
                                              .arg(lowSide ? QStringLiteral("low") : QStringLiteral("high"), side));
            }
        }
        f = translate(stage, f);
        if (stage.translation == ChainStage::Translation::Mixer && f <= 0)
            chain.errors.append(QStringLiteral("%1: the output frequency is 0 Hz (RF equals LO)").arg(stage.path));
        stage.outputFrequency = f;
    }
    return chain;
}

// ---------------------------------------------------------------- evaluator

struct ChainEvaluator::Impl {
    ElementContext context;
    TouchstoneCache privateCache;
    struct Cached {
        CMatrix s;
        CMatrix noise;
    };
    QHash<QPair<int, double>, Cached> cache;
};

ChainEvaluator::ChainEvaluator(const SignalChain &chain, const QString &projectDirectory, TouchstoneCache *touchstones)
    : d(std::make_unique<Impl>()), m_chain(chain)
{
    d->context.projectDirectory = projectDirectory;
    d->context.touchstones = touchstones ? touchstones : &d->privateCache;
    d->context.noise = true;
}

ChainEvaluator::~ChainEvaluator() = default;

double ChainEvaluator::outputFrequency(int stage, double inputFrequency) const
{
    return translate(m_chain.stages[static_cast<size_t>(stage)], inputFrequency);
}

namespace {

// Matched two-port with gain and an output noise temperature T0 (F - 1) G.
void conversionTwoPort(double gainDb, double reverseDb, double nfDb, CMatrix &s, CMatrix &noise)
{
    s = CMatrix::Zero(2, 2);
    s(1, 0) = undb20(gainDb);
    s(0, 1) = undb20(reverseDb);
    noise = CMatrix::Zero(2, 2);
    const double f = std::pow(10.0, std::max(0.0, nfDb) / 10.0);
    noise(1, 1) = T0 * (f - 1.0) * std::norm(s(1, 0));
}

} // namespace

std::optional<CMatrix> ChainEvaluator::stageS(int stage, double inputFrequency, QString *error)
{
    const auto key = qMakePair(stage, inputFrequency);
    if (const auto it = d->cache.constFind(key); it != d->cache.cend())
        return it->s;
    const ChainStage &st = m_chain.stages[static_cast<size_t>(stage)];
    const FlatComponent &c = *st.component;
    Impl::Cached cached;
    if (st.translation == ChainStage::Translation::Mixer) {
        conversionTwoPort(parameter(c, "gain", 0), -parameter(c, "rf_if_isolation", 200), parameter(c, "nf", 0), cached.s,
                          cached.noise);
    } else if (st.translation == ChainStage::Translation::Multiplier) {
        const double gain = parameter(c, "gain", 0);
        conversionTwoPort(gain, -200, gain < 0 ? -gain : 0.0, cached.s, cached.noise);
    } else {
        QStringList warnings;
        QString buildError;
        const auto element = buildElement(c, {inputFrequency}, d->context, &warnings, &buildError);
        if (!element) {
            if (error)
                *error = buildError;
            return std::nullopt;
        }
        const int in = st.inputPin, out = st.outputPin;
        const CMatrix &full = element->s.front();
        cached.s.resize(2, 2);
        cached.s << full(in, in), full(in, out), full(out, in), full(out, out);
        cached.noise = CMatrix::Zero(2, 2);
        if (!element->noise.empty()) {
            const CMatrix &n = element->noise.front();
            cached.noise << n(in, in), n(in, out), n(out, in), n(out, out);
        }
    }
    d->cache.insert(key, cached);
    return cached.s;
}

std::optional<CMatrix> ChainEvaluator::stageNoise(int stage, double inputFrequency, QString *error)
{
    if (!stageS(stage, inputFrequency, error))
        return std::nullopt;
    return d->cache.value(qMakePair(stage, inputFrequency)).noise;
}

double ChainEvaluator::forwardGainDb(int from, int to, double inputFrequency, double *finalFrequency, QString *error)
{
    double f = inputFrequency, gain = 0;
    for (int i = from; i < to; ++i) {
        const auto s = stageS(i, f, error);
        if (!s)
            return NotSet;
        gain += db20((*s)(1, 0));
        f = outputFrequency(i, f);
    }
    if (finalFrequency)
        *finalFrequency = f;
    return gain;
}

double ChainEvaluator::reverseGainDb(int from, int to, double frequency, QString *error)
{
    double gain = 0;
    for (int i = to - 1; i >= from; --i) {
        const auto s = stageS(i, frequency, error);
        if (!s)
            return NotSet;
        gain += db20((*s)(0, 1));
    }
    return gain;
}

} // namespace lvd::rf

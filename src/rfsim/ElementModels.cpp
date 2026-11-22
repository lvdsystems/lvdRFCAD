#include "rfsim/ElementModels.h"

#include "rfdata/Discontinuities.h"
#include "rfdata/LineModels.h"
#include "rfdata/NetworkMath.h"
#include "rfdata/NetworkOps.h"
#include "rfdata/Touchstone.h"
#include "rfsim/BlockModels.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>
#include <functional>

namespace lvd::rf {

namespace {

const Complex J(0, 1);

double undb20(double db)
{
    return std::pow(10.0, db / 20.0);
}

// Reads the evaluated parameters of one component and collects problems.
class Parameters
{
public:
    Parameters(const FlatComponent &component, QString *error) : m_component(component), m_error(error) {}

    bool ok() const { return m_ok; }

    double number(const QString &name)
    {
        const auto it = m_component.parameterValues.constFind(name);
        if (it == m_component.parameterValues.cend()) {
            fail(QStringLiteral("%1: parameter %2 has no value").arg(m_component.path, name));
            return 0;
        }
        return *it;
    }

    double positive(const QString &name)
    {
        const double value = number(name);
        if (m_ok && !(value > 0))
            fail(QStringLiteral("%1: %2 must be positive").arg(m_component.path, name));
        return value;
    }

    double nonNegative(const QString &name)
    {
        const double value = number(name);
        if (m_ok && value < 0)
            fail(QStringLiteral("%1: %2 must not be negative").arg(m_component.path, name));
        return value;
    }

    int integer(const QString &name, int minimum)
    {
        const double value = number(name);
        if (m_ok && (value != std::floor(value) || value < minimum))
            fail(QStringLiteral("%1: %2 must be an integer of at least %3").arg(m_component.path, name).arg(minimum));
        return static_cast<int>(value);
    }

    QString text(const QString &name) const { return m_component.parameterText.value(name); }

    void fail(const QString &message)
    {
        if (m_ok && m_error)
            *m_error = message;
        m_ok = false;
    }

private:
    const FlatComponent &m_component;
    QString *m_error;
    bool m_ok = true;
};

// Changes a block defined at reference z0 to the solver reference.
void toSolverReference(CMatrix &s, CMatrix *noise, double z0)
{
    if (std::abs(z0 - SolverZ0) < 1e-12)
        return;
    const std::vector<double> from(static_cast<size_t>(s.rows()), z0);
    const std::vector<double> to(static_cast<size_t>(s.rows()), SolverZ0);
    if (noise && noise->size() > 0) {
        const CMatrix t = noiseRenormalization(s, from, to);
        *noise = t * *noise * t.adjoint();
    }
    s = renormalize(s, from, to);
}

CMatrix seriesImpedance(Complex z)
{
    // Admittance form when |Z| is large, so an open (Z -> inf) stays finite.
    CMatrix s(2, 2);
    const double r = SolverZ0;
    if (std::abs(z) <= 1e6 * r) {
        const Complex den = z + 2.0 * r;
        s << z / den, 2.0 * r / den, 2.0 * r / den, z / den;
    } else {
        const Complex y = 1.0 / z;
        const Complex den = 1.0 + 2.0 * r * y;
        s << 1.0 / den, 2.0 * r * y / den, 2.0 * r * y / den, 1.0 / den;
    }
    return s;
}

CMatrix oneport(Complex gamma)
{
    CMatrix s(1, 1);
    s(0, 0) = gamma;
    return s;
}

CMatrix idealLine(double zc, double thetaRadians)
{
    return abcdToS(lineAbcd(Complex(zc, 0), J * thetaRadians, 1.0), SolverZ0, SolverZ0);
}

// Theta(f) = elen * f / freq, in radians.
double electricalLength(double elenDegrees, double frequency, double referenceFrequency)
{
    return elenDegrees * Pi / 180.0 * frequency / referenceFrequency;
}

QString resolvePath(const QString &file, const QString &projectDirectory)
{
    if (QFileInfo(file).isAbsolute() || projectDirectory.isEmpty())
        return file;
    return QDir(projectDirectory).absoluteFilePath(file);
}

} // namespace

std::optional<NetworkData> TouchstoneCache::load(const QString &path, QString *error, QStringList *warnings)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        if (error)
            *error = QStringLiteral("file %1 not found").arg(QDir::toNativeSeparators(path));
        return std::nullopt;
    }
    const QString key = info.absoluteFilePath();
    const auto it = m_entries.constFind(key);
    if (it != m_entries.cend() && it->modified == info.lastModified() && it->size == info.size()) {
        if (warnings)
            warnings->append(it->warnings);
        return it->network;
    }
    TouchstoneReadResult result = readTouchstone(key);
    if (!result.network) {
        if (error)
            *error = result.error;
        return std::nullopt;
    }
    if (warnings)
        warnings->append(result.warnings);
    m_entries.insert(key, Entry{info.lastModified(), info.size(), *result.network, result.warnings});
    return result.network;
}

CMatrix passiveNoise(const CMatrix &s, double temperature)
{
    const CMatrix identity = CMatrix::Identity(s.rows(), s.cols());
    return temperature * (identity - s * s.adjoint());
}

Complex prototypeResponse(FilterResponse response, int order, double rippleDb, double omega)
{
    if (!std::isfinite(omega))
        return response == FilterResponse::Ideal ? 0.0 : Complex(0, 0);
    switch (response) {
    case FilterResponse::Ideal:
        return std::abs(omega) <= 1.0 ? 1.0 : 0.0;
    case FilterResponse::Butterworth: {
        // Poles on the unit circle in the left half plane; H(0) = 1.
        Complex h = 1.0;
        const Complex s = J * omega;
        for (int k = 1; k <= order; ++k) {
            const Complex pole = std::polar(1.0, Pi * (2.0 * k + order - 1) / (2.0 * order));
            h *= -pole / (s - pole);
        }
        return h;
    }
    case FilterResponse::Chebyshev: {
        const double epsilon = std::sqrt(std::pow(10.0, rippleDb / 10.0) - 1.0);
        const double mu = std::asinh(1.0 / epsilon) / order;
        Complex h = (order % 2 == 0) ? 1.0 / std::sqrt(1.0 + epsilon * epsilon) : 1.0;
        const Complex s = J * omega;
        for (int k = 1; k <= order; ++k) {
            const double angle = (2.0 * k - 1) * Pi / (2.0 * order);
            const Complex pole(-std::sinh(mu) * std::sin(angle), std::cosh(mu) * std::cos(angle));
            h *= -pole / (s - pole);
        }
        return h;
    }
    }
    return 0.0;
}

std::optional<Element> buildElement(const FlatComponent &component, const std::vector<double> &frequencies,
                                    const ElementContext &context, QStringList *warnings, QString *error)
{
    const ComponentDefinition &definition = *component.definition;
    Element element;
    element.path = component.path;
    element.ports = static_cast<int>(definition.pins.size());
    Parameters p(component, error);
    const auto warn = [&](const QString &message) {
        if (warnings)
            warnings->append(QStringLiteral("%1: %2").arg(component.path, message));
    };
    const auto requirePins = [&](int count) {
        if (element.ports != count)
            p.fail(QStringLiteral("%1: the model needs %2 pins, the component has %3")
                       .arg(component.path)
                       .arg(count)
                       .arg(element.ports));
        return p.ok();
    };

    // Fills S (and noise) per frequency from a function of frequency.
    // Passive models get thermal noise at `temperature`; active ones supply it.
    using Model = std::function<CMatrix(double frequency, CMatrix *noise)>;
    const auto sweep = [&](const Model &model) -> std::optional<Element> {
        element.s.reserve(frequencies.size());
        for (double f : frequencies) {
            CMatrix noise;
            CMatrix s = model(f, context.noise ? &noise : nullptr);
            if (!p.ok())
                return std::nullopt;
            element.s.push_back(std::move(s));
            if (context.noise)
                element.noise.push_back(std::move(noise));
        }
        return element;
    };
    const auto passive = [&](const std::function<CMatrix(double)> &model, double temperature = T0) {
        return sweep([&](double f, CMatrix *noise) {
            CMatrix s = model(f);
            if (noise)
                *noise = passiveNoise(s, temperature);
            return s;
        });
    };
    // A block defined at its own z0, passive at T0.
    const auto passiveBlock = [&](double z0, const std::function<CMatrix(double)> &model) {
        return passive([&, z0](double f) {
            CMatrix s = model(f);
            toSolverReference(s, nullptr, z0);
            return s;
        });
    };

    const ModelDefinition *model = nullptr;
    for (const QString &kind : {QStringLiteral("rf-element"), QStringLiteral("rf-behavioural"), QStringLiteral("sparameters")}) {
        if ((model = definition.model(kind)))
            break;
    }
    if (!model) {
        p.fail(QStringLiteral("%1: %2 has no RF model and cannot be used in an RF analysis")
                   .arg(component.path, definition.name));
        return std::nullopt;
    }

    // ---- Circuit elements ----
    if (model->kind == QLatin1String("rf-element")) {
        const QString type = model->data.value(QStringLiteral("element")).toString();
        // Parameters added after the first release (parasitics, roughness):
        // 0 when a stored copy of an older definition lacks them.
        const auto optionalNonNegative = [&](const char *name) {
            const QString key = QString::fromLatin1(name);
            return definition.parameter(key) ? p.nonNegative(key) : 0.0;
        };
        // Series impedance zs shunted by a capacitance cp: zs / (1 + j w cp zs).
        const auto withParallelC = [](Complex zs, double cp, double f) {
            return cp > 0 ? zs / (1.0 + J * (2 * Pi * f * cp) * zs) : zs;
        };
        // Symmetric coupled lines from the two-port S of the even and odd
        // mode lines. Pins p1, p2 (line A ends 1, 2), p3, p4 (line B ends 1, 2).
        const auto coupledFromModes = [](const CMatrix &even, const CMatrix &odd) {
            const int end[4] = {0, 1, 0, 1};
            const int line[4] = {0, 0, 1, 1};
            CMatrix s(4, 4);
            for (int i = 0; i < 4; ++i) {
                for (int k = 0; k < 4; ++k) {
                    const Complex e = even(end[i], end[k]), o = odd(end[i], end[k]);
                    s(i, k) = line[i] == line[k] ? (e + o) / 2.0 : (e - o) / 2.0;
                }
            }
            return s;
        };
        // Resistor: (r + j w esl) || cp.
        if (type == QLatin1String("resistor") && requirePins(2)) {
            const double r = p.nonNegative(QStringLiteral("r"));
            const double esl = optionalNonNegative("esl");
            const double cp = optionalNonNegative("cp");
            return passive([&](double f) { return seriesImpedance(withParallelC(Complex(r, 2 * Pi * f * esl), cp, f)); });
        }
        // Capacitor: rs(Q) + esr + j w esl + 1/(j w c), rs = 1/(w_q c q) from the Q at freq.
        if (type == QLatin1String("capacitor") && requirePins(2)) {
            const double c = p.positive(QStringLiteral("c"));
            const double q = p.nonNegative(QStringLiteral("q"));
            const double fq = p.positive(QStringLiteral("freq"));
            const double esr = optionalNonNegative("esr");
            const double esl = optionalNonNegative("esl");
            const double rs = q > 0 ? 1.0 / (2 * Pi * fq * c * q) : 0.0;
            return passive([&](double f) {
                const Complex z = f > 0 ? Complex(rs + esr, 2 * Pi * f * esl - 1.0 / (2 * Pi * f * c))
                                        : Complex(std::numeric_limits<double>::infinity(), 0);
                return seriesImpedance(z);
            });
        }
        // Inductor: (rs(Q) + j w l) || cp, rs = w_q l / q; cp sets the self-resonance.
        if (type == QLatin1String("inductor") && requirePins(2)) {
            const double l = p.nonNegative(QStringLiteral("l"));
            const double q = p.nonNegative(QStringLiteral("q"));
            const double fq = p.positive(QStringLiteral("freq"));
            const double cp = optionalNonNegative("cp");
            const double rs = q > 0 ? 2 * Pi * fq * l / q : 0.0;
            return passive([&](double f) { return seriesImpedance(withParallelC(Complex(rs, 2 * Pi * f * l), cp, f)); });
        }
        if (type == QLatin1String("substrate")) {
            p.fail(QStringLiteral("%1: a Substrate part holds substrate data and has no circuit model").arg(component.path));
            return std::nullopt;
        }
        if (type == QLatin1String("tline") && requirePins(2)) {
            const double z0 = p.positive(QStringLiteral("z0"));
            const double elen = p.number(QStringLiteral("elen"));
            const double fq = p.positive(QStringLiteral("freq"));
            return passive([&](double f) { return idealLine(z0, electricalLength(elen, f, fq)); });
        }
        if ((type == QLatin1String("stub_open") || type == QLatin1String("stub_short")) && requirePins(1)) {
            const double z0 = p.positive(QStringLiteral("z0"));
            const double elen = p.number(QStringLiteral("elen"));
            const double fq = p.positive(QStringLiteral("freq"));
            const bool open = type == QLatin1String("stub_open");
            return passive([&](double f) {
                const double tanTheta = std::tan(electricalLength(elen, f, fq));
                Complex gamma;
                if (open) {
                    const Complex y = J * tanTheta / z0; // Zin = -j z0 cot(theta)
                    gamma = (1.0 - SolverZ0 * y) / (1.0 + SolverZ0 * y);
                } else {
                    const Complex z = J * z0 * tanTheta;
                    gamma = reflectionFromImpedance(z, SolverZ0);
                }
                return oneport(gamma);
            });
        }
        if (type == QLatin1String("coupled_lines") && requirePins(4)) {
            const double z0e = p.positive(QStringLiteral("z0e"));
            const double z0o = p.positive(QStringLiteral("z0o"));
            const double elen = p.number(QStringLiteral("elen"));
            const double fq = p.positive(QStringLiteral("freq"));
            return passive([&](double f) {
                const double theta = electricalLength(elen, f, fq);
                return coupledFromModes(idealLine(z0e, theta), idealLine(z0o, theta));
            });
        }
        if (type == QLatin1String("transformer") && requirePins(2)) {
            const double n = p.positive(QStringLiteral("n"));
            return passive([&](double) {
                CMatrix abcd(2, 2);
                abcd << 1.0 / n, 0.0, 0.0, n;
                return abcdToS(abcd, SolverZ0, SolverZ0);
            });
        }

        // Physical lines
        const auto physicalLine = [&](const std::function<std::optional<LineProperties>(double, QString *)> &properties,
                                      double length) {
            return passive([&, length](double f) {
                QString lineError;
                const auto line = properties(f, &lineError);
                if (!line) {
                    p.fail(QStringLiteral("%1: %2").arg(component.path, lineError));
                    return CMatrix(CMatrix::Zero(2, 2));
                }
                return abcdToS(lineAbcd(line->zc, line->gamma, length), SolverZ0, SolverZ0);
            });
        };
        if (type == QLatin1String("microstrip") && requirePins(2)) {
            MicrostripGeometry g{p.positive(QStringLiteral("w")), p.positive(QStringLiteral("h")),
                                 p.nonNegative(QStringLiteral("t")), p.positive(QStringLiteral("er")),
                                 p.nonNegative(QStringLiteral("tand")), p.nonNegative(QStringLiteral("sigma")),
                                 optionalNonNegative("rough")};
            const double l = p.nonNegative(QStringLiteral("l"));
            if (!p.ok())
                return std::nullopt;
            return physicalLine([g](double f, QString *e) { return microstrip(g, f, e); }, l);
        }
        if (type == QLatin1String("stripline") && requirePins(2)) {
            StriplineGeometry g{p.positive(QStringLiteral("w")), p.positive(QStringLiteral("b")),
                                p.nonNegative(QStringLiteral("t")), p.positive(QStringLiteral("er")),
                                p.nonNegative(QStringLiteral("tand")), p.nonNegative(QStringLiteral("sigma")),
                                optionalNonNegative("rough")};
            const double l = p.nonNegative(QStringLiteral("l"));
            if (!p.ok())
                return std::nullopt;
            return physicalLine([g](double f, QString *e) { return stripline(g, f, e); }, l);
        }
        if (type == QLatin1String("coax") && requirePins(2)) {
            CoaxGeometry g{p.positive(QStringLiteral("din")), p.positive(QStringLiteral("dout")),
                           p.positive(QStringLiteral("er")), p.nonNegative(QStringLiteral("tand")),
                           p.nonNegative(QStringLiteral("sigma"))};
            const double l = p.nonNegative(QStringLiteral("l"));
            if (!p.ok())
                return std::nullopt;
            return physicalLine([g](double f, QString *e) { return coax(g, f, e); }, l);
        }
        if (type == QLatin1String("cpw") && requirePins(2)) {
            CpwGeometry g{p.positive(QStringLiteral("w")), p.positive(QStringLiteral("s")),
                          p.positive(QStringLiteral("h")), p.nonNegative(QStringLiteral("t")),
                          p.positive(QStringLiteral("er")), p.nonNegative(QStringLiteral("tand")),
                          p.nonNegative(QStringLiteral("sigma")), p.text(QStringLiteral("backed")) == QLatin1String("yes"),
                          optionalNonNegative("rough")};
            const double l = p.nonNegative(QStringLiteral("l"));
            if (!p.ok())
                return std::nullopt;
            return physicalLine([g](double f, QString *e) { return cpw(g, f, e); }, l);
        }
        if (type == QLatin1String("coupled_microstrip") && requirePins(4)) {
            CoupledMicrostripGeometry g{p.positive(QStringLiteral("w")),     p.positive(QStringLiteral("s")),
                                        p.positive(QStringLiteral("h")),     p.nonNegative(QStringLiteral("t")),
                                        p.positive(QStringLiteral("er")),    p.nonNegative(QStringLiteral("tand")),
                                        p.nonNegative(QStringLiteral("sigma")), optionalNonNegative("rough")};
            const double l = p.nonNegative(QStringLiteral("l"));
            if (!p.ok())
                return std::nullopt;
            return passive([&, g, l](double f) {
                QString lineError;
                const auto modes = coupledMicrostrip(g, f, &lineError);
                if (!modes) {
                    p.fail(QStringLiteral("%1: %2").arg(component.path, lineError));
                    return CMatrix(CMatrix::Zero(4, 4));
                }
                const auto mode = [l](const LineProperties &line) {
                    return abcdToS(lineAbcd(line.zc, line.gamma, l), SolverZ0, SolverZ0);
                };
                return coupledFromModes(mode(modes->even), mode(modes->odd));
            });
        }

        // Microstrip discontinuities: substrate h, er, t (and tand, sigma,
        // rough for the tee's arm lines).
        const auto substrate = [&]() {
            MicrostripSubstrate s;
            s.h = p.positive(QStringLiteral("h"));
            s.er = p.positive(QStringLiteral("er"));
            s.t = p.nonNegative(QStringLiteral("t"));
            if (definition.parameter(QStringLiteral("tand")))
                s.tand = p.nonNegative(QStringLiteral("tand"));
            if (definition.parameter(QStringLiteral("sigma")))
                s.sigma = p.nonNegative(QStringLiteral("sigma"));
            s.rough = optionalNonNegative("rough");
            return s;
        };
        // Reports a model error once and returns the placeholder matrix.
        const auto failed = [&](const QString &message, int ports) {
            p.fail(QStringLiteral("%1: %2").arg(component.path, message));
            return CMatrix(CMatrix::Zero(ports, ports));
        };
        // Series L1, shunt C, series L2 as a two-port.
        const auto tee2 = [](double l1, double c, double l2, double f) {
            const double w = 2 * Pi * f;
            CMatrix a(2, 2), b(2, 2), d(2, 2);
            a << 1.0, J * w * l1, 0.0, 1.0;
            b << 1.0, 0.0, J * w * c, 1.0;
            d << 1.0, J * w * l2, 0.0, 1.0;
            return abcdToS(a * b * d, SolverZ0, SolverZ0);
        };
        if (type == QLatin1String("microstrip_open") && requirePins(1)) {
            const MicrostripSubstrate s = substrate();
            const double w = p.positive(QStringLiteral("w"));
            if (!p.ok())
                return std::nullopt;
            return passive([&, s, w](double f) {
                QString modelError;
                const auto c = openEndCapacitance(s, w, f, &modelError);
                if (!c)
                    return failed(modelError, 1);
                const Complex y = J * (2 * Pi * f * *c);
                return oneport((1.0 - SolverZ0 * y) / (1.0 + SolverZ0 * y));
            });
        }
        if (type == QLatin1String("microstrip_gap") && requirePins(2)) {
            const MicrostripSubstrate s = substrate();
            const double w1 = p.positive(QStringLiteral("w1")), w2 = p.positive(QStringLiteral("w2"));
            const double gap = p.positive(QStringLiteral("s"));
            if (!p.ok())
                return std::nullopt;
            return passive([&, s, w1, w2, gap](double f) {
                QString modelError;
                const auto g = microstripGap(s, w1, w2, gap, f, &modelError);
                if (!g)
                    return failed(modelError, 2);
                const Complex jw = J * (2 * Pi * f);
                CMatrix y(2, 2);
                y << jw * (g->c1 + g->cs), -jw * g->cs, -jw * g->cs, jw * (g->c2 + g->cs);
                return yToS(y, {SolverZ0, SolverZ0});
            });
        }
        if (type == QLatin1String("microstrip_step") && requirePins(2)) {
            const MicrostripSubstrate s = substrate();
            const double w1 = p.positive(QStringLiteral("w1")), w2 = p.positive(QStringLiteral("w2"));
            if (!p.ok())
                return std::nullopt;
            return passive([&, s, w1, w2](double f) {
                QString modelError;
                const auto step = microstripStep(s, w1, w2, f, &modelError);
                if (!step)
                    return failed(modelError, 2);
                return tee2(step->l1, step->c, step->l2, f);
            });
        }
        if (type == QLatin1String("microstrip_bend") && requirePins(2)) {
            const MicrostripSubstrate s = substrate();
            const double w = p.positive(QStringLiteral("w"));
            const bool mitred = p.text(QStringLiteral("mitred")) == QLatin1String("yes");
            if (!p.ok())
                return std::nullopt;
            QString modelError;
            const auto bend = microstripBend(s, w, mitred, &modelError);
            if (!bend) {
                p.fail(QStringLiteral("%1: %2").arg(component.path, modelError));
                return std::nullopt;
            }
            const BendCircuit b = *bend;
            return passive([&, b](double f) { return tee2(b.l, b.c, b.l, f); });
        }
        if (type == QLatin1String("microstrip_tee") && requirePins(3)) {
            const MicrostripSubstrate s = substrate();
            const double w1 = p.positive(QStringLiteral("w1")), w2 = p.positive(QStringLiteral("w2"));
            const double w3 = p.positive(QStringLiteral("w3"));
            if (!p.ok())
                return std::nullopt;
            return passive([&, s, w1, w2, w3](double f) {
                if (!(f > 0)) { // DC: an ideal three-way node
                    CMatrix node = CMatrix::Constant(3, 3, Complex(2.0 / 3.0, 0));
                    node.diagonal().setConstant(Complex(-1.0 / 3.0, 0));
                    return node;
                }
                QString modelError;
                const auto tee = microstripTee(s, w1, w2, w3, f, SolverZ0, &modelError);
                if (!tee)
                    return failed(modelError, 3);
                return *tee;
            });
        }
        if (type == QLatin1String("microstrip_via") && requirePins(1)) {
            const double d = p.positive(QStringLiteral("d")), h = p.positive(QStringLiteral("h"));
            const double tp = p.nonNegative(QStringLiteral("tp")), sigma = p.nonNegative(QStringLiteral("sigma"));
            if (!p.ok())
                return std::nullopt;
            return passive([&, d, h, tp, sigma](double f) {
                QString modelError;
                const auto z = viaImpedance(d, h, tp, sigma, f, &modelError);
                if (!z)
                    return failed(modelError, 1);
                return oneport(reflectionFromImpedance(*z, SolverZ0));
            });
        }
        if (p.ok())
            p.fail(QStringLiteral("%1: unknown RF element \"%2\"").arg(component.path, type));
        return std::nullopt;
    }

    // ---- S-parameter files ----
    if (model->kind == QLatin1String("sparameters")) {
        QString file = model->data.value(QStringLiteral("file")).toString();
        if (file.startsWith(QLatin1Char('{')) && file.endsWith(QLatin1Char('}')))
            file = p.text(file.mid(1, file.size() - 2));
        if (file.trimmed().isEmpty()) {
            p.fail(QStringLiteral("%1: no S-parameter file is set").arg(component.path));
            return std::nullopt;
        }
        if (!context.touchstones) {
            p.fail(QStringLiteral("%1: S-parameter files are not available here").arg(component.path));
            return std::nullopt;
        }
        QString fileError;
        QStringList fileWarnings;
        const auto data = context.touchstones->load(resolvePath(file, context.projectDirectory), &fileError, &fileWarnings);
        for (const QString &message : fileWarnings)
            warn(message);
        if (!data) {
            p.fail(QStringLiteral("%1: %2").arg(component.path, fileError));
            return std::nullopt;
        }
        if (!requirePins(data->portCount()))
            return std::nullopt;
        QString interpolationError, interpolationWarning;
        const auto resampled = interpolate(*data, frequencies, Extrapolation::Hold, &interpolationError, &interpolationWarning);
        if (!resampled) {
            p.fail(QStringLiteral("%1: %2").arg(component.path, interpolationError));
            return std::nullopt;
        }
        if (!interpolationWarning.isEmpty())
            warn(interpolationWarning);
        const std::vector<double> solverZ0(static_cast<size_t>(element.ports), SolverZ0);
        bool warnedNoise = false;
        for (size_t i = 0; i < frequencies.size(); ++i) {
            const CMatrix &s = resampled->s[i];
            CMatrix noise;
            if (context.noise) {
                if (resampled->hasNoiseParameters()) {
                    // Interpolation put the noise parameters on the same grid.
                    noise = correlationFromNoiseParameters(s, resampled->noise[i], resampled->z0[0]);
                } else if (isPassive(s, 1e-6)) {
                    noise = passiveNoise(s, T0);
                } else {
                    noise = CMatrix::Zero(s.rows(), s.cols());
                    if (!warnedNoise)
                        warn(QStringLiteral("active S-parameter data without noise data is treated as noiseless"));
                    warnedNoise = true;
                }
                const CMatrix t = noiseRenormalization(s, resampled->z0, solverZ0);
                noise = t * noise * t.adjoint();
                element.noise.push_back(noise);
            }
            element.s.push_back(renormalize(s, resampled->z0, solverZ0));
        }
        return element;
    }

    // ---- Behavioural blocks ----
    const QString block = model->data.value(QStringLiteral("block")).toString();

    if (block == QLatin1String("port") && requirePins(1)) {
        element.isPort = true;
        element.portNumber = p.integer(QStringLiteral("num"), 1);
        element.portZ0 = p.positive(QStringLiteral("z0"));
        if (!p.ok())
            return std::nullopt;
        return element;
    }

    const double z0 = definition.parameter(QStringLiteral("z0")) ? p.positive(QStringLiteral("z0")) : SolverZ0;
    if (!p.ok())
        return std::nullopt;

    // Parameters added after a block's first release (return losses, slopes,
    // their reference frequency): a component defined without them, such as
    // an older copy of the library in a project, gets the neutral default.
    const auto optional = [&](const char *name, double fallback) {
        const QString key = QString::fromLatin1(name);
        return definition.parameter(key) ? p.number(key) : fallback;
    };
    // |Gamma| from a return loss parameter; 0 dB (the default) is an ideal match.
    const auto gammaOf = [&](const char *name) {
        const double rl = optional(name, 0);
        if (p.ok() && rl < 0)
            p.fail(QStringLiteral("%1: %2 must not be negative").arg(component.path, QString::fromLatin1(name)));
        return returnLossToGamma(rl);
    };
    const double slopeReference = optional("freq", 1e9);
    const double ilSlope = optional("il_slope", 0);
    // Insertion loss at f with the block's slope (dB/GHz around `freq`), never below 0.
    const auto lossAt = [=](double il, double f) { return sloped(il, ilSlope, f, slopeReference, true); };

    // One-port loads: terminations, the ends of a system chain and the sources.
    if ((block == QLatin1String("termination") || block == QLatin1String("source") || block == QLatin1String("lo")
         || block == QLatin1String("vco") || block == QLatin1String("pll") || block == QLatin1String("dac")
         || block == QLatin1String("adc") || block == QLatin1String("detector") || block == QLatin1String("antenna"))
        && requirePins(1)) {
        const double temperature =
            block == QLatin1String("antenna") ? p.nonNegative(QStringLiteral("temp")) : T0;
        // The newer sources and converters say what they are here, like the multiplier.
        if (block == QLatin1String("vco") || block == QLatin1String("pll") || block == QLatin1String("dac")
            || block == QLatin1String("detector"))
            warn(QStringLiteral("%1 is modelled as a matched %2 ohm load in the linear analysis (its signal, noise "
                                "and phase noise are budget quantities)")
                     .arg(definition.name)
                     .arg(z0));
        const Complex gamma = reflectionFromImpedance(z0, SolverZ0);
        return passive([gamma](double) { return oneport(gamma); }, temperature);
    }

    if (block == QLatin1String("amplifier") && requirePins(2)) {
        const double gain = p.number(QStringLiteral("gain"));
        const double nf = p.nonNegative(QStringLiteral("nf"));
        const double isolation = p.number(QStringLiteral("isolation"));
        const double gainSlope = optional("gain_slope", 0);
        const double nfSlope = optional("nf_slope", 0);
        const std::vector<double> gamma{gammaOf("rl_in"), gammaOf("rl_out")};
        if (!p.ok())
            return std::nullopt;
        return sweep([&](double frequency, CMatrix *noise) {
            CMatrix s(2, 2);
            s << 0.0, undb20(-isolation), undb20(sloped(gain, gainSlope, frequency, slopeReference, false)), 0.0;
            s = withPortReflections(s, gamma, 1.0, false);
            if (noise) {
                // All added noise appears at the output: Te = T0 (F - 1), amplified.
                const double f = std::pow(10.0, sloped(nf, nfSlope, frequency, slopeReference, true) / 10.0);
                *noise = CMatrix::Zero(2, 2);
                (*noise)(1, 1) = T0 * (f - 1.0) * std::norm(s(1, 0));
            }
            toSolverReference(s, noise, z0);
            return s;
        });
    }

    // A two-port with forward and reverse transmission at f, matched or with
    // the block's rl_in / rl_out (reflections in quadrature with S21).
    const auto twoPort = [&](const std::function<std::pair<Complex, Complex>(double)> &transmission) {
        const std::vector<double> gamma{gammaOf("rl_in"), gammaOf("rl_out")};
        return passiveBlock(z0, [=](double f) {
            const auto [forward, reverse] = transmission(f);
            CMatrix s(2, 2);
            s << 0.0, reverse, forward, 0.0;
            return withPortReflections(s, gamma, forward);
        });
    };
    const auto twoPortThrough = [&](Complex forward, Complex reverse) {
        return twoPort([=](double) { return std::make_pair(forward, reverse); });
    };

    if ((block == QLatin1String("attenuator") || block == QLatin1String("limiter")) && requirePins(2)) {
        const double il = p.number(QStringLiteral("il"));
        if (!p.ok())
            return std::nullopt;
        return twoPort([=](double f) {
            const Complex t = undb20(-lossAt(il, f));
            return std::make_pair(t, t);
        });
    }
    if (block == QLatin1String("isolator") && requirePins(2)) {
        const double il = p.number(QStringLiteral("il"));
        const Complex reverse = undb20(-p.number(QStringLiteral("isolation")));
        if (!p.ok())
            return std::nullopt;
        return twoPort([=](double f) { return std::make_pair(Complex(undb20(-lossAt(il, f))), reverse); });
    }
    if (block == QLatin1String("phase_shifter") && requirePins(2)) {
        const double il = p.number(QStringLiteral("il"));
        const double phase = p.number(QStringLiteral("phase"));
        if (!p.ok())
            return std::nullopt;
        return twoPort([=](double f) {
            const Complex t = fromDbPhase(-lossAt(il, f), -phase);
            return std::make_pair(t, t);
        });
    }
    if (block == QLatin1String("cable") && requirePins(2)) {
        const double length = p.nonNegative(QStringLiteral("length"));
        const double lossPerM = p.nonNegative(QStringLiteral("loss_per_m"));
        const double fq = p.positive(QStringLiteral("freq"));
        const double il = p.nonNegative(QStringLiteral("il"));
        const double vf = p.positive(QStringLiteral("vf"));
        if (!p.ok())
            return std::nullopt;
        return twoPort([=](double f) {
            const double loss = lossAt(il, f) + lossPerM * length * std::sqrt(f / fq);
            const double phase = 360.0 * f * length / (vf * SpeedOfLight);
            const Complex t = fromDbPhase(-loss, -phase);
            return std::make_pair(t, t);
        });
    }
    if (block == QLatin1String("dc_block") && requirePins(2)) {
        const double il = p.nonNegative(QStringLiteral("il"));
        const double fc = p.positive(QStringLiteral("fc"));
        if (!p.ok())
            return std::nullopt;
        return passiveBlock(z0, [=](double f) { return dcBlock(f, fc, il); });
    }
    if (block == QLatin1String("filter") && requirePins(2)) {
        const QString type = p.text(QStringLiteral("type"));
        const QString responseName = p.text(QStringLiteral("response"));
        const FilterResponse response = responseName == QLatin1String("ideal")       ? FilterResponse::Ideal
                                        : responseName == QLatin1String("chebyshev") ? FilterResponse::Chebyshev
                                                                                     : FilterResponse::Butterworth;
        const int order = response == FilterResponse::Ideal ? 1 : p.integer(QStringLiteral("order"), 1);
        const double fc = p.positive(QStringLiteral("fc"));
        const double bw = (type == QLatin1String("bandpass") || type == QLatin1String("bandstop"))
                              ? p.positive(QStringLiteral("bw"))
                              : 0.0;
        const double il = p.number(QStringLiteral("il"));
        const double ripple = response == FilterResponse::Chebyshev ? p.positive(QStringLiteral("ripple")) : 0.0;
        const double rejection = p.positive(QStringLiteral("rejection"));
        if (bw > 0 && fc - bw / 2 <= 0)
            p.fail(QStringLiteral("%1: the band edge fc - bw/2 must be above 0 Hz").arg(component.path));
        if (!p.ok())
            return std::nullopt;
        const double f1 = fc - bw / 2, f2 = fc + bw / 2;
        const double f0 = std::sqrt(f1 * f2), band = f2 - f1;
        const double floor = undb20(-rejection);
        const double inf = std::numeric_limits<double>::infinity();
        return passiveBlock(z0, [=](double f) {
            double omega = 0;
            if (type == QLatin1String("lowpass"))
                omega = f / fc;
            else if (type == QLatin1String("highpass"))
                omega = f > 0 ? -fc / f : -inf;
            else if (type == QLatin1String("bandpass"))
                omega = f > 0 ? (f * f - f0 * f0) / (band * f) : -inf;
            else // bandstop
                omega = std::abs(f0 * f0 - f * f) > 0 ? band * f / (f0 * f0 - f * f) : inf;
            Complex t = prototypeResponse(response, order, ripple, omega);
            // Attenuation limited to `rejection`, keeping the phase.
            if (std::abs(t) < floor)
                t = std::abs(t) > 0 ? t / std::abs(t) * floor : Complex(floor, 0);
            // A lossless two-port with this transmission; S11 = S22 in quadrature with S21.
            const double reflectionMagnitude = std::sqrt(std::max(0.0, 1.0 - std::norm(t)));
            const Complex direction = std::abs(t) > 0 ? t / std::abs(t) : Complex(1, 0);
            const Complex reflection = J * reflectionMagnitude * direction;
            const Complex transmission = t * undb20(-il);
            CMatrix s(2, 2);
            s << reflection, transmission, transmission, reflection;
            return s;
        });
    }
    if ((block == QLatin1String("splitter") || block == QLatin1String("combiner")) && requirePins(3)) {
        const double il = p.nonNegative(QStringLiteral("il"));
        const double isolation = p.number(QStringLiteral("isolation"));
        const double imbalance = p.number(QStringLiteral("imbalance"));
        const double phaseImbalance = p.number(QStringLiteral("phase_imbalance"));
        const bool splitter = block == QLatin1String("splitter");
        const int common = splitter ? 0 : 2, a = splitter ? 1 : 0, b = splitter ? 2 : 1;
        // rl_in at the input side (splitter: in; combiner: in1, in2), rl_out at the output side.
        const double gammaIn = gammaOf("rl_in"), gammaOut = gammaOf("rl_out");
        std::vector<double> gamma(3, splitter ? gammaOut : gammaIn);
        gamma[static_cast<size_t>(common)] = splitter ? gammaIn : gammaOut;
        if (!p.ok())
            return std::nullopt;
        return passiveBlock(z0, [=](double f) {
            const double split = 10.0 * std::log10(2.0);
            const double loss = lossAt(il, f);
            const Complex ta = fromDbPhase(-(split + loss + imbalance / 2), -phaseImbalance / 2);
            const Complex tb = fromDbPhase(-(split + loss - imbalance / 2), phaseImbalance / 2);
            const Complex iso = undb20(-isolation);
            CMatrix s = CMatrix::Zero(3, 3);
            s(common, a) = s(a, common) = ta;
            s(common, b) = s(b, common) = tb;
            s(a, b) = s(b, a) = iso;
            return withPortReflections(s, gamma);
        });
    }
    if (block == QLatin1String("coupler") && requirePins(4)) {
        const double coupling = p.number(QStringLiteral("coupling"));
        const double directivity = p.number(QStringLiteral("directivity"));
        const double il = p.nonNegative(QStringLiteral("il"));
        const std::vector<double> gamma(4, gammaOf("rl"));
        if (!p.ok())
            return std::nullopt;
        return passiveBlock(z0, [=](double f) {
            const Complex t = undb20(-lossAt(il, f));
            const Complex c = J * undb20(-coupling); // coupled paths +90 degrees
            const Complex i = undb20(-(coupling + directivity));
            CMatrix s = CMatrix::Zero(4, 4);
            // in 0, out 1, cpl 2, iso 3
            s(0, 1) = s(1, 0) = t;
            s(2, 3) = s(3, 2) = t;
            s(0, 2) = s(2, 0) = c;
            s(1, 3) = s(3, 1) = c;
            s(0, 3) = s(3, 0) = i;
            s(1, 2) = s(2, 1) = i;
            return withPortReflections(s, gamma);
        });
    }
    if ((block == QLatin1String("hybrid_90") || block == QLatin1String("hybrid_180")) && requirePins(4)) {
        const double il = p.nonNegative(QStringLiteral("il"));
        const double isolation = p.number(QStringLiteral("isolation"));
        const double imbalance = p.number(QStringLiteral("imbalance"));
        const double phaseImbalance = p.number(QStringLiteral("phase_imbalance"));
        const std::vector<double> gamma(4, gammaOf("rl"));
        const bool quadrature = block == QLatin1String("hybrid_90");
        if (!p.ok())
            return std::nullopt;
        return passiveBlock(z0, [=](double f) {
            const double loss = lossAt(il, f);
            const CMatrix s = quadrature ? hybrid90(loss, isolation, imbalance, phaseImbalance)
                                         : hybrid180(loss, isolation, imbalance, phaseImbalance);
            return withPortReflections(s, gamma);
        });
    }
    if (block == QLatin1String("balun") && requirePins(3)) {
        const double il = p.nonNegative(QStringLiteral("il"));
        const double isolation = p.number(QStringLiteral("isolation"));
        const double imbalance = p.number(QStringLiteral("imbalance"));
        const double phaseImbalance = p.number(QStringLiteral("phase_imbalance"));
        const double gammaIn = gammaOf("rl_in"), gammaOut = gammaOf("rl_out");
        if (!p.ok())
            return std::nullopt;
        return passiveBlock(z0, [=](double f) {
            return withPortReflections(balun(lossAt(il, f), isolation, imbalance, phaseImbalance), {gammaIn, gammaOut, gammaOut});
        });
    }
    if (block == QLatin1String("bias_tee") && requirePins(3)) {
        const double il = p.nonNegative(QStringLiteral("il"));
        const double isolation = p.number(QStringLiteral("isolation"));
        const double gammaIn = gammaOf("rl_in"), gammaOut = gammaOf("rl_out");
        if (!p.ok())
            return std::nullopt;
        return passiveBlock(z0, [=](double f) {
            // rf_in 0, rf_out 1, dc 2: the dc pin is matched at RF and isolated from both RF pins.
            const Complex t = undb20(-lossAt(il, f)), i = undb20(-isolation);
            CMatrix s = CMatrix::Zero(3, 3);
            s(0, 1) = s(1, 0) = t;
            s(0, 2) = s(2, 0) = s(1, 2) = s(2, 1) = i;
            return limitToPassive(withPortReflections(s, {gammaIn, gammaOut, 0.0}));
        });
    }
    if (block == QLatin1String("diplexer") && requirePins(3)) {
        const double crossover = p.positive(QStringLiteral("crossover"));
        const int order = p.integer(QStringLiteral("order"), 1);
        const double il = p.nonNegative(QStringLiteral("il"));
        if (!p.ok())
            return std::nullopt;
        return passiveBlock(z0, [=](double f) { return diplexer(f, crossover, order, lossAt(il, f)); });
    }
    if ((block == QLatin1String("switch_spdt") || block == QLatin1String("switch_sp3t") || block == QLatin1String("switch_sp4t"))
        && requirePins(block == QLatin1String("switch_spdt") ? 3 : block == QLatin1String("switch_sp3t") ? 4 : 5)) {
        const int throws = element.ports - 1;
        const QString state = p.text(QStringLiteral("state"));
        int selected = 1;
        for (int k = 1; k <= throws; ++k) {
            if (state == QStringLiteral("t%1").arg(k))
                selected = k;
        }
        // `select` (an expression, 0 by default) overrides the state with a throw number.
        if (const int number = static_cast<int>(std::lround(component.parameterValues.value(QStringLiteral("select"), 0.0)));
            number != 0) {
            if (number < 1 || number > throws) {
                if (error)
                    *error = QStringLiteral("select = %1 is not a throw of this switch (1 to %2)").arg(number).arg(throws);
                return std::nullopt;
            }
            selected = number;
        }
        const double il = p.nonNegative(QStringLiteral("il"));
        const double isolation = p.number(QStringLiteral("isolation"));
        std::vector<double> gamma(static_cast<size_t>(element.ports), gammaOf("rl_out"));
        gamma[0] = gammaOf("rl_in");
        if (!p.ok())
            return std::nullopt;
        return passiveBlock(z0, [=](double f) {
            return withPortReflections(multiThrowSwitch(throws, selected, lossAt(il, f), isolation), gamma);
        });
    }
    if (block == QLatin1String("circulator") && requirePins(3)) {
        const double il = p.nonNegative(QStringLiteral("il"));
        const double isolation = p.number(QStringLiteral("isolation"));
        return passiveBlock(z0, [=](double) {
            const Complex forward = undb20(-il), reverse = undb20(-isolation);
            CMatrix s = CMatrix::Zero(3, 3);
            s(1, 0) = s(2, 1) = s(0, 2) = forward;
            s(0, 1) = s(1, 2) = s(2, 0) = reverse;
            return s;
        });
    }
    if (block == QLatin1String("mixer") && requirePins(3)) {
        warn(QStringLiteral("the frequency conversion of a mixer is not part of the linear analysis; only its port "
                            "isolations are modelled"));
        const Complex loRf = undb20(-p.number(QStringLiteral("lo_rf_isolation")));
        const Complex loIf = undb20(-p.number(QStringLiteral("lo_if_isolation")));
        const Complex rfIf = undb20(-p.number(QStringLiteral("rf_if_isolation")));
        const std::vector<double> gamma{gammaOf("rl_rf"), gammaOf("rl_lo"), gammaOf("rl_if")};
        if (!p.ok())
            return std::nullopt;
        return passiveBlock(z0, [=](double) {
            CMatrix s = CMatrix::Zero(3, 3); // rf 0, lo 1, if 2
            s(0, 1) = s(1, 0) = loRf;
            s(1, 2) = s(2, 1) = loIf;
            s(0, 2) = s(2, 0) = rfIf;
            return withPortReflections(s, gamma);
        });
    }
    if (block == QLatin1String("multiplier") && requirePins(2)) {
        warn(QStringLiteral("a frequency multiplier is not part of the linear analysis; it is modelled as two "
                            "matched loads"));
        return twoPortThrough(0.0, 0.0);
    }

    if (p.ok())
        p.fail(QStringLiteral("%1: block \"%2\" is not supported by the linear analysis").arg(component.path, block));
    return std::nullopt;
}

} // namespace lvd::rf

#include "rfsim/ElementModels.h"

#include "rfdata/LineModels.h"
#include "rfdata/NetworkMath.h"
#include "rfdata/NetworkOps.h"
#include "rfdata/Touchstone.h"

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
        if (type == QLatin1String("resistor") && requirePins(2)) {
            const double r = p.nonNegative(QStringLiteral("r"));
            return passive([&](double) { return seriesImpedance(r); });
        }
        if (type == QLatin1String("capacitor") && requirePins(2)) {
            const double c = p.positive(QStringLiteral("c"));
            const double q = p.nonNegative(QStringLiteral("q"));
            const double fq = p.positive(QStringLiteral("freq"));
            const double rs = q > 0 ? 1.0 / (2 * Pi * fq * c * q) : 0.0;
            return passive([&](double f) {
                const Complex z = f > 0 ? Complex(rs, -1.0 / (2 * Pi * f * c))
                                        : Complex(std::numeric_limits<double>::infinity(), 0);
                return seriesImpedance(z);
            });
        }
        if (type == QLatin1String("inductor") && requirePins(2)) {
            const double l = p.nonNegative(QStringLiteral("l"));
            const double q = p.nonNegative(QStringLiteral("q"));
            const double fq = p.positive(QStringLiteral("freq"));
            const double rs = q > 0 ? 2 * Pi * fq * l / q : 0.0;
            return passive([&](double f) { return seriesImpedance(Complex(rs, 2 * Pi * f * l)); });
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
                const CMatrix even = idealLine(z0e, theta);
                const CMatrix odd = idealLine(z0o, theta);
                // Pins p1, p2 (line A ends 1, 2), p3, p4 (line B ends 1, 2).
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
                                 p.nonNegative(QStringLiteral("tand")), p.nonNegative(QStringLiteral("sigma"))};
            const double l = p.nonNegative(QStringLiteral("l"));
            if (!p.ok())
                return std::nullopt;
            return physicalLine([g](double f, QString *e) { return microstrip(g, f, e); }, l);
        }
        if (type == QLatin1String("stripline") && requirePins(2)) {
            StriplineGeometry g{p.positive(QStringLiteral("w")), p.positive(QStringLiteral("b")),
                                p.nonNegative(QStringLiteral("t")), p.positive(QStringLiteral("er")),
                                p.nonNegative(QStringLiteral("tand")), p.nonNegative(QStringLiteral("sigma"))};
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
                          p.nonNegative(QStringLiteral("sigma")), p.text(QStringLiteral("backed")) == QLatin1String("yes")};
            const double l = p.nonNegative(QStringLiteral("l"));
            if (!p.ok())
                return std::nullopt;
            return physicalLine([g](double f, QString *e) { return cpw(g, f, e); }, l);
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

    // One-port loads: terminations and the ends of a system chain.
    if ((block == QLatin1String("termination") || block == QLatin1String("source") || block == QLatin1String("lo")
         || block == QLatin1String("adc") || block == QLatin1String("antenna"))
        && requirePins(1)) {
        const double temperature =
            block == QLatin1String("antenna") ? p.nonNegative(QStringLiteral("temp")) : T0;
        const Complex gamma = reflectionFromImpedance(z0, SolverZ0);
        return passive([gamma](double) { return oneport(gamma); }, temperature);
    }

    if (block == QLatin1String("amplifier") && requirePins(2)) {
        const double gain = p.number(QStringLiteral("gain"));
        const double nf = p.nonNegative(QStringLiteral("nf"));
        const double isolation = p.number(QStringLiteral("isolation"));
        return sweep([&](double, CMatrix *noise) {
            CMatrix s(2, 2);
            s << 0.0, undb20(-isolation), undb20(gain), 0.0;
            if (noise) {
                // All added noise appears at the output: Te = T0 (F - 1), amplified.
                const double f = std::pow(10.0, nf / 10.0);
                *noise = CMatrix::Zero(2, 2);
                (*noise)(1, 1) = T0 * (f - 1.0) * std::norm(s(1, 0));
            }
            toSolverReference(s, noise, z0);
            return s;
        });
    }

    const auto twoPortThrough = [&](Complex forward, Complex reverse) {
        return passiveBlock(z0, [forward, reverse](double) {
            CMatrix s(2, 2);
            s << 0.0, reverse, forward, 0.0;
            return s;
        });
    };

    if (block == QLatin1String("attenuator") && requirePins(2)) {
        const Complex t = undb20(-p.number(QStringLiteral("il")));
        return twoPortThrough(t, t);
    }
    if (block == QLatin1String("isolator") && requirePins(2)) {
        return twoPortThrough(undb20(-p.number(QStringLiteral("il"))), undb20(-p.number(QStringLiteral("isolation"))));
    }
    if (block == QLatin1String("phase_shifter") && requirePins(2)) {
        const Complex t = fromDbPhase(-p.number(QStringLiteral("il")), -p.number(QStringLiteral("phase")));
        return twoPortThrough(t, t);
    }
    if (block == QLatin1String("cable") && requirePins(2)) {
        const double length = p.nonNegative(QStringLiteral("length"));
        const double lossPerM = p.nonNegative(QStringLiteral("loss_per_m"));
        const double fq = p.positive(QStringLiteral("freq"));
        const double il = p.nonNegative(QStringLiteral("il"));
        const double vf = p.positive(QStringLiteral("vf"));
        return passiveBlock(z0, [=](double f) {
            const double loss = il + lossPerM * length * std::sqrt(f / fq);
            const double phase = 360.0 * f * length / (vf * SpeedOfLight);
            const Complex t = fromDbPhase(-loss, -phase);
            CMatrix s(2, 2);
            s << 0.0, t, t, 0.0;
            return s;
        });
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
        return passiveBlock(z0, [=](double) {
            const double split = 10.0 * std::log10(2.0);
            const Complex ta = fromDbPhase(-(split + il + imbalance / 2), -phaseImbalance / 2);
            const Complex tb = fromDbPhase(-(split + il - imbalance / 2), phaseImbalance / 2);
            const Complex iso = undb20(-isolation);
            CMatrix s = CMatrix::Zero(3, 3);
            s(common, a) = s(a, common) = ta;
            s(common, b) = s(b, common) = tb;
            s(a, b) = s(b, a) = iso;
            return s;
        });
    }
    if (block == QLatin1String("coupler") && requirePins(4)) {
        const double coupling = p.number(QStringLiteral("coupling"));
        const double directivity = p.number(QStringLiteral("directivity"));
        const double il = p.nonNegative(QStringLiteral("il"));
        return passiveBlock(z0, [=](double) {
            const Complex t = undb20(-il);
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
            return s;
        });
    }
    if (block == QLatin1String("switch_spdt") && requirePins(3)) {
        const bool first = p.text(QStringLiteral("state")) != QLatin1String("t2");
        const double il = p.nonNegative(QStringLiteral("il"));
        const double isolation = p.number(QStringLiteral("isolation"));
        return passiveBlock(z0, [=](double) {
            const int on = first ? 1 : 2, off = first ? 2 : 1;
            CMatrix s = CMatrix::Zero(3, 3);
            s(0, on) = s(on, 0) = undb20(-il);
            s(0, off) = s(off, 0) = undb20(-isolation);
            s(on, off) = s(off, on) = undb20(-isolation);
            return s;
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
        return passiveBlock(z0, [=](double) {
            CMatrix s = CMatrix::Zero(3, 3); // rf 0, lo 1, if 2
            s(0, 1) = s(1, 0) = loRf;
            s(1, 2) = s(2, 1) = loIf;
            s(0, 2) = s(2, 0) = rfIf;
            return s;
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

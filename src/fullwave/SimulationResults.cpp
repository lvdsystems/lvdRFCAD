#include "fullwave/SimulationResults.h"

#include "core/JsonFile.h"
#include "fullwave/Hdf5File.h"

#include <QFile>
#include <QRegularExpression>

#include <cmath>
#include <map>
#include <numbers>

namespace lvd::fw {

std::optional<ProbeSignal> readProbeFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setError(error, QStringLiteral("cannot open %1: %2").arg(path, file.errorString()));
        return std::nullopt;
    }
    ProbeSignal s;
    const QRegularExpression split(QStringLiteral("[\\s,]+"));
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('%')) || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList parts = line.split(split, Qt::SkipEmptyParts);
        if (parts.size() < 2)
            continue;
        bool okT = false, okV = false;
        const double t = parts[0].toDouble(&okT);
        const double v = parts[1].toDouble(&okV);
        if (okT && okV) {
            s.time.push_back(t);
            s.value.push_back(v);
        }
    }
    if (s.time.size() < 2) {
        setError(error, QStringLiteral("%1 holds no time samples").arg(path));
        return std::nullopt;
    }
    return s;
}

std::vector<cplx> dftPulse(const ProbeSignal &signal, const std::vector<double> &frequencies)
{
    std::vector<cplx> out(frequencies.size());
    if (signal.time.size() < 2)
        return out;
    const double dt = signal.time[1] - signal.time[0];
    for (std::size_t k = 0; k < frequencies.size(); ++k) {
        const double w = -2 * std::numbers::pi * frequencies[k];
        cplx sum = 0.0;
        for (std::size_t n = 0; n < signal.time.size(); ++n)
            sum += signal.value[n] * std::polar(1.0, w * signal.time[n]);
        out[k] = 2.0 * sum * dt; // single-sided spectrum, as openEMS's DFT_time2freq
    }
    return out;
}

PortResult lumpedPortResult(const ProbeSignal &voltage, const ProbeSignal &current, const std::vector<double> &frequencies,
                            double zRef)
{
    PortResult r;
    r.frequency = frequencies;
    r.zRef = zRef;
    r.voltage = dftPulse(voltage, frequencies);
    r.current = dftPulse(current, frequencies);
    for (std::size_t k = 0; k < frequencies.size(); ++k) {
        const cplx u = r.voltage[k], i = r.current[k];
        const cplx uInc = 0.5 * (u + i * zRef);
        const cplx iInc = 0.5 * (i + u / zRef);
        const cplx uRef = u - uInc;
        r.s11.push_back(std::abs(uInc) > 0 ? uRef / uInc : cplx(0.0));
        r.zin.push_back(std::abs(i) > 0 ? u / i : cplx(INFINITY));
        r.acceptedPower.push_back(0.5 * std::real(u * std::conj(i)));
        r.incidentPower.push_back(0.5 * std::real(uInc * std::conj(iInc)));
    }
    return r;
}

std::optional<FarField> readFarField(const QString &path, QString *error)
{
    Hdf5File file(path);
    if (!file.isOpen()) {
        setError(error, file.error());
        return std::nullopt;
    }
    FarField ff;
    const auto theta = file.readDataset(QStringLiteral("/Mesh/theta"));
    const auto phi = file.readDataset(QStringLiteral("/Mesh/phi"));
    const auto frequency = file.readAttribute(QStringLiteral("/nf2ff"), QStringLiteral("Frequency"));
    if (!theta || !phi || !frequency) {
        setError(error, QStringLiteral("%1 is not an nf2ff far-field file").arg(path));
        return std::nullopt;
    }
    ff.theta = *theta;
    ff.phi = *phi;
    ff.frequency = *frequency;
    if (const auto r = file.readDataset(QStringLiteral("/Mesh/r")); r && !r->empty())
        ff.radius = r->front();
    if (const auto d = file.readAttribute(QStringLiteral("/nf2ff"), QStringLiteral("Dmax")))
        ff.dmax = *d;
    if (const auto p = file.readAttribute(QStringLiteral("/nf2ff"), QStringLiteral("Prad")))
        ff.radiatedPower = *p;
    const std::size_t nt = ff.theta.size(), np = ff.phi.size();
    for (std::size_t f = 0; f < ff.frequency.size(); ++f) {
        std::vector<cplx> components[2];
        int c = 0;
        for (const char *name : {"E_theta", "E_phi"}) {
            const QString base = QStringLiteral("/nf2ff/%1/FD/f%2").arg(QLatin1String(name)).arg(f);
            std::vector<std::uint64_t> dims;
            const auto re = file.readDataset(base + QStringLiteral("_real"), &dims);
            const auto im = file.readDataset(base + QStringLiteral("_imag"));
            if (!re || !im || re->size() != nt * np || im->size() != nt * np) {
                setError(error, QStringLiteral("%1: no legacy far-field data for frequency %2 (write it with LegacyHDF5=\"1\")")
                                    .arg(path)
                                    .arg(f));
                return std::nullopt;
            }
            // Legacy layout: (phi, theta), theta fastest; ours: theta major, phi fastest.
            std::vector<cplx> values(nt * np);
            for (std::size_t ip = 0; ip < np; ++ip)
                for (std::size_t it = 0; it < nt; ++it)
                    values[it * np + ip] = cplx((*re)[ip * nt + it], (*im)[ip * nt + it]);
            components[c++] = std::move(values);
        }
        ff.eTheta.push_back(std::move(components[0]));
        ff.ePhi.push_back(std::move(components[1]));
    }
    return ff;
}

ant::PatternTable toPatternTable(const FarField &ff, const QString &source)
{
    // Theta may span -180..180 (two-sided cuts): fold negative theta to
    // (|theta|, phi + 180), flipping both components, as for imported files.
    struct Sample {
        double theta, phi;
        std::size_t index;
        bool flip;
    };
    // nf2ff stores float32 angles: round to 1e-4 degrees before folding.
    auto key = [](double v) { return std::round(v * 1e4) / 1e4; };
    std::map<double, std::size_t> thetaIndex, phiIndex;
    std::vector<Sample> samples;
    for (std::size_t it = 0; it < ff.theta.size(); ++it)
        for (std::size_t ip = 0; ip < ff.phi.size(); ++ip) {
            double t = key(ant::toDegrees(ff.theta[it]));
            double p = key(ant::toDegrees(ff.phi[ip]));
            bool flip = false;
            if (t < 0) {
                t = -t;
                p += 180;
                flip = true;
            }
            p = std::fmod(std::fmod(p, 360.0) + 360.0, 360.0);
            if (p >= 360 - 1e-6)
                p = 0;
            if (t > 180 + 1e-6)
                continue;
            samples.push_back({key(t), key(p), it * ff.phi.size() + ip, flip});
            thetaIndex.emplace(key(t), 0);
            phiIndex.emplace(key(p), 0);
        }
    ant::PatternTable table;
    table.source = source;
    table.hasPhase = true;
    for (auto &[v, i] : thetaIndex) {
        i = table.theta.size();
        table.theta.push_back(v);
    }
    for (auto &[v, i] : phiIndex) {
        i = table.phi.size();
        table.phi.push_back(v);
    }
    table.frequencies = ff.frequency;
    for (std::size_t f = 0; f < ff.frequency.size(); ++f) {
        std::vector<ant::Field> grid(table.theta.size() * table.phi.size());
        for (const Sample &s : samples) {
            const double sign = s.flip ? -1.0 : 1.0;
            grid[thetaIndex[s.theta] * table.phi.size() + phiIndex[s.phi]] = {sign * ff.eTheta[f][s.index],
                                                                              sign * ff.ePhi[f][s.index]};
        }
        table.fields.push_back(std::move(grid));
    }
    return table;
}

} // namespace lvd::fw

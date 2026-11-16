#include "antenna/Synthesis.h"

#include "antenna/PatternEngine.h"

#include <QJsonArray>

#include <algorithm>

namespace lvd::ant {

double SynthesisSpec::levelAt(double angleDeg) const
{
    double level = sllDb;
    double from = -1;
    for (const MaskSegment &s : segments)
        if (angleDeg >= s.fromDeg && s.fromDeg > from) {
            level = s.levelDb;
            from = s.fromDeg;
        }
    return level;
}

QJsonObject SynthesisSpec::toJson() const
{
    QJsonObject o{{QStringLiteral("mainBeam"), mainBeamDeg},
                  {QStringLiteral("sll"), sllDb},
                  {QStringLiteral("phaseOnly"), phaseOnly},
                  {QStringLiteral("iterations"), iterations}};
    if (!segments.empty()) {
        QJsonArray list;
        for (const MaskSegment &s : segments)
            list.append(QJsonObject{{QStringLiteral("from"), s.fromDeg}, {QStringLiteral("level"), s.levelDb}});
        o.insert(QStringLiteral("segments"), list);
    }
    return o;
}

SynthesisSpec SynthesisSpec::fromJson(const QJsonObject &o)
{
    SynthesisSpec s;
    s.mainBeamDeg = std::clamp(o.value(QStringLiteral("mainBeam")).toDouble(0), 0.0, 90.0);
    s.sllDb = std::min(0.0, o.value(QStringLiteral("sll")).toDouble(-30));
    s.phaseOnly = o.value(QStringLiteral("phaseOnly")).toBool(false);
    s.iterations = std::clamp(o.value(QStringLiteral("iterations")).toInt(300), 1, 100000);
    for (const QJsonValue &v : o.value(QStringLiteral("segments")).toArray()) {
        const QJsonObject seg = v.toObject();
        s.segments.push_back({seg.value(QStringLiteral("from")).toDouble(), std::min(0.0, seg.value(QStringLiteral("level")).toDouble(-40))});
    }
    return s;
}

SynthesisResult synthesise(const ArrayLayout &layout, const std::vector<cplx> &initial, double frequency,
                           const SynthesisSpec &spec, const std::atomic_bool *cancel, const std::function<void(int)> &progress)
{
    SynthesisResult result;
    const std::size_t n = layout.elements.size();
    if (n == 0) {
        result.warnings << QStringLiteral("the array has no elements");
        return result;
    }
    const double lambda = wavelength(frequency);
    const double k = 2 * kPi / lambda;

    double xMin = INFINITY, xMax = -INFINITY, yMin = INFINITY, yMax = -INFINITY, zSpread = 0;
    for (const ArrayElement &e : layout.elements) {
        xMin = std::min(xMin, e.position.x());
        xMax = std::max(xMax, e.position.x());
        yMin = std::min(yMin, e.position.y());
        yMax = std::max(yMax, e.position.y());
        zSpread = std::max(zSpread, std::abs(e.position.z()));
    }
    if (zSpread > lambda / 100 || !layout.identicalOrientation)
        result.warnings << QStringLiteral("synthesis uses the element positions projected on the xy-plane and isotropic elements");
    const bool line = (yMax - yMin) < lambda * 1e-6;
    const double dx = xMax - xMin + std::max(layout.pitchX, lambda / 2);
    const double dy = yMax - yMin + std::max(layout.pitchY, lambda / 2);

    // Sample points in UV.
    std::vector<double> us, vs;
    if (line) {
        const int m = std::clamp(int(16 * n), 1001, 4001);
        for (int i = 0; i < m; ++i) {
            us.push_back(-1.0 + 2.0 * i / (m - 1));
            vs.push_back(0.0);
        }
    } else {
        const double du = 0.4 * lambda / std::max(dx, dy);
        const int g = std::clamp(int(std::ceil(2.0 / du)) + 1, 41, 161);
        for (int iv = 0; iv < g; ++iv)
            for (int iu = 0; iu < g; ++iu) {
                const double u = -1.0 + 2.0 * iu / (g - 1), v = -1.0 + 2.0 * iv / (g - 1);
                if (u * u + v * v <= 1.0) {
                    us.push_back(u);
                    vs.push_back(v);
                }
            }
    }
    const std::size_t m = us.size();

    // Main-beam region and mask bound per sample.
    std::vector<bool> mainBeam(m, false);
    std::vector<double> bound(m, 1.0); // amplitude relative to the peak
    if (spec.mainBeamDeg > 0)
        result.mainBeamDeg = spec.mainBeamDeg;
    else
        result.mainBeamDeg = toDegrees(std::asin(std::min(1.0, 1.5 * lambda / (line ? dx : std::min(dx, dy)))));
    for (std::size_t i = 0; i < m; ++i) {
        const double s = std::hypot(us[i], vs[i]);
        const double angle = toDegrees(std::asin(std::min(1.0, s)));
        bool inside;
        if (spec.mainBeamDeg > 0 || line)
            inside = angle < result.mainBeamDeg;
        else
            inside = std::pow(us[i] * dx / (1.5 * lambda), 2) + std::pow(vs[i] * dy / (1.5 * lambda), 2) < 1.0;
        mainBeam[i] = inside;
        bound[i] = amplitudeFromDb(spec.levelAt(angle));
    }

    // Separable phase terms exp(j k u x) and exp(j k v y), cached per distinct u and v.
    std::vector<cplx> ex(m * n), ey(line ? 0 : m * n);
    parallelFor(m, [&](std::size_t i) {
        for (std::size_t j = 0; j < n; ++j) {
            ex[i * n + j] = std::polar(1.0, k * us[i] * layout.elements[j].position.x());
            if (!line)
                ex[i * n + j] *= std::polar(1.0, k * vs[i] * layout.elements[j].position.y());
        }
    }, cancel);

    std::vector<cplx> w = initial.size() == n ? initial : std::vector<cplx>(n, 1.0);
    std::vector<double> keep(n);
    double peakW = 0;
    for (const cplx &x : w)
        peakW = std::max(peakW, std::abs(x));
    if (peakW <= 0) {
        w.assign(n, 1.0);
        peakW = 1;
    }
    for (std::size_t j = 0; j < n; ++j) {
        w[j] /= peakW;
        keep[j] = std::abs(w[j]) > 0 ? std::abs(w[j]) : 0.0;
    }
    // Phase-only synthesis from symmetric real weights would stay real (the
    // gradient is real too); a small seeded perturbation breaks the symmetry.
    if (spec.phaseOnly) {
        std::uint32_t state = 12345;
        for (cplx &x : w) {
            state = state * 1664525u + 1013904223u;
            x *= std::polar(1.0, 0.3 * (double(state >> 8) / double(1u << 24) - 0.5));
        }
    }

    std::vector<cplx> f(m), r(m), g(n);
    auto arrayFactor = [&] {
        parallelFor(m, [&](std::size_t i) {
            cplx s = 0.0;
            const cplx *row = &ex[i * n];
            for (std::size_t j = 0; j < n; ++j)
                s += w[j] * row[j];
            f[i] = s;
        }, cancel);
    };
    auto evaluate = [&](double *peak, double *worst, double *sll) {
        *peak = 0;
        for (std::size_t i = 0; i < m; ++i)
            if (mainBeam[i])
                *peak = std::max(*peak, std::abs(f[i]));
        *worst = -INFINITY;
        *sll = -INFINITY;
        for (std::size_t i = 0; i < m; ++i) {
            if (mainBeam[i] || *peak <= 0)
                continue;
            const double rel = std::abs(f[i]) / *peak;
            *sll = std::max(*sll, rel);
            *worst = std::max(*worst, 20 * std::log10(std::max(rel, 1e-20) / bound[i]));
        }
        *sll = std::isfinite(*sll) ? 20 * std::log10(std::max(*sll, 1e-20)) : NAN;
    };

    const double mu = 1.0 / double(m);
    for (int it = 0; it < spec.iterations; ++it) {
        if (cancel && cancel->load()) {
            result.cancelled = true;
            break;
        }
        arrayFactor();
        double peak, worst, sll;
        evaluate(&peak, &worst, &sll);
        result.history.push_back(worst);
        result.iterations = it;
        if (worst <= 0.05) {
            result.converged = true;
            break;
        }
        // Residual: the part of the pattern above the mask.
        for (std::size_t i = 0; i < m; ++i) {
            r[i] = 0.0;
            if (mainBeam[i])
                continue;
            const double a = std::abs(f[i]);
            const double b = bound[i] * peak;
            if (a > b)
                r[i] = f[i] * (1.0 - b / a);
        }
        parallelFor(n, [&](std::size_t j) {
            cplx s = 0.0;
            for (std::size_t i = 0; i < m; ++i)
                s += std::conj(ex[i * n + j]) * r[i];
            g[j] = s;
        }, cancel);
        // Over-relaxed step: the residual is small compared with the pattern.
        double top = 0;
        for (std::size_t j = 0; j < n; ++j) {
            w[j] -= 1.8 * mu * g[j];
            if (spec.phaseOnly)
                w[j] = std::polar(keep[j], std::arg(w[j]));
            top = std::max(top, std::abs(w[j]));
        }
        if (top > 0)
            for (cplx &x : w)
                x /= top;
        if (progress)
            progress(int(100.0 * (it + 1) / spec.iterations));
    }
    arrayFactor();
    double peak, worst, sll;
    evaluate(&peak, &worst, &sll);
    result.worstExcessDb = worst;
    result.achievedSllDb = sll;
    result.converged = result.converged || worst <= 0.05;
    result.weights = w;
    if (!result.converged && !result.cancelled)
        result.warnings << QStringLiteral("the mask is not met after %1 iterations (worst excess %2 dB)")
                               .arg(result.iterations + 1)
                               .arg(worst, 0, 'f', 2);
    return result;
}

} // namespace lvd::ant

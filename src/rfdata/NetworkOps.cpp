#include "rfdata/NetworkOps.h"

#include "rfdata/NetworkMath.h"

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lvd::rf {

namespace {

constexpr double Infinity = std::numeric_limits<double>::infinity();

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

// Wraps an angle into (-pi, pi].
double wrapPhase(double angle)
{
    double wrapped = std::remainder(angle, 2.0 * Pi);
    if (wrapped <= -Pi)
        wrapped += 2.0 * Pi;
    return wrapped;
}

// Linear interpolation of magnitude and unwrapped phase between two samples
// (t in [0, 1]). Only the phase step of this interval matters, so unwrapping
// reduces to taking the shortest phase path. A step beyond 90 degrees means
// the curve passes near the origin between the samples (or a sample is
// zero), where magnitude/phase interpolation would bulge around the origin;
// such intervals are interpolated in real and imaginary parts instead.
Complex interpolateValue(Complex a, Complex b, double t)
{
    if (t <= 0)
        return a;
    if (t >= 1)
        return b;
    const double ma = std::abs(a), mb = std::abs(b);
    const double step = wrapPhase(std::arg(b) - std::arg(a));
    if (ma == 0 || mb == 0 || std::abs(step) > Pi / 2)
        return a + t * (b - a);
    return std::polar(ma + t * (mb - ma), std::arg(a) + t * step);
}

CMatrix interpolateMatrix(const CMatrix &a, const CMatrix &b, double t)
{
    CMatrix m(a.rows(), a.cols());
    for (Eigen::Index i = 0; i < a.rows(); ++i)
        for (Eigen::Index j = 0; j < a.cols(); ++j)
            m(i, j) = interpolateValue(a(i, j), b(i, j), t);
    return m;
}

// Position of f on an increasing grid: interval index k and fraction t, with
// f clamped to the grid. `outside` reports whether clamping was needed
// (beyond a relative tolerance that absorbs unit-conversion rounding).
struct GridPosition {
    size_t k = 0;
    double t = 0;
    bool outside = false;
};

GridPosition locate(const std::vector<double> &grid, double f)
{
    GridPosition p;
    const double lo = grid.front(), hi = grid.back();
    const double tolerance = 1e-12 * std::max(std::abs(hi), 1.0);
    if (f < lo - tolerance || f > hi + tolerance)
        p.outside = true;
    if (grid.size() == 1 || f <= lo) {
        p.k = 0;
        p.t = 0;
        return p;
    }
    if (f >= hi) {
        p.k = grid.size() - 2;
        p.t = 1;
        return p;
    }
    const auto upper = std::upper_bound(grid.begin(), grid.end(), f);
    p.k = static_cast<size_t>(upper - grid.begin()) - 1;
    p.t = (f - grid[p.k]) / (grid[p.k + 1] - grid[p.k]);
    return p;
}

template<typename T, typename Interp>
T sampleAt(const std::vector<T> &values, const GridPosition &p, Interp interp)
{
    if (values.size() == 1)
        return values.front();
    return interp(values[p.k], values[p.k + 1], p.t);
}

NoiseParameters interpolateNoise(const NoiseParameters &a, const NoiseParameters &b, double t)
{
    NoiseParameters n;
    n.fminDb = a.fminDb + t * (b.fminDb - a.fminDb);
    n.rn = a.rn + t * (b.rn - a.rn);
    n.gammaOpt = interpolateValue(a.gammaOpt, b.gammaOpt, t);
    return n;
}

QString checkGrid(const std::vector<double> &frequencies)
{
    if (frequencies.empty())
        return QStringLiteral("no frequencies given");
    for (size_t i = 0; i < frequencies.size(); ++i) {
        if (!std::isfinite(frequencies[i]) || frequencies[i] < 0)
            return QStringLiteral("invalid frequency %1").arg(frequencies[i]);
        if (i > 0 && frequencies[i] <= frequencies[i - 1])
            return QStringLiteral("frequencies are not strictly increasing at %1 Hz").arg(frequencies[i]);
    }
    return {};
}

bool isTwoPort(const NetworkData &network, const QString &name, QString *error)
{
    const QString problem = network.validate();
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("%1: %2").arg(name, problem));
        return false;
    }
    if (network.portCount() != 2) {
        setError(error, QStringLiteral("%1 is not a two-port").arg(name));
        return false;
    }
    return true;
}

// Noise wave correlation of a two-port with per-port references z0 from its
// noise parameters (which refer to z0[0]): computed with z0[0] on both
// ports, then transformed to the actual references.
CMatrix correlationFromNoise(const CMatrix &s, const std::vector<double> &z0, const NoiseParameters &noise)
{
    const std::vector<double> common{z0[0], z0[0]};
    if (z0 == common)
        return correlationFromNoiseParameters(s, noise, z0[0]);
    const CMatrix sCommon = renormalize(s, z0, common);
    const CMatrix c = correlationFromNoiseParameters(sCommon, noise, z0[0]);
    const CMatrix t = noiseRenormalization(sCommon, common, z0);
    return t * c * t.adjoint();
}

NoiseParameters noiseFromCorrelation(const CMatrix &s, const std::vector<double> &z0, const CMatrix &correlation)
{
    const std::vector<double> common{z0[0], z0[0]};
    if (z0 == common)
        return noiseParametersFromCorrelation(s, correlation, z0[0]);
    const CMatrix t = noiseRenormalization(s, z0, common);
    return noiseParametersFromCorrelation(renormalize(s, z0, common), t * correlation * t.adjoint(), z0[0]);
}

// Noise wave correlation matrices of a two-port on its own frequency grid:
// the stored matrices, or matrices derived from noise parameters (only when
// the noise grid covers the network grid), or thermal noise at T0 of a
// passive network without noise data, T0 (I - S S^H).
std::optional<std::vector<CMatrix>> noiseCorrelationOf(const NetworkData &network, bool *fromData)
{
    *fromData = false;
    if (network.hasNoiseCorrelation()) {
        *fromData = true;
        return network.noiseCorrelation;
    }
    if (network.hasNoiseParameters()) {
        std::vector<CMatrix> result;
        for (size_t k = 0; k < network.frequencies.size(); ++k) {
            const GridPosition p = locate(network.noiseFrequencies, network.frequencies[k]);
            if (p.outside)
                return std::nullopt;
            const NoiseParameters noise = sampleAt(network.noise, p, interpolateNoise);
            result.push_back(correlationFromNoise(network.s[k], network.z0, noise));
        }
        *fromData = true;
        return result;
    }
    if (!nonPassiveFrequencies(network, 1e-9).empty())
        return std::nullopt;
    std::vector<CMatrix> result;
    for (const CMatrix &s : network.s) {
        const CMatrix identity = CMatrix::Identity(s.rows(), s.cols());
        result.push_back(T0 * (identity - s * s.adjoint()));
    }
    return result;
}

void fillNoiseParameters(NetworkData &network)
{
    network.noiseFrequencies = network.frequencies;
    network.noise.clear();
    for (size_t k = 0; k < network.frequencies.size(); ++k)
        network.noise.push_back(noiseFromCorrelation(network.s[k], network.z0, network.noiseCorrelation[k]));
}

// numpy.unwrap: steps beyond pi are corrected by multiples of 2 pi.
std::vector<double> unwrap(const std::vector<double> &phase)
{
    std::vector<double> result(phase.size());
    double correction = 0;
    for (size_t k = 0; k < phase.size(); ++k) {
        if (k > 0) {
            const double step = phase[k] - phase[k - 1];
            double wrapped = std::fmod(step + Pi, 2.0 * Pi);
            if (wrapped < 0)
                wrapped += 2.0 * Pi;
            wrapped -= Pi;
            if (wrapped == -Pi && step > 0)
                wrapped = Pi;
            if (std::abs(step) >= Pi)
                correction += wrapped - step;
        }
        result[k] = phase[k] + correction;
    }
    return result;
}

} // namespace

std::optional<NetworkData> interpolate(const NetworkData &network, const std::vector<double> &frequencies,
                                       Extrapolation extrapolation, QString *error, QString *warning)
{
    if (warning)
        warning->clear();
    const QString problem = network.validate();
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Invalid network: %1").arg(problem));
        return std::nullopt;
    }
    if (network.frequencies.empty()) {
        setError(error, QStringLiteral("The network has no data"));
        return std::nullopt;
    }
    const QString gridProblem = checkGrid(frequencies);
    if (!gridProblem.isEmpty()) {
        setError(error, QStringLiteral("Invalid target frequencies: %1").arg(gridProblem));
        return std::nullopt;
    }

    NetworkData result;
    result.z0 = network.z0;
    result.comment = network.comment;
    result.frequencies = frequencies;
    int held = 0;
    for (double f : frequencies) {
        const GridPosition p = locate(network.frequencies, f);
        if (p.outside) {
            if (extrapolation == Extrapolation::Error) {
                setError(error, QStringLiteral("Frequency %1 Hz is outside the data range %2 Hz to %3 Hz")
                                    .arg(f)
                                    .arg(network.frequencies.front())
                                    .arg(network.frequencies.back()));
                return std::nullopt;
            }
            ++held;
        }
        result.s.push_back(sampleAt(network.s, p, interpolateMatrix));
        if (network.hasNoiseCorrelation())
            result.noiseCorrelation.push_back(sampleAt(network.noiseCorrelation, p, interpolateMatrix));
    }

    // Noise parameters have their own grid: with Error, target frequencies
    // outside it get no noise parameters; with Hold, the end values are kept.
    int noiseOutside = 0;
    if (network.hasNoiseParameters()) {
        for (double f : frequencies) {
            const GridPosition p = locate(network.noiseFrequencies, f);
            if (p.outside) {
                ++noiseOutside;
                if (extrapolation == Extrapolation::Error)
                    continue;
            }
            result.noiseFrequencies.push_back(f);
            result.noise.push_back(sampleAt(network.noise, p, interpolateNoise));
        }
    }

    if (warning) {
        QStringList notes;
        if (held > 0) {
            notes << QStringLiteral("%1 frequencies outside the data range %2 Hz to %3 Hz use the end values")
                         .arg(held)
                         .arg(network.frequencies.front())
                         .arg(network.frequencies.back());
        }
        if (noiseOutside > 0) {
            notes << QStringLiteral("%1 frequencies are outside the noise data range %2 Hz to %3 Hz; %4")
                         .arg(noiseOutside)
                         .arg(network.noiseFrequencies.front())
                         .arg(network.noiseFrequencies.back())
                         .arg(extrapolation == Extrapolation::Hold ? QStringLiteral("they use the end values")
                                                                   : QStringLiteral("they have no noise parameters"));
        }
        *warning = notes.join(QStringLiteral("; "));
    }
    return result;
}

NetworkData renormalized(const NetworkData &network, const std::vector<double> &z0)
{
    if (z0.size() != network.z0.size() || z0 == network.z0)
        return network;
    NetworkData result = network;
    result.z0 = z0;
    for (size_t k = 0; k < network.s.size(); ++k) {
        result.s[k] = renormalize(network.s[k], network.z0, z0);
        if (k < network.noiseCorrelation.size()) {
            const CMatrix t = noiseRenormalization(network.s[k], network.z0, z0);
            result.noiseCorrelation[k] = t * network.noiseCorrelation[k] * t.adjoint();
        }
    }
    // Fmin and Rn do not depend on the reference; the optimum source
    // impedance stays, so gammaOpt is re-expressed for the new port 1 reference.
    if (z0[0] != network.z0[0]) {
        for (NoiseParameters &noise : result.noise) {
            const Complex zOpt = impedanceFromReflection(noise.gammaOpt, network.z0[0]);
            noise.gammaOpt = reflectionFromImpedance(zOpt, z0[0]);
        }
    }
    return result;
}

std::optional<NetworkData> renumberPorts(const NetworkData &network, const std::vector<int> &order, QString *error)
{
    const QString problem = network.validate();
    if (!problem.isEmpty()) {
        setError(error, QStringLiteral("Invalid network: %1").arg(problem));
        return std::nullopt;
    }
    const int n = network.portCount();
    if (static_cast<int>(order.size()) != n) {
        setError(error, QStringLiteral("The port order has %1 entries for %2 ports").arg(order.size()).arg(n));
        return std::nullopt;
    }
    std::vector<bool> used(static_cast<size_t>(n), false);
    for (int p : order) {
        if (p < 0 || p >= n || used[static_cast<size_t>(p)]) {
            setError(error, QStringLiteral("The port order is not a permutation of 0..%1").arg(n - 1));
            return std::nullopt;
        }
        used[static_cast<size_t>(p)] = true;
    }
    Eigen::MatrixXd permutation = Eigen::MatrixXd::Zero(n, n);
    for (int i = 0; i < n; ++i)
        permutation(i, order[static_cast<size_t>(i)]) = 1;
    const CMatrix p = permutation.cast<Complex>();

    NetworkData result = network;
    for (int i = 0; i < n; ++i)
        result.z0[static_cast<size_t>(i)] = network.z0[static_cast<size_t>(order[static_cast<size_t>(i)])];
    for (size_t k = 0; k < network.s.size(); ++k)
        result.s[k] = p * network.s[k] * p.transpose();
    for (size_t k = 0; k < network.noiseCorrelation.size(); ++k)
        result.noiseCorrelation[k] = p * network.noiseCorrelation[k] * p.transpose();

    // Swapping the ports of a two-port changes its noise parameters: they are
    // recomputed through the noise wave correlation at the noise frequencies.
    if (network.hasNoiseParameters() && order[0] == 1) {
        QString interpolationError;
        const auto atNoise = interpolate(network, network.noiseFrequencies, Extrapolation::Hold, &interpolationError);
        if (!atNoise) {
            setError(error, interpolationError);
            return std::nullopt;
        }
        for (size_t k = 0; k < network.noise.size(); ++k) {
            const CMatrix c = correlationFromNoise(atNoise->s[k], network.z0, network.noise[k]);
            result.noise[k] = noiseFromCorrelation(p * atNoise->s[k] * p.transpose(), result.z0, p * c * p.transpose());
        }
    }
    return result;
}

// Connection of two two-ports by the Redheffer star product, with both waves
// at the junction referred to the same impedance. Noise waves combine as
// c = Ma cA + Mb cB (derived from the same junction equations).
std::optional<NetworkData> cascade(const NetworkData &a, const NetworkData &b, QString *error)
{
    if (!isTwoPort(a, QStringLiteral("First network"), error) || !isTwoPort(b, QStringLiteral("Second network"), error))
        return std::nullopt;
    if (a.frequencies.empty()) {
        setError(error, QStringLiteral("The first network has no data"));
        return std::nullopt;
    }
    QString interpolationError;
    auto bi = interpolate(b, a.frequencies, Extrapolation::Error, &interpolationError);
    if (!bi) {
        setError(error, QStringLiteral("Second network: %1").arg(interpolationError));
        return std::nullopt;
    }
    if (bi->z0[0] != a.z0[1])
        *bi = renormalized(*bi, {a.z0[1], bi->z0[1]});

    bool aFromData = false, bFromData = false;
    const auto noiseA = noiseCorrelationOf(a, &aFromData);
    const auto noiseB = noiseCorrelationOf(*bi, &bFromData);
    const bool withNoise = noiseA && noiseB && (aFromData || bFromData);

    NetworkData result;
    result.frequencies = a.frequencies;
    result.z0 = {a.z0[0], bi->z0[1]};
    for (size_t k = 0; k < a.frequencies.size(); ++k) {
        const CMatrix &sa = a.s[k];
        const CMatrix &sb = bi->s[k];
        const Complex d = 1.0 - sa(1, 1) * sb(0, 0);
        if (std::abs(d) < 1e-14) {
            setError(error, QStringLiteral("The connection is singular at %1 Hz (total reflection loop)").arg(a.frequencies[k]));
            return std::nullopt;
        }
        CMatrix s(2, 2);
        s(0, 0) = sa(0, 0) + sa(0, 1) * sb(0, 0) * sa(1, 0) / d;
        s(0, 1) = sa(0, 1) * sb(0, 1) / d;
        s(1, 0) = sb(1, 0) * sa(1, 0) / d;
        s(1, 1) = sb(1, 1) + sb(1, 0) * sa(1, 1) * sb(0, 1) / d;
        result.s.push_back(s);
        if (withNoise) {
            CMatrix ma(2, 2), mb(2, 2);
            ma << 1.0, sa(0, 1) * sb(0, 0) / d, 0.0, sb(1, 0) / d;
            mb << sa(0, 1) / d, 0.0, sb(1, 0) * sa(1, 1) / d, 1.0;
            result.noiseCorrelation.push_back(ma * (*noiseA)[k] * ma.adjoint() + mb * (*noiseB)[k] * mb.adjoint());
        }
    }
    if (withNoise && (a.hasNoiseParameters() || b.hasNoiseParameters()))
        fillNoiseParameters(result);
    return result;
}

// With [b1; a1] = T [a2; b2], a chain multiplies: T = T_left T_dut T_right.
std::optional<NetworkData> deembed(const NetworkData &measured, const std::optional<NetworkData> &left,
                                   const std::optional<NetworkData> &right, QString *error)
{
    if (!isTwoPort(measured, QStringLiteral("Measured network"), error))
        return std::nullopt;
    if (left && !isTwoPort(*left, QStringLiteral("Left fixture"), error))
        return std::nullopt;
    if (right && !isTwoPort(*right, QStringLiteral("Right fixture"), error))
        return std::nullopt;

    QString interpolationError;
    std::optional<NetworkData> l, r;
    if (left) {
        l = interpolate(*left, measured.frequencies, Extrapolation::Error, &interpolationError);
        if (!l) {
            setError(error, QStringLiteral("Left fixture: %1").arg(interpolationError));
            return std::nullopt;
        }
    }
    if (right) {
        r = interpolate(*right, measured.frequencies, Extrapolation::Error, &interpolationError);
        if (!r) {
            setError(error, QStringLiteral("Right fixture: %1").arg(interpolationError));
            return std::nullopt;
        }
    }
    // The measurement ports must use the outer references of the fixtures.
    const NetworkData m = renormalized(measured, {l ? l->z0[0] : measured.z0[0], r ? r->z0[1] : measured.z0[1]});

    NetworkData result;
    result.frequencies = measured.frequencies;
    result.z0 = {l ? l->z0[1] : measured.z0[0], r ? r->z0[0] : measured.z0[1]};
    result.comment = measured.comment;
    for (size_t k = 0; k < measured.frequencies.size(); ++k) {
        const double f = measured.frequencies[k];
        auto t = sToT(m.s[k]);
        if (!t) {
            setError(error, QStringLiteral("The measured network has no transmission at %1 Hz").arg(f));
            return std::nullopt;
        }
        CMatrix tm = *t;
        if (l) {
            const auto tl = sToT(l->s[k]);
            if (!tl || std::abs(tl->determinant()) < 1e-300) {
                setError(error, QStringLiteral("The left fixture cannot be inverted at %1 Hz").arg(f));
                return std::nullopt;
            }
            tm = tl->inverse() * tm;
        }
        if (r) {
            const auto tr = sToT(r->s[k]);
            if (!tr || std::abs(tr->determinant()) < 1e-300) {
                setError(error, QStringLiteral("The right fixture cannot be inverted at %1 Hz").arg(f));
                return std::nullopt;
            }
            tm = tm * tr->inverse();
        }
        const CMatrix s = tToS(tm);
        if (!s.allFinite()) {
            setError(error, QStringLiteral("De-embedding is singular at %1 Hz").arg(f));
            return std::nullopt;
        }
        result.s.push_back(s);
    }
    return result;
}

bool isPassive(const CMatrix &s, double tolerance)
{
    if (s.size() == 0)
        return true;
    const Eigen::JacobiSVD<CMatrix> svd(s);
    return svd.singularValues()(0) <= 1.0 + tolerance;
}

bool isReciprocal(const CMatrix &s, double tolerance)
{
    if (s.size() == 0)
        return true;
    return (s - s.transpose()).cwiseAbs().maxCoeff() <= tolerance;
}

std::vector<int> nonPassiveFrequencies(const NetworkData &network, double tolerance)
{
    std::vector<int> result;
    for (size_t k = 0; k < network.s.size(); ++k) {
        if (!isPassive(network.s[k], tolerance))
            result.push_back(static_cast<int>(k));
    }
    return result;
}

std::vector<int> nonReciprocalFrequencies(const NetworkData &network, double tolerance)
{
    std::vector<int> result;
    for (size_t k = 0; k < network.s.size(); ++k) {
        if (!isReciprocal(network.s[k], tolerance))
            result.push_back(static_cast<int>(k));
    }
    return result;
}

Complex stabilityDelta(const CMatrix &s)
{
    return s(0, 0) * s(1, 1) - s(0, 1) * s(1, 0);
}

// Rollett: K = (1 - |S11|^2 - |S22|^2 + |Delta|^2) / (2 |S12 S21|); infinite
// for a unilateral two-port.
double stabilityK(const CMatrix &s)
{
    const double den = 2.0 * std::abs(s(0, 1)) * std::abs(s(1, 0));
    const double num = 1.0 - std::norm(s(0, 0)) - std::norm(s(1, 1)) + std::norm(stabilityDelta(s));
    if (den == 0)
        return num >= 0 ? Infinity : -Infinity;
    return num / den;
}

// Edwards and Sinsky, "A new criterion for linear 2-port stability using a
// single geometrically derived parameter", IEEE T-MTT 1992:
// mu = (1 - |S11|^2) / (|S22 - Delta S11*| + |S12 S21|), mu' with ports swapped.
double stabilityMu(const CMatrix &s)
{
    const double den = std::abs(s(1, 1) - stabilityDelta(s) * std::conj(s(0, 0))) + std::abs(s(0, 1) * s(1, 0));
    const double num = 1.0 - std::norm(s(0, 0));
    return den > 0 ? num / den : (num >= 0 ? Infinity : -Infinity);
}

double stabilityMuPrime(const CMatrix &s)
{
    const double den = std::abs(s(0, 0) - stabilityDelta(s) * std::conj(s(1, 1))) + std::abs(s(0, 1) * s(1, 0));
    const double num = 1.0 - std::norm(s(1, 1));
    return den > 0 ? num / den : (num >= 0 ? Infinity : -Infinity);
}

// MAG = |S21/S12| (K - sqrt(K^2 - 1)) for K > 1, otherwise MSG = |S21/S12|
// (as scikit-rf's max_gain and ADS max_gain). A unilateral two-port gives
// |S21|^2 / ((1 - |S11|^2)(1 - |S22|^2)).
double maximumAvailableGainDb(const CMatrix &s)
{
    const double s21 = std::abs(s(1, 0)), s12 = std::abs(s(0, 1));
    if (s21 == 0)
        return -Infinity;
    if (s12 == 0) {
        const double den = (1.0 - std::norm(s(0, 0))) * (1.0 - std::norm(s(1, 1)));
        return den > 0 ? db10(s21 * s21 / den) : Infinity;
    }
    const double k = stabilityK(s);
    double gain = s21 / s12;
    if (k > 1)
        gain *= k - std::sqrt(k * k - 1.0);
    return db10(gain);
}

std::vector<double> unwrappedPhase(const NetworkData &network, int row, int col)
{
    std::vector<double> phase;
    phase.reserve(network.s.size());
    for (const CMatrix &s : network.s) {
        if (row < 0 || col < 0 || row >= s.rows() || col >= s.cols())
            return {};
        phase.push_back(std::arg(s(row, col)));
    }
    return unwrap(phase);
}

// -d(phase)/d(omega) with numpy.gradient's formulas: second-order central
// differences on the (possibly uneven) grid inside, one-sided at the ends.
std::vector<double> groupDelay(const NetworkData &network, int row, int col)
{
    const std::vector<double> phase = unwrappedPhase(network, row, col);
    const size_t n = phase.size();
    if (n != network.frequencies.size())
        return {};
    std::vector<double> delay(n, 0.0);
    if (n < 2)
        return delay;
    std::vector<double> w(n);
    for (size_t k = 0; k < n; ++k)
        w[k] = 2.0 * Pi * network.frequencies[k];
    delay[0] = -(phase[1] - phase[0]) / (w[1] - w[0]);
    delay[n - 1] = -(phase[n - 1] - phase[n - 2]) / (w[n - 1] - w[n - 2]);
    for (size_t k = 1; k + 1 < n; ++k) {
        const double h1 = w[k] - w[k - 1], h2 = w[k + 1] - w[k];
        const double derivative =
            (h1 * h1 * phase[k + 1] - h2 * h2 * phase[k - 1] + (h2 * h2 - h1 * h1) * phase[k]) / (h1 * h2 * (h1 + h2));
        delay[k] = -derivative;
    }
    return delay;
}

double vswr(Complex reflection)
{
    const double m = std::abs(reflection);
    return m < 1 ? (1 + m) / (1 - m) : Infinity;
}

// Hillbrand and Russer, "An efficient method for computer aided noise
// analysis of linear amplifier networks", IEEE T-CAS 1976.
// Chain form: noise sources vn (series) and in (shunt) at the input of the
// noiseless two-port, V1 = A V2 + B I2 + vn, I1 = C V2 + D I2 + in. Its
// correlation in kelvin is
//   CA = <[vn; in][vn; in]^H> / kB
//      = 4 T0 [[Rn, (Fmin - 1)/2 - Rn Yopt*], [(Fmin - 1)/2 - Rn Yopt, Rn |Yopt|^2]],
// the convention of scikit-rf (Network.set_noise_a). With power waves
// a = (V + Z I)/(2 sqrt Z), b = (V - Z I)/(2 sqrt Z), moving the chain
// sources out of the network gives the noise waves of b = S a + c:
//   c = M [vn; in],  M = 1/(2 sqrt Z) [[1 - S11, -Z (1 + S11)], [-S21, -Z S21]].
// So CS = M CA M^H and CA = M^-1 CS M^-H.
namespace {

CMatrix chainToWave(const CMatrix &s, double z0)
{
    CMatrix m(2, 2);
    m << 1.0 - s(0, 0), -z0 * (1.0 + s(0, 0)), -s(1, 0), -z0 * s(1, 0);
    return m / (2.0 * std::sqrt(z0));
}

} // namespace

NoiseParameters noiseParametersFromCorrelation(const CMatrix &s, const CMatrix &correlation, double z0)
{
    NoiseParameters noise;
    const CMatrix m = chainToWave(s, z0);
    const Complex det = m.determinant();
    if (std::abs(det) < 1e-300 || std::abs(s(1, 0)) < 1e-300) {
        noise.fminDb = Infinity;
        noise.rn = Infinity;
        return noise;
    }
    const CMatrix mInverse = m.inverse();
    const CMatrix ca = mInverse * correlation * mInverse.adjoint();
    const double c11 = ca(0, 0).real(), c22 = ca(1, 1).real();
    const Complex c12 = ca(0, 1);
    if (!(c11 > 0)) {
        // No voltage noise: the noise figure is that of a source-independent
        // current noise, minimal for an open (gammaOpt = 1) and equal to 1 there.
        noise.fminDb = 0;
        noise.gammaOpt = 1.0;
        noise.rn = 0;
        return noise;
    }
    noise.rn = c11 / (4.0 * T0);
    const double bOpt = c12.imag() / c11;
    const double gOpt = std::sqrt(std::max(0.0, c22 / c11 - bOpt * bOpt));
    const Complex yOpt(gOpt, bOpt);
    const double fmin = 1.0 + (c12.real() + c11 * gOpt) / (2.0 * T0);
    noise.fminDb = db10(fmin);
    noise.gammaOpt = (1.0 - z0 * yOpt) / (1.0 + z0 * yOpt);
    return noise;
}

CMatrix correlationFromNoiseParameters(const CMatrix &s, const NoiseParameters &noise, double z0)
{
    const double fmin = std::pow(10.0, noise.fminDb / 10.0);
    const Complex yOpt = (1.0 - noise.gammaOpt) / (z0 * (1.0 + noise.gammaOpt));
    CMatrix ca(2, 2);
    ca(0, 0) = noise.rn;
    ca(0, 1) = (fmin - 1.0) / 2.0 - noise.rn * std::conj(yOpt);
    ca(1, 0) = (fmin - 1.0) / 2.0 - noise.rn * yOpt;
    ca(1, 1) = noise.rn * std::norm(yOpt);
    ca *= 4.0 * T0;
    const CMatrix m = chainToWave(s, z0);
    return m * ca * m.adjoint();
}

// F = Fmin + 4 rn |Gs - Gopt|^2 / ((1 - |Gs|^2) |1 + Gopt|^2), rn = Rn / z0.
double noiseFigureDb(const NoiseParameters &noise, Complex gammaS, double z0)
{
    const double fmin = std::pow(10.0, noise.fminDb / 10.0);
    const double den = (1.0 - std::norm(gammaS)) * std::norm(1.0 + noise.gammaOpt);
    if (!(den > 0))
        return Infinity;
    return db10(fmin + 4.0 * noise.rn / z0 * std::norm(gammaS - noise.gammaOpt) / den);
}

} // namespace lvd::rf

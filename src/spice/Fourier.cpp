#include "spice/Fourier.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lvd::spice {

namespace {

constexpr double Pi = std::numbers::pi;

template <typename T>
std::optional<T> fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

bool isPowerOfTwo(std::size_t n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

std::size_t nextPowerOfTwo(std::size_t n)
{
    std::size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

// In-place iterative radix-2 transform, unnormalised; sign -1 forward, +1 inverse.
void radix2(std::vector<Complex> &a, int sign)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::size_t half = len / 2;
        // Twiddles computed directly per stage, not by repeated multiplication,
        // so rounding does not accumulate over large transforms.
        std::vector<Complex> twiddle(half);
        for (std::size_t k = 0; k < half; ++k)
            twiddle[k] = std::polar(1.0, sign * 2.0 * Pi * double(k) / double(len));
        for (std::size_t i = 0; i < n; i += len) {
            for (std::size_t k = 0; k < half; ++k) {
                const Complex u = a[i + k];
                const Complex v = a[i + k + half] * twiddle[k];
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
        }
    }
}

// Bluestein: X[k] = w[k] * sum_n (x[n] w[n]) conj(w[k-n]) with w[k] = e^{sign j pi k^2 / N},
// a convolution done with power-of-two transforms.
void bluestein(std::vector<Complex> &a, int sign)
{
    const std::size_t n = a.size();
    const std::size_t m = nextPowerOfTwo(2 * n - 1);
    std::vector<Complex> chirp(n);
    for (std::size_t k = 0; k < n; ++k) {
        // k^2 mod 2N keeps the angle small, so large k lose no precision.
        const auto k2 = (static_cast<unsigned long long>(k) * k) % (2ull * n);
        chirp[k] = std::polar(1.0, sign * Pi * double(k2) / double(n));
    }
    std::vector<Complex> u(m), v(m);
    for (std::size_t k = 0; k < n; ++k)
        u[k] = a[k] * chirp[k];
    v[0] = std::conj(chirp[0]);
    for (std::size_t k = 1; k < n; ++k)
        v[k] = v[m - k] = std::conj(chirp[k]);
    radix2(u, -1);
    radix2(v, -1);
    for (std::size_t k = 0; k < m; ++k)
        u[k] *= v[k];
    radix2(u, 1);
    for (std::size_t k = 0; k < n; ++k)
        a[k] = chirp[k] * u[k] / double(m);
}

// Cosine-sum coefficients a0, a1, ... of w[n] = sum (-1)^k a_k cos(2 pi k n / N).
std::vector<double> cosineSum(WindowType type)
{
    switch (type) {
    case WindowType::Rectangular:
        return {1.0};
    case WindowType::Hann:
        return {0.5, 0.5};
    case WindowType::Hamming:
        return {0.54, 0.46};
    case WindowType::Blackman:
        return {0.42, 0.5, 0.08};
    case WindowType::BlackmanHarris:
        return {0.35875, 0.48829, 0.14128, 0.01168};
    case WindowType::FlatTop:
        // HFT95 (Heinzel, Ruediger, Schilling 2002): scalloping below 0.0044 dB,
        // half that of the classic SRS/Matlab flat top, sidelobes at -95 dB.
        return {1.0, 1.9383379, 1.3045202, 0.4028270, 0.0350665};
    }
    return {1.0};
}

double wrapDegrees(double deg)
{
    deg = std::remainder(deg, 360.0);
    return deg <= -180.0 ? deg + 360.0 : deg;
}

// E0 = integral_0^1 e^{-j theta v} dv and E1 = integral_0^1 v e^{-j theta v} dv,
// with a series for small theta where the closed forms cancel.
void segmentKernels(double theta, Complex &e0, Complex &e1)
{
    if (std::abs(theta) < 0.5) {
        const Complex a(0.0, -theta);
        Complex term(1.0, 0.0); // a^k / k!
        e0 = 0;
        e1 = 0;
        for (int k = 0; k < 20; ++k) {
            e0 += term / double(k + 1);
            e1 += term / double(k + 2);
            term *= a / double(k + 1);
        }
        return;
    }
    const Complex a(0.0, -theta);
    const Complex ea = std::exp(a);
    e0 = (ea - 1.0) / a;
    e1 = ea / a - (ea - 1.0) / (a * a);
}

} // namespace

std::vector<Complex> fft(std::vector<Complex> data, bool inverse)
{
    const std::size_t n = data.size();
    if (n <= 1)
        return data;
    const int sign = inverse ? 1 : -1;
    if (isPowerOfTwo(n))
        radix2(data, sign);
    else
        bluestein(data, sign);
    if (inverse) {
        for (auto &value : data)
            value /= double(n);
    }
    return data;
}

QString windowName(WindowType type)
{
    switch (type) {
    case WindowType::Rectangular:
        return QStringLiteral("Rectangular");
    case WindowType::Hann:
        return QStringLiteral("Hann");
    case WindowType::Hamming:
        return QStringLiteral("Hamming");
    case WindowType::Blackman:
        return QStringLiteral("Blackman");
    case WindowType::BlackmanHarris:
        return QStringLiteral("Blackman-Harris");
    case WindowType::FlatTop:
        return QStringLiteral("Flat top");
    }
    return QString();
}

QList<WindowType> allWindows()
{
    return {WindowType::Rectangular, WindowType::Hann,           WindowType::Hamming,
            WindowType::Blackman,    WindowType::BlackmanHarris, WindowType::FlatTop};
}

std::optional<WindowType> windowFromName(const QString &name)
{
    const auto simplify = [](QString text) {
        text.remove(QLatin1Char(' '));
        text.remove(QLatin1Char('-'));
        text.remove(QLatin1Char('_'));
        return text.toLower();
    };
    const QString key = simplify(name);
    for (const WindowType type : allWindows()) {
        if (simplify(windowName(type)) == key)
            return type;
    }
    if (key == QLatin1String("rect") || key == QLatin1String("none") || key == QLatin1String("uniform"))
        return WindowType::Rectangular;
    if (key == QLatin1String("hanning"))
        return WindowType::Hann;
    return std::nullopt;
}

std::vector<double> windowCoefficients(WindowType type, int n)
{
    if (n <= 0)
        return {};
    const auto a = cosineSum(type);
    std::vector<double> w(std::size_t(n), 0.0);
    for (int i = 0; i < n; ++i) {
        double sum = 0;
        for (std::size_t k = 0; k < a.size(); ++k) {
            const double sign = (k % 2) ? -1.0 : 1.0;
            sum += sign * a[k] * std::cos(2.0 * Pi * double(k) * double(i) / double(n));
        }
        w[std::size_t(i)] = sum;
    }
    return w;
}

std::optional<Spectrum> amplitudeSpectrum(const Waveform &w, const SpectrumOptions &options, QString *error)
{
    auto part = slice(w, options.range, error);
    if (!part)
        return std::nullopt;
    const double from = part->x.front();
    const double width = part->x.back() - from;
    if (!(width > 0))
        return fail<Spectrum>(error, QStringLiteral("the range has zero width"));

    constexpr std::size_t MaxPoints = std::size_t(1) << 22;
    std::size_t n = 0;
    if (options.points > 0) {
        n = std::size_t(options.points);
        if (n < 2 || n > 4 * MaxPoints)
            return fail<Spectrum>(error, QStringLiteral("the number of points must be between 2 and %1").arg(4 * MaxPoints));
    } else {
        n = std::clamp(nextPowerOfTwo(part->x.size()), std::size_t(1024), MaxPoints);
    }

    // Uniform resampling by walking the piecewise-linear trace once.
    const double dt = width / double(n);
    const auto window = windowCoefficients(options.window, int(n));
    double gain = 0;
    for (const double c : window)
        gain += c;
    gain /= double(n);

    std::vector<Complex> data(n);
    std::size_t seg = 0;
    const auto &x = part->x;
    for (std::size_t k = 0; k < n; ++k) {
        const double t = from + double(k) * dt;
        while (seg + 2 < x.size() && x[seg + 1] <= t)
            ++seg;
        const double x0 = x[seg];
        const double x1 = x[seg + 1];
        const double y0 = part->y[seg].real();
        const double y1 = part->y[seg + 1].real();
        const double v = x1 > x0 ? y0 + (y1 - y0) * (t - x0) / (x1 - x0) : y1;
        data[k] = v * window[k];
    }
    data = fft(std::move(data));

    Spectrum result;
    result.sampleRate = 1.0 / dt;
    result.resolution = 1.0 / width;
    const std::size_t bins = n / 2 + 1;
    result.frequency.resize(bins);
    result.value.resize(bins);
    for (std::size_t k = 0; k < bins; ++k) {
        // DC and (for even n) Nyquist have no mirror image to fold in.
        const bool single = k == 0 || (n % 2 == 0 && k == n / 2);
        result.frequency[k] = double(k) * result.resolution;
        result.value[k] = data[k] * ((single ? 1.0 : 2.0) / (double(n) * gain));
    }
    return result;
}

std::optional<FourierResult> fourier(const Waveform &w, double fundamental, int harmonics, int periods,
                                     XWindow window, QString *error)
{
    if (harmonics < 1)
        return fail<FourierResult>(error, QStringLiteral("at least one harmonic is needed"));
    if (periods < 1)
        return fail<FourierResult>(error, QStringLiteral("at least one period is needed"));
    auto whole = slice(w, window, error);
    if (!whole)
        return std::nullopt;
    if (!(fundamental > 0)) {
        QString reason;
        const auto f = frequency(w, window, &reason);
        if (!f)
            return fail<FourierResult>(error, QStringLiteral("cannot find the fundamental: %1").arg(reason));
        fundamental = *f;
    }
    if (!std::isfinite(fundamental))
        return fail<FourierResult>(error, QStringLiteral("the fundamental frequency is not finite"));

    const double length = double(periods) / fundamental;
    const double to = whole->x.back();
    const double from = to - length;
    // A relative slack absorbs rounding when the trace is exactly `periods` long.
    if (from < whole->x.front() - 1e-9 * length) {
        return fail<FourierResult>(error, QStringLiteral("the trace is shorter than %1 period(s) of %2 Hz")
                                              .arg(periods)
                                              .arg(fundamental));
    }
    const auto part = slice(w, XWindow{std::max(from, whole->x.front()), to}, error);
    if (!part)
        return std::nullopt;

    // c_n = (2/T) integral y(t) e^{-j n w0 (t - from)} dt, exact for each linear segment.
    const double w0 = 2.0 * Pi * fundamental;
    std::vector<Complex> c(std::size_t(harmonics) + 1, Complex(0, 0));
    for (std::size_t i = 1; i < part->x.size(); ++i) {
        const double x0 = part->x[i - 1];
        const double h = part->x[i] - x0;
        if (!(h > 0))
            continue;
        const double y0 = part->y[i - 1].real();
        const double y1 = part->y[i].real();
        c[0] += 0.5 * h * (y0 + y1);
        for (int n = 1; n <= harmonics; ++n) {
            Complex e0, e1;
            segmentKernels(n * w0 * h, e0, e1);
            c[std::size_t(n)] += std::polar(h, -n * w0 * (x0 - from)) * (y0 * (e0 - e1) + y1 * e1);
        }
    }

    FourierResult result;
    result.fundamental = fundamental;
    result.from = from;
    result.to = to;
    result.dc = c[0].real() / length;
    double distortion = 0;
    for (int n = 1; n <= harmonics; ++n) {
        const Complex cn = c[std::size_t(n)] * (2.0 / length);
        // cn = a - j b for y = a cos + b sin; SPICE reports the phase of
        // M sin(n w0 t + phase), so phase = atan2(a, b).
        Harmonic h;
        h.n = n;
        h.frequency = n * fundamental;
        h.magnitude = std::abs(cn);
        h.phaseDeg = std::atan2(cn.real(), -cn.imag()) * 180.0 / Pi;
        result.harmonics.push_back(h);
        if (n >= 2)
            distortion += h.magnitude * h.magnitude;
    }
    const Harmonic &first = result.harmonics.front();
    if (!(first.magnitude > 0))
        return fail<FourierResult>(error, QStringLiteral("the fundamental has zero amplitude"));
    for (auto &h : result.harmonics) {
        h.normalizedMagnitude = h.magnitude / first.magnitude;
        h.normalizedPhaseDeg = wrapDegrees(h.phaseDeg - first.phaseDeg);
    }
    result.thdPercent = std::sqrt(distortion) / first.magnitude * 100.0;
    return result;
}

std::optional<double> thd(const Waveform &w, double fundamental, int harmonics, int periods, XWindow window,
                          QString *error)
{
    const auto result = fourier(w, fundamental, harmonics, periods, window, error);
    if (!result)
        return std::nullopt;
    return result->thdPercent;
}

} // namespace lvd::spice

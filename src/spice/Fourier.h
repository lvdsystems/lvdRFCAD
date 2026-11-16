#pragma once

#include "spice/WaveformMath.h"

#include <QList>
#include <QString>

#include <optional>
#include <vector>

namespace lvd::spice {

// Discrete Fourier transform of any length: radix-2 for powers of two,
// Bluestein's chirp-z (through a radix-2 convolution) otherwise. The forward
// transform is unnormalised, X[k] = sum x[n] e^{-j 2 pi k n / N}; the inverse
// divides by N, so fft(fft(x), true) == x.
std::vector<Complex> fft(std::vector<Complex> data, bool inverse = false);

enum class WindowType { Rectangular, Hann, Hamming, Blackman, BlackmanHarris, FlatTop };

// "Rectangular", "Hann", "Hamming", "Blackman", "Blackman-Harris", "Flat top";
// windowFromName ignores case, spaces and dashes.
QString windowName(WindowType type);
std::optional<WindowType> windowFromName(const QString &name);
QList<WindowType> allWindows();

// Periodic (DFT-even) window of n points, as used for spectral analysis.
// Blackman-Harris is the 4-term, -92 dB version; flat top is HFT95 (amplitude
// error below 0.0044 dB = 0.05 % anywhere between bins, 3.8 bins wide).
std::vector<double> windowCoefficients(WindowType type, int n);

struct SpectrumOptions {
    WindowType window = WindowType::Hann;
    XWindow range;
    int points = 0; // 0: next power of two >= samples in range, at least 1024, at most 2^22
};

struct Spectrum {
    std::vector<double> frequency; // 0 .. fs/2, points/2 + 1 bins
    // Single-sided peak amplitude, corrected for the window's coherent gain:
    // a 1 V sine at a bin centre reads 1 V there. The phase is that of a
    // cosine starting at the range start (a sine reads -90 degrees).
    std::vector<Complex> value;
    double resolution = 0; // Hz per bin = 1 / range width
    double sampleRate = 0;
};

// Resamples the real part of the (non-uniform) trace uniformly over the range
// (points samples spaced width/points apart, the range end excluded, so a
// periodic signal with a whole number of periods in the range lands on bins),
// applies the window and transforms. The scale is taken as seconds.
std::optional<Spectrum> amplitudeSpectrum(const Waveform &w, const SpectrumOptions &options = {},
                                          QString *error = nullptr);

struct Harmonic {
    int n = 0;
    double frequency = 0;
    double magnitude = 0;          // peak amplitude
    double phaseDeg = 0;           // sine reference, relative to the analysed interval start (as SPICE .four)
    double normalizedMagnitude = 0; // magnitude / fundamental magnitude
    double normalizedPhaseDeg = 0;  // phase - fundamental phase, wrapped to (-180, 180]
};

struct FourierResult {
    double fundamental = 0; // Hz
    double dc = 0;
    std::vector<Harmonic> harmonics; // n = 1 .. count
    double thdPercent = 0;           // sqrt(sum_{n>=2} |c_n|^2) / |c_1| * 100, over the computed harmonics
    double from = 0;                 // analysed interval
    double to = 0;
};

// Like SPICE .four: Fourier coefficients of the last `periods` full periods
// of the fundamental, ending at the end of the trace (or window.to). The
// coefficients are integrated exactly over the piecewise-linear trace
// (closed form per segment), which is the limit of the fine uniform
// resampling SPICE uses, so non-uniform time steps and sharp edges cost no
// accuracy. fundamental <= 0 estimates it with frequency() over the window.
std::optional<FourierResult> fourier(const Waveform &w, double fundamental = 0, int harmonics = 9, int periods = 1,
                                     XWindow window = {}, QString *error = nullptr);

// THD in percent; same arguments as fourier().
std::optional<double> thd(const Waveform &w, double fundamental = 0, int harmonics = 9, int periods = 1,
                          XWindow window = {}, QString *error = nullptr);

} // namespace lvd::spice

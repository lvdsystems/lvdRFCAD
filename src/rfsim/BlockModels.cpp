#include "rfsim/BlockModels.h"

#include "rfdata/NetworkMath.h"
#include "rfsim/ElementModels.h"

#include <Eigen/SVD>

#include <algorithm>
#include <cmath>

namespace lvd::rf {

namespace {

const Complex J(0, 1);

double undb20(double db)
{
    return std::pow(10.0, db / 20.0);
}

// Amplitudes of the two split paths: path a with +imbalance/2 dB of loss, path b with -imbalance/2.
void splitPaths(double ilDb, double imbalanceDb, Complex *a, Complex *b)
{
    const double split = 10.0 * std::log10(2.0);
    *a = undb20(-(split + ilDb + imbalanceDb / 2));
    *b = undb20(-(split + ilDb - imbalanceDb / 2));
}

// Phase imbalance as a phase shift at the two output ports (-phase/2 at `first`,
// +phase/2 at `second`), applied as D S D with a diagonal unitary D: it moves the
// phases of the paths through those ports without changing passivity.
CMatrix withPortPhases(const CMatrix &s, int first, int second, double phaseImbalanceDegrees)
{
    CMatrix d = CMatrix::Identity(s.rows(), s.cols());
    d(first, first) = std::polar(1.0, -phaseImbalanceDegrees / 2 * Pi / 180);
    d(second, second) = std::polar(1.0, phaseImbalanceDegrees / 2 * Pi / 180);
    return d * s * d;
}

} // namespace

double returnLossToGamma(double returnLossDb)
{
    return returnLossDb > 0 ? undb20(-returnLossDb) : 0.0;
}

double sloped(double value, double slopePerGHz, double frequency, double reference, bool nonNegative)
{
    if (slopePerGHz == 0)
        return value;
    const double result = value + slopePerGHz * (frequency - reference) / 1e9;
    return nonNegative ? std::max(0.0, result) : result;
}

double largestSingularValue(const CMatrix &s)
{
    if (s.size() == 0)
        return 0;
    return Eigen::JacobiSVD<CMatrix>(s).singularValues()(0);
}

CMatrix limitToPassive(const CMatrix &s)
{
    constexpr double tolerance = 1e-12;
    if (largestSingularValue(s) <= 1.0 + tolerance)
        return s;
    const CMatrix diagonal = s.diagonal().asDiagonal();
    const CMatrix offDiagonal = s - diagonal;
    double low = 0, high = 1;
    for (int i = 0; i < 60; ++i) {
        const double k = (low + high) / 2;
        if (largestSingularValue(diagonal + k * offDiagonal) <= 1.0)
            low = k;
        else
            high = k;
    }
    return diagonal + low * offDiagonal;
}

CMatrix withPortReflections(const CMatrix &s, const std::vector<double> &gamma, Complex phase, bool passive)
{
    if (std::none_of(gamma.begin(), gamma.end(), [](double g) { return g > 0; }))
        return s;
    CMatrix result = s;
    const Complex direction = std::abs(phase) > 0 ? phase / std::abs(phase) : Complex(1, 0);
    for (Eigen::Index i = 0; i < result.rows() && i < static_cast<Eigen::Index>(gamma.size()); ++i)
        result(i, i) = J * gamma[static_cast<size_t>(i)] * direction;
    return passive ? limitToPassive(result) : result;
}

CMatrix multiThrowSwitch(int throws, int selected, double ilDb, double isolationDb)
{
    const int n = throws + 1;
    CMatrix s = CMatrix::Zero(n, n);
    const Complex isolation = undb20(-isolationDb);
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < n; ++k) {
            if (i != k)
                s(i, k) = isolation;
        }
    }
    s(0, selected) = s(selected, 0) = undb20(-ilDb);
    // The SPDT keeps the matrix it always had; the larger switches are kept passive.
    return throws > 2 ? limitToPassive(s) : s;
}

CMatrix hybrid90(double ilDb, double isolationDb, double imbalanceDb, double phaseImbalanceDegrees)
{
    Complex a, b;
    splitPaths(ilDb, imbalanceDb, &a, &b);
    const Complex isolation = undb20(-isolationDb);
    CMatrix s = CMatrix::Zero(4, 4); // in 0, out0 1, out90 2, iso 3
    s(0, 1) = s(1, 0) = a;
    s(0, 2) = s(2, 0) = -J * b;
    s(1, 3) = s(3, 1) = -J * b;
    s(2, 3) = s(3, 2) = a;
    s(0, 3) = s(3, 0) = isolation;
    s(1, 2) = s(2, 1) = isolation;
    return limitToPassive(withPortPhases(s, 1, 2, phaseImbalanceDegrees));
}

CMatrix hybrid180(double ilDb, double isolationDb, double imbalanceDb, double phaseImbalanceDegrees)
{
    Complex a, b;
    splitPaths(ilDb, imbalanceDb, &a, &b);
    const Complex isolation = undb20(-isolationDb);
    CMatrix s = CMatrix::Zero(4, 4); // sum 0, diff 1, p1 2, p2 3
    s(0, 2) = s(2, 0) = a;
    s(0, 3) = s(3, 0) = b;
    s(1, 2) = s(2, 1) = a;
    s(1, 3) = s(3, 1) = -b;
    s(0, 1) = s(1, 0) = isolation;
    s(2, 3) = s(3, 2) = isolation;
    return limitToPassive(withPortPhases(s, 2, 3, phaseImbalanceDegrees));
}

CMatrix balun(double ilDb, double isolationDb, double imbalanceDb, double phaseImbalanceDegrees)
{
    Complex a, b;
    splitPaths(ilDb, imbalanceDb, &a, &b);
    CMatrix s = CMatrix::Zero(3, 3); // unbal 0, bal_p 1, bal_n 2
    s(0, 1) = s(1, 0) = a;
    s(0, 2) = s(2, 0) = -b;
    s(1, 2) = s(2, 1) = undb20(-isolationDb);
    return limitToPassive(withPortPhases(s, 1, 2, phaseImbalanceDegrees));
}

CMatrix diplexer(double frequency, double crossover, int order, double ilDb)
{
    const double inf = std::numeric_limits<double>::infinity();
    const Complex low = prototypeResponse(FilterResponse::Butterworth, order, 0, frequency / crossover);
    const Complex high = prototypeResponse(FilterResponse::Butterworth, order, 0, frequency > 0 ? -crossover / frequency : -inf);
    CMatrix s(3, 3); // com 0, low 1, high 2
    const Complex cross = -std::conj(high * low);
    s << 0.0, low, high, low, std::conj(high) * std::conj(high), cross, high, cross, std::conj(low) * std::conj(low);
    return s * undb20(-ilDb);
}

CMatrix dcBlock(double frequency, double fc, double ilDb)
{
    CMatrix s(2, 2);
    if (!(frequency > 0)) {
        s << 1.0, 0.0, 0.0, 1.0; // open at DC
        return s;
    }
    // Series capacitor with 1 / (omega C 2 z0) = fc / f: z / 2z0 = -j fc / f.
    const Complex x = -J * (fc / frequency);
    const Complex reflection = x / (1.0 + x);
    const Complex transmission = 1.0 / (1.0 + x) * undb20(-ilDb);
    s << reflection, transmission, transmission, reflection;
    return s;
}

double sincDb(double frequency, double fs)
{
    if (!(fs > 0))
        return 0;
    const double x = Pi * frequency / fs;
    if (std::abs(x) < 1e-12)
        return 0;
    const double value = std::abs(std::sin(x) / x);
    return value > 0 ? 20.0 * std::log10(value) : -INFINITY;
}

DacFigures dacFigures(double frequency, double fs, double bits, double fullScaleDbm, double backoffDb, double nsdDbmHz)
{
    DacFigures d;
    d.sincDb = sincDb(frequency, fs);
    d.signalDbm = fullScaleDbm - backoffDb + d.sincDb;
    d.nsdFromQuantisation = nsdDbmHz == 0;
    d.nsdDbmHz = d.nsdFromQuantisation ? fullScaleDbm - 6.02 * bits - 1.76 - 10.0 * std::log10(fs / 2) : nsdDbmHz;
    d.imageFrequency = std::abs(fs - frequency);
    d.imageDbm = fullScaleDbm - backoffDb + sincDb(d.imageFrequency, fs);
    d.noiseTemperature = std::pow(10.0, (d.nsdDbmHz - 30.0) / 10.0) / Boltzmann;
    return d;
}

} // namespace lvd::rf

#pragma once

#include <QPointF>
#include <QPolygonF>

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace lvd {

// Reflection coefficient and impedance, for a reference impedance z0 (ohm).
inline std::complex<double> gammaFromImpedance(std::complex<double> z, double z0)
{
    return (z - z0) / (z + z0);
}

inline std::complex<double> impedanceFromGamma(std::complex<double> gamma, double z0)
{
    return z0 * (1.0 + gamma) / (1.0 - gamma);
}

// Infinite for |gamma| >= 1.
inline double vswrFromGamma(std::complex<double> gamma)
{
    const double m = std::abs(gamma);
    return m < 1 ? (1 + m) / (1 - m) : INFINITY;
}

inline double magnitudeDb(std::complex<double> value)
{
    return 20.0 * std::log10(std::abs(value));
}

// Min/max decimation of a polyline whose x values increase: the points falling
// in one pixel column are reduced to that column's minimum and maximum (in
// their original order), so every extreme stays visible and the drawn line
// looks like the full one. mapX and mapY return pixel coordinates; a
// non-finite x or y starts a new segment.
template <class MapX, class MapY>
void decimateMinMax(const double *x, const double *y, std::size_t begin, std::size_t end, MapX mapX, MapY mapY,
                    std::vector<QPolygonF> &segments)
{
    QPolygonF current;
    struct Column {
        long long index = 0;
        QPointF min, max;
        std::size_t iMin = 0, iMax = 0;
        int count = 0;
    } column;

    const auto flush = [&]() {
        if (column.count == 0)
            return;
        // The extremes in data order; first and last lie between them, so
        // the vertical stroke already covers them.
        if (column.iMin == column.iMax) {
            current.append(column.min);
        } else if (column.iMin < column.iMax) {
            current.append(column.min);
            current.append(column.max);
        } else {
            current.append(column.max);
            current.append(column.min);
        }
        column.count = 0;
    };
    const auto breakLine = [&]() {
        flush();
        if (!current.isEmpty())
            segments.push_back(std::move(current));
        current = QPolygonF();
    };

    for (std::size_t i = begin; i < end; ++i) {
        const double px = mapX(x[i]);
        const double py = mapY(y[i]);
        if (!std::isfinite(px) || !std::isfinite(py)) {
            breakLine();
            continue;
        }
        const QPointF p(px, py);
        const auto index = static_cast<long long>(std::floor(px));
        if (column.count > 0 && index != column.index)
            flush();
        if (column.count == 0) {
            column.index = index;
            column.min = column.max = p;
            column.iMin = column.iMax = i;
            column.count = 1;
            continue;
        }
        // Pixel y grows downwards, but "min" and "max" only need to be the
        // two extremes, whichever direction.
        if (py < column.min.y()) {
            column.min = p;
            column.iMin = i;
        }
        if (py > column.max.y()) {
            column.max = p;
            column.iMax = i;
        }
        ++column.count;
    }
    breakLine();
}

// One pixel column of a decimated trace: the vertical extent of the line.
struct EnvelopeColumn {
    double x = 0; // column centre
    double min = 0;
    double max = 0;
};

// Like decimateMinMax, but returns per pixel column the range the line covers
// (including half of the segments joining neighbouring columns), so a dense
// trace can be filled as one polygon instead of stroked point by point.
template <class MapX, class MapY>
void decimateEnvelope(const double *x, const double *y, std::size_t begin, std::size_t end, MapX mapX, MapY mapY,
                      std::vector<std::vector<EnvelopeColumn>> &segments)
{
    std::vector<EnvelopeColumn> current;
    bool open = false;
    long long index = 0;
    double lastY = 0;
    EnvelopeColumn column;
    const auto close = [&]() {
        if (open)
            current.push_back(column);
        open = false;
        if (!current.empty())
            segments.push_back(std::move(current));
        current.clear();
    };
    for (std::size_t i = begin; i < end; ++i) {
        const double px = mapX(x[i]);
        const double py = mapY(y[i]);
        if (!std::isfinite(px) || !std::isfinite(py)) {
            close();
            continue;
        }
        const auto k = static_cast<long long>(std::floor(px));
        if (open && k == index) {
            column.min = std::min(column.min, py);
            column.max = std::max(column.max, py);
            lastY = py;
            continue;
        }
        if (open) {
            const double mid = (lastY + py) / 2;
            column.min = std::min(column.min, mid);
            column.max = std::max(column.max, mid);
            current.push_back(column);
            column.min = std::min(py, mid);
            column.max = std::max(py, mid);
        } else {
            column.min = column.max = py;
        }
        column.x = static_cast<double>(k) + 0.5;
        index = k;
        lastY = py;
        open = true;
    }
    close();
}

} // namespace lvd

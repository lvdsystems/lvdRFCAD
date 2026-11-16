#pragma once

// Internal structures of PlotWidget, shared by its implementation files.

#include "plot/PlotWidget.h"

#include <QFont>
#include <QRectF>

#include <utility>

namespace lvd {

struct PlotWidget::Trace {
    int id = -1;
    QString name;
    QString unit;
    TraceStyle style;
    int colorSlot = 0;
    bool visible = true;

    bool isComplex = false;
    ComplexPart part = ComplexPart::MagnitudeDb;
    std::vector<double> x;
    std::vector<double> y; // real data, or the shown part of the complex values
    std::vector<std::complex<double>> z;

    // Statistics over finite values, refreshed when the data change.
    bool xAscending = true;
    double xMin = NAN, xMax = NAN, xMinPositive = NAN;
    double yMin = NAN, yMax = NAN, yMinPositive = NAN;
    double polarMax = NAN; // largest polar value

    std::size_t size() const { return x.size(); }
};

struct PlotWidget::SampledValue {
    double y = 0;
    std::complex<double> z;
};

// Geometry of one rendering (widget or export).
struct PlotWidget::Layout {
    QRectF target;
    QRectF titleRect;
    QRectF legendRect;
    std::vector<std::pair<int, QRectF>> legendItems;
    QRectF plotRect;
    QRectF tableRect; // Smith and polar: band reserved for the marker table
    QFont font;
    QFont titleFont;
    QFont smallFont;
    bool interactive = false;

    // Rectangular.
    AxisScale x, yLeft, yRight;
    bool rightAxis = false;
    AxisTicks xTicks, yLeftTicks, yRightTicks;

    // Smith and polar: pixel position of the view centre and pixels per unit.
    QPointF centre;
    double scale = 1;
    std::complex<double> viewCentre;

    QPointF gammaToPixel(std::complex<double> g) const
    {
        const std::complex<double> d = g - viewCentre;
        return {centre.x() + d.real() * scale, centre.y() - d.imag() * scale};
    }
    std::complex<double> pixelToGamma(const QPointF &p) const
    {
        return viewCentre + std::complex<double>((p.x() - centre.x()) / scale, (centre.y() - p.y()) / scale);
    }
};

} // namespace lvd

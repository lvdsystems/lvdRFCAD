#pragma once

// Access to the scikit-rf reference data in tests/data (written by
// tools/generate_rf_reference.py). Complex numbers are [re, im] pairs,
// matrices lists of rows.

#include "rfdata/RfTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace lvd::rf::test {

QString dataPath(const QString &name);
QJsonObject loadJson(const QString &name); // empty object (and a Catch2 failure) when missing

Complex toComplex(const QJsonValue &value);
CMatrix toMatrix(const QJsonValue &value);
std::vector<CMatrix> toMatrices(const QJsonValue &value);
std::vector<double> toDoubles(const QJsonValue &value);
std::vector<Complex> toComplexes(const QJsonValue &value);

// Largest |a - b| over all elements, relative to max(|b|, floor) per element.
double maxDifference(const CMatrix &a, const CMatrix &b, double floor = 1.0);
double relativeDifference(double a, double b);

} // namespace lvd::rf::test

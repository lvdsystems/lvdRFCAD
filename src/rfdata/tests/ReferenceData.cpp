#include "ReferenceData.h"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

namespace lvd::rf::test {

QString dataPath(const QString &name)
{
    return QStringLiteral(LVD_RFDATA_TEST_DATA "/") + name;
}

QJsonObject loadJson(const QString &name)
{
    QFile file(dataPath(name));
    if (!file.open(QIODevice::ReadOnly)) {
        FAIL("cannot open reference data " << name.toStdString());
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        FAIL("invalid JSON in " << name.toStdString() << ": " << parseError.errorString().toStdString());
        return {};
    }
    return document.object();
}

Complex toComplex(const QJsonValue &value)
{
    if (value.isDouble())
        return value.toDouble();
    const QJsonArray pair = value.toArray();
    return {pair.at(0).toDouble(), pair.at(1).toDouble()};
}

CMatrix toMatrix(const QJsonValue &value)
{
    const QJsonArray rows = value.toArray();
    const auto n = static_cast<Eigen::Index>(rows.size());
    const auto m = n > 0 ? static_cast<Eigen::Index>(rows.at(0).toArray().size()) : 0;
    CMatrix result(n, m);
    for (Eigen::Index i = 0; i < n; ++i) {
        const QJsonArray row = rows.at(i).toArray();
        for (Eigen::Index j = 0; j < m; ++j)
            result(i, j) = toComplex(row.at(j));
    }
    return result;
}

std::vector<CMatrix> toMatrices(const QJsonValue &value)
{
    std::vector<CMatrix> result;
    for (const QJsonValue &item : value.toArray())
        result.push_back(toMatrix(item));
    return result;
}

std::vector<double> toDoubles(const QJsonValue &value)
{
    std::vector<double> result;
    for (const QJsonValue &item : value.toArray())
        result.push_back(item.toDouble());
    return result;
}

std::vector<Complex> toComplexes(const QJsonValue &value)
{
    std::vector<Complex> result;
    for (const QJsonValue &item : value.toArray())
        result.push_back(toComplex(item));
    return result;
}

double maxDifference(const CMatrix &a, const CMatrix &b, double floor)
{
    if (a.rows() != b.rows() || a.cols() != b.cols())
        return INFINITY;
    double worst = 0;
    for (Eigen::Index i = 0; i < a.rows(); ++i)
        for (Eigen::Index j = 0; j < a.cols(); ++j)
            worst = std::max(worst, std::abs(a(i, j) - b(i, j)) / std::max(std::abs(b(i, j)), floor));
    return worst;
}

double relativeDifference(double a, double b)
{
    if (a == b)
        return 0;
    return std::abs(a - b) / std::max(std::abs(a), std::abs(b));
}

} // namespace lvd::rf::test

#pragma once

#include "rfdata/Touchstone.h"
#include "rfsim/Measurement.h"

#include <QAbstractTableModel>
#include <QStringList>

#include <memory>
#include <vector>

namespace lvd::sim {

// Frequency against a list of measurements, for the table view, the
// clipboard and CSV export. Complex measurements give two columns (real and
// imaginary part) in text exports and a "magnitude / angle" cell in the view.
class ResultsTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit ResultsTableModel(QObject *parent = nullptr);

    void setResult(std::shared_ptr<const rf::LinearResult> result);
    void setExpressions(const QStringList &expressions);
    QStringList expressions() const { return m_expressions; }
    // Evaluation errors, one "expression: message" per failed column.
    QStringList errors() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // Text of the table with full precision: a header line, then one line per
    // frequency (or only the given rows, when not empty), frequencies in Hz.
    QString text(QChar separator, const QList<int> &rows = {}) const;

private:
    void evaluate();

    std::shared_ptr<const rf::LinearResult> m_result;
    QStringList m_expressions;
    std::vector<rf::Measurement> m_columns;
};

// CSV of measurements on a result (comma separated, frequencies in Hz).
QString resultsCsv(const rf::LinearResult &result, const QStringList &expressions);

// Writes the result's S-parameters, with two-port noise parameters when the
// result has noise data. Version 2 is used when the ports have different
// reference impedances and the options ask for version 1.
bool exportTouchstone(const QString &path, const rf::LinearResult &result, rf::TouchstoneWriteOptions options,
                      QString *error = nullptr);
// The network as written by exportTouchstone().
rf::NetworkData touchstoneNetwork(const rf::LinearResult &result);

} // namespace lvd::sim

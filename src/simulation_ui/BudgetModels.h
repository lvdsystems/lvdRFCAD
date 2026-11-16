#pragma once

#include "rfsim/Budget.h"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>

#include <memory>
#include <vector>

namespace lvd::sim {

// The budget cascade as a table (RF-6): one column for the chain input and
// one per stage, one row per quantity. Display text is rounded; EditRole
// gives the value as a double (NaN when the cell is empty, +inf when the
// quantity does not apply), the text exports use full precision. Cells of
// compressed stages or with negative headroom are highlighted; stage notes
// are tooltips.
class BudgetTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum class Row {
        Component,
        Type,
        Frequency,
        StageGain,
        StageNf,
        StageOip3,
        StageOp1db,
        InputReturnLoss,
        CumulativeGain,
        CumulativeNf,
        Ip3,      // IIP3 / OIP3, cumulative
        P1db,     // IP1dB / OP1dB, cumulative
        Iip2,
        Signal,
        Noise,
        Snr,
        Sfdr,
        Headroom,
        AdcHeadroom,
        AdcSnr,
        AdcAlias,
    };

    explicit BudgetTableModel(QObject *parent = nullptr);

    // The run keeps the budget alive; null clears the table.
    void setBudget(std::shared_ptr<const rf::BudgetRun> run, const rf::BudgetResult *budget);
    const rf::BudgetResult *budget() const { return m_budget; }
    void setDarkTheme(bool dark);

    // Rows shown (the ADC rows only when the chain ends in an ADC).
    QList<Row> rows() const { return m_rows; }
    int rowOf(Row row) const { return static_cast<int>(m_rows.indexOf(row)); }
    static QString rowName(Row row);
    static QString rowUnit(Row row);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // Header line and one line per quantity (or only the given rows, when
    // not empty); for the clipboard (tab) and CSV (comma).
    QString text(QChar separator, const QList<int> &rows = {}) const;

private:
    // Value of a cell (column 0 = input) and, for paired rows, the second value.
    double value(Row row, int column, double *second = nullptr) const;
    QString cellText(Row row, int column, bool exact) const;
    bool isBad(Row row, int column) const;

    std::shared_ptr<const rf::BudgetRun> m_run;
    const rf::BudgetResult *m_budget = nullptr;
    QList<Row> m_rows;
    bool m_dark = false;
};

// The spurs of a run (RF-8): kind, mixer, m x n, frequencies, levels and
// flags. Qt::UserRole gives a sort key (numbers for numeric columns).
class SpurTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Kind, Mixer, Order, Frequency, InputFrequency, Level, Dbc, Rejection, InBand, Estimated, Note, ColumnCount };

    explicit SpurTableModel(QObject *parent = nullptr);

    void setSpurs(std::shared_ptr<const rf::BudgetRun> run, const rf::SpurResult *spurs);
    const rf::SpurResult *spurs() const { return m_spurs; }
    void setDarkTheme(bool dark);
    const rf::Spur *spur(int row) const;
    static QString kindName(rf::Spur::Kind kind);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    QString text(QChar separator, const QList<int> &rows = {}) const;

private:
    std::shared_ptr<const rf::BudgetRun> m_run;
    const rf::SpurResult *m_spurs = nullptr;
    bool m_dark = false;
};

// Sorts spurs by the UserRole keys and optionally keeps only in-band ones.
class SpurFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit SpurFilterModel(QObject *parent = nullptr);
    void setInBandOnly(bool inBandOnly);
    bool inBandOnly() const { return m_inBandOnly; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    bool m_inBandOnly = false;
};

} // namespace lvd::sim

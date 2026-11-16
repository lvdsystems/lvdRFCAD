#include "simulation_ui/ResultsTable.h"

#include "core/Expression.h"
#include "rfdata/NetworkOps.h"

#include <cmath>

namespace lvd::sim {

namespace {

QString number(double value)
{
    return QString::number(value, 'g', 12);
}

QString headerWithUnit(const QString &name, const QString &unit)
{
    return unit.isEmpty() ? name : QStringLiteral("%1 [%2]").arg(name, unit);
}

} // namespace

ResultsTableModel::ResultsTableModel(QObject *parent) : QAbstractTableModel(parent) {}

void ResultsTableModel::setResult(std::shared_ptr<const rf::LinearResult> result)
{
    beginResetModel();
    m_result = std::move(result);
    evaluate();
    endResetModel();
}

void ResultsTableModel::setExpressions(const QStringList &expressions)
{
    if (expressions == m_expressions)
        return;
    beginResetModel();
    m_expressions = expressions;
    evaluate();
    endResetModel();
}

QStringList ResultsTableModel::errors() const
{
    QStringList list;
    for (size_t i = 0; i < m_columns.size(); ++i) {
        if (!m_columns[i].ok())
            list.append(QStringLiteral("%1: %2").arg(m_expressions.at(static_cast<qsizetype>(i)), m_columns[i].error));
    }
    return list;
}

void ResultsTableModel::evaluate()
{
    m_columns.clear();
    if (!m_result || !m_result->ok())
        return;
    for (const QString &expression : m_expressions)
        m_columns.push_back(rf::evaluateMeasurement(*m_result, expression));
}

int ResultsTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_result || !m_result->ok())
        return 0;
    return m_result->network.frequencyCount();
}

int ResultsTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : 1 + static_cast<int>(m_expressions.size());
}

QVariant ResultsTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !m_result)
        return {};
    const auto row = static_cast<size_t>(index.row());
    if (role == Qt::TextAlignmentRole)
        return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
    if (index.column() == 0) {
        if (role == Qt::DisplayRole)
            return formatEngineering(m_result->network.frequencies[row], QStringLiteral("Hz"), 7);
        if (role == Qt::EditRole)
            return m_result->network.frequencies[row];
        return {};
    }
    const auto column = static_cast<size_t>(index.column() - 1);
    if (column >= m_columns.size())
        return {};
    const rf::Measurement &m = m_columns[column];
    if (!m.ok()) {
        if (role == Qt::DisplayRole && row == 0)
            return m.error;
        if (role == Qt::ToolTipRole)
            return m.error;
        return {};
    }
    if (row >= m.values.size())
        return {};
    const rf::Complex v = m.values[row];
    if (role == Qt::DisplayRole) {
        if (m.isComplex)
            return QStringLiteral("%1 ∠ %2°").arg(QString::number(std::abs(v), 'g', 5),
                                                          QString::number(std::arg(v) * 180 / rf::Pi, 'f', 2));
        return QString::number(v.real(), 'g', 6);
    }
    if (role == Qt::EditRole)
        return v.real();
    return {};
}

QVariant ResultsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);
    if (section == 0)
        return tr("Frequency");
    const auto column = static_cast<size_t>(section - 1);
    const QString expression = m_expressions.value(section - 1);
    if (column < m_columns.size() && m_columns[column].ok())
        return headerWithUnit(expression, m_columns[column].unit);
    return expression;
}

QString ResultsTableModel::text(QChar separator, const QList<int> &rows) const
{
    if (!m_result || !m_result->ok())
        return {};
    QStringList header{QStringLiteral("Frequency [Hz]")};
    for (size_t c = 0; c < m_columns.size(); ++c) {
        const rf::Measurement &m = m_columns[c];
        const QString expression = m_expressions.at(static_cast<qsizetype>(c));
        if (!m.ok())
            continue;
        if (m.isComplex) {
            header << headerWithUnit(QStringLiteral("re(%1)").arg(expression), m.unit)
                   << headerWithUnit(QStringLiteral("im(%1)").arg(expression), m.unit);
        } else {
            header << headerWithUnit(expression, m.unit);
        }
    }
    const auto quote = [separator](QString field) {
        if (field.contains(separator) || field.contains(QLatin1Char('"'))) {
            field.replace(QLatin1String("\""), QLatin1String("\"\""));
            return QStringLiteral("\"%1\"").arg(field);
        }
        return field;
    };
    QStringList lines;
    QStringList quoted;
    for (const QString &field : header)
        quoted << quote(field);
    lines << quoted.join(separator);

    QList<int> order = rows;
    if (order.isEmpty()) {
        for (int i = 0; i < m_result->network.frequencyCount(); ++i)
            order.append(i);
    }
    for (int row : order) {
        if (row < 0 || row >= m_result->network.frequencyCount())
            continue;
        const auto r = static_cast<size_t>(row);
        QStringList fields{number(m_result->network.frequencies[r])};
        for (const rf::Measurement &m : m_columns) {
            if (!m.ok())
                continue;
            const rf::Complex v = r < m.values.size() ? m.values[r] : rf::Complex(NAN, NAN);
            fields << number(v.real());
            if (m.isComplex)
                fields << number(v.imag());
        }
        lines << fields.join(separator);
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString resultsCsv(const rf::LinearResult &result, const QStringList &expressions)
{
    ResultsTableModel model;
    model.setExpressions(expressions);
    model.setResult(std::make_shared<const rf::LinearResult>(result));
    return model.text(QLatin1Char(','));
}

rf::NetworkData touchstoneNetwork(const rf::LinearResult &result)
{
    rf::NetworkData network = result.network;
    network.noiseCorrelation.clear();
    network.noise.clear();
    network.noiseFrequencies.clear();
    if (network.portCount() == 2 && result.network.hasNoiseCorrelation()
        && std::abs(network.z0[0] - network.z0[1]) < 1e-12) {
        std::vector<rf::NoiseParameters> noise;
        bool finite = true;
        for (int i = 0; i < network.frequencyCount() && finite; ++i) {
            const auto k = static_cast<size_t>(i);
            const rf::NoiseParameters p
                = rf::noiseParametersFromCorrelation(network.s[k], result.network.noiseCorrelation[k], network.z0[0]);
            finite = std::isfinite(p.fminDb) && std::isfinite(p.rn) && std::isfinite(std::abs(p.gammaOpt));
            noise.push_back(p);
        }
        if (finite) {
            network.noise = std::move(noise);
            network.noiseFrequencies = network.frequencies;
        }
    }
    if (network.comment.isEmpty())
        network.comment = QStringLiteral("lvd RF cad linear simulation");
    return network;
}

bool exportTouchstone(const QString &path, const rf::LinearResult &result, rf::TouchstoneWriteOptions options,
                      QString *error)
{
    if (!result.ok() || result.network.frequencies.empty()) {
        if (error)
            *error = QStringLiteral("there is no result to export");
        return false;
    }
    const rf::NetworkData network = touchstoneNetwork(result);
    for (double z0 : network.z0) {
        if (std::abs(z0 - network.z0.front()) > 1e-12)
            options.version = 2;
    }
    return rf::writeTouchstone(path, network, options, error);
}

} // namespace lvd::sim

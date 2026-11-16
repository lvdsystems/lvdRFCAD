#include "sparam_ui/SParamTableModel.h"

#include "rfdata/NetworkMath.h"
#include "sparam_ui/SParamFormat.h"

#include <QLocale>

#include <cmath>

namespace lvd {

namespace {

double degrees(double radians)
{
    return radians * 180.0 / rf::Pi;
}

std::optional<double> toNumber(const QVariant &value)
{
    bool ok = false;
    const QString text = value.toString().trimmed();
    double number = text.toDouble(&ok);
    if (!ok)
        number = QLocale().toDouble(text, &ok);
    if (!ok || !std::isfinite(number))
        return std::nullopt;
    return number;
}

QString csvField(const QString &text)
{
    if (text.contains(QLatin1Char(',')) || text.contains(QLatin1Char('"')))
        return QLatin1Char('"') + QString(text).replace(QLatin1String("\""), QLatin1String("\"\"")) + QLatin1Char('"');
    return text;
}

} // namespace

SParamTableModel::SParamTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void SParamTableModel::setNetwork(const rf::NetworkData *network)
{
    beginResetModel();
    m_network = network;
    m_cache.clear();
    m_cached.clear();
    updateUnit();
    endResetModel();
}

void SParamTableModel::setParameters(const std::vector<std::pair<int, int>> &parameters)
{
    if (parameters == m_parameters)
        return;
    beginResetModel();
    m_parameters = parameters;
    endResetModel();
}

void SParamTableModel::setRepresentation(SParamRepresentation representation)
{
    if (representation == m_representation)
        return;
    beginResetModel();
    m_representation = representation;
    m_cache.clear();
    m_cached.clear();
    endResetModel();
}

void SParamTableModel::setForm(SParamTableForm form)
{
    if (form == m_form)
        return;
    beginResetModel();
    m_form = form;
    endResetModel();
}

void SParamTableModel::networkReset()
{
    beginResetModel();
    m_cache.clear();
    m_cached.clear();
    updateUnit();
    endResetModel();
}

void SParamTableModel::frequencyChanged(int frequencyIndex)
{
    if (frequencyIndex < 0 || frequencyIndex >= rowCount())
        return;
    if (static_cast<std::size_t>(frequencyIndex) < m_cached.size())
        m_cached[static_cast<std::size_t>(frequencyIndex)] = 0;
    Q_EMIT dataChanged(index(frequencyIndex, 0), index(frequencyIndex, columnCount() - 1));
}

void SParamTableModel::updateUnit()
{
    m_unitScale = 1.0;
    m_unit = QStringLiteral("Hz");
    if (m_network && !m_network->frequencies.empty())
        m_unit = sparam::frequencyUnitFor(m_network->frequencies.back(), &m_unitScale);
}

bool SParamTableModel::isAvailable(SParamRepresentation representation, int portCount)
{
    if (representation == SParamRepresentation::Abcd || representation == SParamRepresentation::T)
        return portCount == 2;
    return portCount > 0;
}

QString SParamTableModel::elementName(int row, int column) const
{
    const int ports = m_network ? m_network->portCount() : 0;
    switch (m_representation) {
    case SParamRepresentation::S:
        return sparam::parameterName(QStringLiteral("S"), row, column, ports);
    case SParamRepresentation::Z:
        return sparam::parameterName(QStringLiteral("Z"), row, column, ports);
    case SParamRepresentation::Y:
        return sparam::parameterName(QStringLiteral("Y"), row, column, ports);
    case SParamRepresentation::T:
        return sparam::parameterName(QStringLiteral("T"), row, column, ports);
    case SParamRepresentation::Abcd: {
        static const char *names[2][2] = {{"A", "B"}, {"C", "D"}};
        if (row >= 0 && row < 2 && column >= 0 && column < 2)
            return QString::fromLatin1(names[row][column]);
        return QString();
    }
    }
    return QString();
}

QString SParamTableModel::elementUnit(int row, int column) const
{
    switch (m_representation) {
    case SParamRepresentation::Z:
        return QStringLiteral("Ω");
    case SParamRepresentation::Y:
        return QStringLiteral("S");
    case SParamRepresentation::Abcd:
        if (row == 0 && column == 1)
            return QStringLiteral("Ω");
        if (row == 1 && column == 0)
            return QStringLiteral("S");
        return QString();
    default:
        return QString();
    }
}

int SParamTableModel::parameterIndexOfColumn(int column, int *part) const
{
    if (column <= 0)
        return -1;
    if (part)
        *part = (column - 1) % 2;
    return (column - 1) / 2;
}

const std::optional<rf::CMatrix> &SParamTableModel::matrix(int frequencyIndex) const
{
    const auto n = static_cast<std::size_t>(m_network->frequencyCount());
    if (m_cache.size() != n) {
        m_cache.assign(n, std::nullopt);
        m_cached.assign(n, 0);
    }
    const auto k = static_cast<std::size_t>(frequencyIndex);
    if (!m_cached[k]) {
        const rf::CMatrix &s = m_network->s[k];
        const std::vector<double> &z0 = m_network->z0;
        switch (m_representation) {
        case SParamRepresentation::S:
            m_cache[k] = s;
            break;
        case SParamRepresentation::Z:
            m_cache[k] = rf::sToZ(s, z0);
            break;
        case SParamRepresentation::Y:
            m_cache[k] = rf::sToY(s, z0);
            break;
        case SParamRepresentation::Abcd:
            m_cache[k] = z0.size() == 2 ? rf::sToAbcd(s, z0[0], z0[1]) : std::nullopt;
            break;
        case SParamRepresentation::T:
            m_cache[k] = z0.size() == 2 ? rf::sToT(s) : std::nullopt;
            break;
        }
        m_cached[k] = 1;
    }
    return m_cache[k];
}

std::optional<rf::Complex> SParamTableModel::element(int frequencyIndex, int row, int column) const
{
    if (!m_network || frequencyIndex < 0 || frequencyIndex >= m_network->frequencyCount())
        return std::nullopt;
    const int ports = m_network->portCount();
    if (row < 0 || column < 0 || row >= ports || column >= ports)
        return std::nullopt;
    if (m_representation == SParamRepresentation::S)
        return m_network->s[static_cast<std::size_t>(frequencyIndex)](row, column);
    const std::optional<rf::CMatrix> &m = matrix(frequencyIndex);
    if (!m || row >= m->rows() || column >= m->cols())
        return std::nullopt;
    const rf::Complex value = (*m)(row, column);
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
        return std::nullopt;
    return value;
}

double SParamTableModel::partValue(rf::Complex value, int part) const
{
    switch (m_form) {
    case SParamTableForm::RealImaginary:
        return part == 0 ? value.real() : value.imag();
    case SParamTableForm::MagnitudeAngle:
        return part == 0 ? std::abs(value) : degrees(std::arg(value));
    case SParamTableForm::DecibelAngle:
        return part == 0 ? rf::db20(value) : degrees(std::arg(value));
    }
    return 0;
}

QString SParamTableModel::columnTitle(int column, bool withUnit) const
{
    if (column == 0)
        return withUnit ? tr("Frequency (%1)").arg(m_unit) : tr("Frequency");
    int part = 0;
    const int p = parameterIndexOfColumn(column, &part);
    if (p < 0 || p >= static_cast<int>(m_parameters.size()))
        return QString();
    const auto [row, col] = m_parameters[static_cast<std::size_t>(p)];
    const QString name = elementName(row, col);
    const QString unit = elementUnit(row, col);
    const QString unitSuffix = withUnit && !unit.isEmpty() ? QStringLiteral(" (%1)").arg(unit) : QString();
    switch (m_form) {
    case SParamTableForm::RealImaginary:
        return (part == 0 ? tr("Re %1") : tr("Im %1")).arg(name) + unitSuffix;
    case SParamTableForm::MagnitudeAngle:
        return part == 0 ? QStringLiteral("|%1|").arg(name) + unitSuffix
                         : (withUnit ? tr("∠%1 (°)") : tr("∠%1")).arg(name);
    case SParamTableForm::DecibelAngle:
        return part == 0 ? (withUnit ? tr("%1 (dB)") : tr("%1 dB")).arg(name)
                         : (withUnit ? tr("∠%1 (°)") : tr("∠%1")).arg(name);
    }
    return QString();
}

int SParamTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_network)
        return 0;
    return m_network->frequencyCount();
}

int SParamTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_network)
        return 0;
    return 1 + 2 * static_cast<int>(m_parameters.size());
}

QVariant SParamTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !m_network)
        return {};
    if (role != Qt::DisplayRole && role != Qt::EditRole && role != Qt::UserRole && role != Qt::TextAlignmentRole)
        return {};
    if (role == Qt::TextAlignmentRole)
        return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
    const int k = index.row();
    if (index.column() == 0) {
        const double f = m_network->frequencies[static_cast<std::size_t>(k)];
        if (role == Qt::UserRole)
            return f;
        return QLocale().toString(f / m_unitScale, 'g', 10);
    }
    int part = 0;
    const int p = parameterIndexOfColumn(index.column(), &part);
    if (p < 0 || p >= static_cast<int>(m_parameters.size()))
        return {};
    const auto [row, col] = m_parameters[static_cast<std::size_t>(p)];
    const std::optional<rf::Complex> value = element(k, row, col);
    if (!value)
        return role == Qt::UserRole ? QVariant() : QVariant(QStringLiteral("—"));
    const double number = partValue(*value, part);
    if (role == Qt::UserRole)
        return number;
    if (role == Qt::EditRole)
        return QString::number(number, 'g', 12);
    return QLocale().toString(number, 'g', 7);
}

bool SParamTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || !m_network || index.column() == 0
        || m_representation != SParamRepresentation::S || !m_editHandler)
        return false;
    int part = 0;
    const int p = parameterIndexOfColumn(index.column(), &part);
    if (p < 0 || p >= static_cast<int>(m_parameters.size()))
        return false;
    const std::optional<double> number = toNumber(value);
    if (!number)
        return false;
    const auto [row, col] = m_parameters[static_cast<std::size_t>(p)];
    const rf::Complex old = m_network->s[static_cast<std::size_t>(index.row())](row, col);
    rf::Complex updated = old;
    switch (m_form) {
    case SParamTableForm::RealImaginary:
        updated = part == 0 ? rf::Complex(*number, old.imag()) : rf::Complex(old.real(), *number);
        break;
    case SParamTableForm::MagnitudeAngle:
        updated = part == 0 ? std::polar(*number, std::arg(old)) : std::polar(std::abs(old), *number * rf::Pi / 180.0);
        break;
    case SParamTableForm::DecibelAngle:
        updated = part == 0 ? rf::fromDbPhase(*number, degrees(std::arg(old)))
                            : std::polar(std::abs(old), *number * rf::Pi / 180.0);
        break;
    }
    if (updated == old)
        return true;
    return m_editHandler(index.row(), row, col, updated);
}

QVariant SParamTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return {};
    if (orientation == Qt::Vertical)
        return section + 1;
    return columnTitle(section, true);
}

Qt::ItemFlags SParamTableModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && index.column() > 0 && m_representation == SParamRepresentation::S)
        f |= Qt::ItemIsEditable;
    return f;
}

QString SParamTableModel::csvText() const
{
    QString text;
    if (!m_network)
        return text;
    const int columns = columnCount();
    QStringList header;
    header << tr("Frequency (Hz)");
    for (int c = 1; c < columns; ++c)
        header << csvField(columnTitle(c, true));
    text += header.join(QLatin1Char(',')) + QLatin1Char('\n');
    const int rows = rowCount();
    for (int k = 0; k < rows; ++k) {
        QStringList fields;
        fields.reserve(columns);
        fields << QString::number(m_network->frequencies[static_cast<std::size_t>(k)], 'g', 15);
        for (int c = 1; c < columns; ++c) {
            int part = 0;
            const int p = parameterIndexOfColumn(c, &part);
            const auto [row, col] = m_parameters[static_cast<std::size_t>(p)];
            const std::optional<rf::Complex> value = element(k, row, col);
            fields << (value ? QString::number(partValue(*value, part), 'g', 12) : QString());
        }
        text += fields.join(QLatin1Char(',')) + QLatin1Char('\n');
    }
    return text;
}

} // namespace lvd

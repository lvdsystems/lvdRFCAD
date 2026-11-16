#include "simulation_ui/BudgetModels.h"

#include "core/Expression.h"

#include <QBrush>
#include <QColor>
#include <QFont>

#include <cmath>

namespace lvd::sim {

namespace {

constexpr QChar Infinity(0x221E);

QString decibels(double value)
{
    if (std::isnan(value))
        return {};
    if (std::isinf(value))
        return value > 0 ? QString(Infinity) : QStringLiteral("−") + Infinity;
    return QString::number(value, 'f', 2);
}

QString hertz(double value)
{
    if (!std::isfinite(value))
        return {};
    return formatEngineering(value, QStringLiteral("Hz"), 6);
}

QString exactNumber(double value)
{
    if (std::isnan(value))
        return {};
    if (std::isinf(value))
        return value > 0 ? QStringLiteral("inf") : QStringLiteral("-inf");
    return QString::number(value, 'g', 12);
}

QString quoted(QString field, QChar separator)
{
    if (field.contains(separator) || field.contains(QLatin1Char('"')) || field.contains(QLatin1Char('\n'))) {
        field.replace(QLatin1String("\""), QLatin1String("\"\""));
        return QStringLiteral("\"%1\"").arg(field);
    }
    return field;
}

QString withUnit(const QString &name, const QString &unit)
{
    return unit.isEmpty() ? name : QStringLiteral("%1 [%2]").arg(name, unit);
}

QColor badBackground(bool dark)
{
    return dark ? QColor(0x5E, 0x26, 0x26) : QColor(0xFB, 0xD3, 0xD3);
}

QColor badForeground(bool dark)
{
    return dark ? QColor(0xFF, 0xD6, 0xD6) : QColor(0x8A, 0x10, 0x10);
}

} // namespace

// ---------------------------------------------------------------- budget table

BudgetTableModel::BudgetTableModel(QObject *parent) : QAbstractTableModel(parent) {}

void BudgetTableModel::setBudget(std::shared_ptr<const rf::BudgetRun> run, const rf::BudgetResult *budget)
{
    beginResetModel();
    m_run = std::move(run);
    m_budget = m_run ? budget : nullptr;
    m_rows.clear();
    if (m_budget && !m_budget->stages.empty()) {
        for (int r = static_cast<int>(Row::Component); r <= static_cast<int>(Row::Headroom); ++r)
            m_rows.append(static_cast<Row>(r));
        if (m_budget->hasAdc)
            m_rows << Row::AdcHeadroom << Row::AdcSnr << Row::AdcAlias;
    }
    endResetModel();
}

void BudgetTableModel::setDarkTheme(bool dark)
{
    if (dark == m_dark)
        return;
    m_dark = dark;
    if (rowCount() > 0)
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

QString BudgetTableModel::rowName(Row row)
{
    switch (row) {
    case Row::Component: return tr("Component");
    case Row::Type: return tr("Type");
    case Row::Frequency: return tr("Frequency in → out");
    case Row::StageGain: return tr("Stage gain");
    case Row::StageNf: return tr("Stage NF");
    case Row::StageOip3: return tr("Stage OIP3");
    case Row::StageOp1db: return tr("Stage OP1dB");
    case Row::InputReturnLoss: return tr("Input return loss");
    case Row::CumulativeGain: return tr("Cumulative gain");
    case Row::CumulativeNf: return tr("Cumulative NF");
    case Row::Ip3: return tr("IIP3 / OIP3");
    case Row::P1db: return tr("IP1dB / OP1dB");
    case Row::Iip2: return tr("IIP2");
    case Row::Signal: return tr("Signal level");
    case Row::Noise: return tr("Noise level");
    case Row::Snr: return tr("SNR");
    case Row::Sfdr: return tr("SFDR");
    case Row::Headroom: return tr("Compression headroom");
    case Row::AdcHeadroom: return tr("ADC full-scale headroom");
    case Row::AdcSnr: return tr("ADC quantisation SNR");
    case Row::AdcAlias: return tr("ADC alias frequency");
    }
    return {};
}

QString BudgetTableModel::rowUnit(Row row)
{
    switch (row) {
    case Row::Component:
    case Row::Type:
    case Row::Frequency:
    case Row::AdcAlias:
        return {};
    case Row::StageOip3:
    case Row::StageOp1db:
    case Row::Ip3:
    case Row::P1db:
    case Row::Iip2:
    case Row::Signal:
    case Row::Noise:
        return QStringLiteral("dBm");
    default:
        return QStringLiteral("dB");
    }
}

int BudgetTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int BudgetTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid() || !m_budget || m_budget->stages.empty())
        return 0;
    return 1 + static_cast<int>(m_budget->stages.size());
}

double BudgetTableModel::value(Row row, int column, double *second) const
{
    if (second)
        *second = NAN;
    if (!m_budget || column < 0 || column > static_cast<int>(m_budget->stages.size()))
        return NAN;
    const rf::BudgetResult &b = *m_budget;
    if (column == 0) {
        switch (row) {
        case Row::Frequency:
            if (second)
                *second = b.inputFrequency;
            return b.inputFrequency;
        case Row::Signal: return b.inputPowerDbm;
        case Row::Noise: return b.inputNoiseDbm;
        case Row::Snr: return b.inputPowerDbm - b.inputNoiseDbm;
        default: return NAN;
        }
    }
    const rf::BudgetStage &s = b.stages[static_cast<size_t>(column - 1)];
    const bool last = column == static_cast<int>(b.stages.size());
    switch (row) {
    case Row::Component:
    case Row::Type:
        return NAN;
    case Row::Frequency:
        if (second)
            *second = s.outputFrequency;
        return s.inputFrequency;
    case Row::StageGain: return s.gainDb;
    case Row::StageNf: return s.nfDb;
    case Row::StageOip3: return s.oip3Dbm;
    case Row::StageOp1db: return s.op1dbDbm;
    case Row::InputReturnLoss: return s.inputReturnLossDb;
    case Row::CumulativeGain: return s.cumulativeGainDb;
    case Row::CumulativeNf: return s.cumulativeNfDb;
    case Row::Ip3:
        if (second)
            *second = s.oip3CumulativeDbm;
        return s.iip3Dbm;
    case Row::P1db:
        if (second)
            *second = s.op1dbCumulativeDbm;
        return s.ip1dbDbm;
    case Row::Iip2: return s.iip2Dbm;
    case Row::Signal: return s.signalDbm;
    case Row::Noise: return s.noiseDbm;
    case Row::Snr: return s.snrDb;
    case Row::Sfdr: return s.sfdrDb;
    case Row::Headroom: return s.headroomDb;
    case Row::AdcHeadroom: return last && b.hasAdc ? b.adcHeadroomDb : NAN;
    case Row::AdcSnr: return last && b.hasAdc ? b.adcQuantisationSnrDb : NAN;
    case Row::AdcAlias: return last && b.hasAdc ? b.adcAliasFrequency : NAN;
    }
    return NAN;
}

QString BudgetTableModel::cellText(Row row, int column, bool exact) const
{
    if (!m_budget)
        return {};
    const rf::BudgetResult &b = *m_budget;
    if (row == Row::Component) {
        if (column == 0)
            return b.start.isEmpty() ? tr("Input") : b.start;
        return b.stages[static_cast<size_t>(column - 1)].path;
    }
    if (row == Row::Type) {
        if (column == 0)
            return m_run ? m_run->chain.startKind : QString();
        const rf::BudgetStage &s = b.stages[static_cast<size_t>(column - 1)];
        return s.kind.isEmpty() ? s.name : s.kind;
    }
    double second = NAN;
    const double first = value(row, column, &second);
    const auto number = [exact](double v) { return exact ? exactNumber(v) : decibels(v); };
    switch (row) {
    case Row::Frequency:
        if (exact)
            return std::abs(first - second) > 1e-9 * std::max(1.0, std::abs(first))
                       ? QStringLiteral("%1 -> %2").arg(exactNumber(first), exactNumber(second))
                       : exactNumber(first);
        if (std::isfinite(second) && std::abs(first - second) > 1e-9 * std::max(1.0, std::abs(first))) {
            QString text = QStringLiteral("%1 → %2").arg(hertz(first), hertz(second));
            // Mixers: which product the chain follows.
            if (m_run && column >= 1 && static_cast<size_t>(column - 1) < m_run->chain.stages.size()) {
                const QString product = rf::mixerProductName(m_run->chain.stages[static_cast<size_t>(column - 1)]);
                if (!product.isEmpty())
                    text += QStringLiteral(" (%1)").arg(product == QLatin1String("sum") ? tr("sum") : tr("difference"));
            }
            return text;
        }
        return hertz(first);
    case Row::AdcAlias:
        return exact ? exactNumber(first) : hertz(first);
    case Row::Ip3:
    case Row::P1db:
        if (std::isnan(first) && std::isnan(second))
            return {};
        return QStringLiteral("%1 / %2").arg(number(first), number(second));
    default:
        return number(first);
    }
}

bool BudgetTableModel::isBad(Row row, int column) const
{
    if (!m_budget || column < 1)
        return false;
    const rf::BudgetResult &b = *m_budget;
    const rf::BudgetStage &s = b.stages[static_cast<size_t>(column - 1)];
    switch (row) {
    case Row::Headroom:
        return s.compressed || s.headroomDb < 0;
    case Row::Signal:
        return s.compressed;
    case Row::AdcHeadroom:
        return column == static_cast<int>(b.stages.size()) && b.hasAdc && b.adcHeadroomDb < 0;
    default:
        return false;
    }
}

QVariant BudgetTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !m_budget || index.row() >= m_rows.size())
        return {};
    const Row row = m_rows.at(index.row());
    const int column = index.column();
    switch (role) {
    case Qt::DisplayRole:
        return cellText(row, column, false);
    case Qt::EditRole:
        if (row == Row::Component || row == Row::Type)
            return cellText(row, column, false);
        if (row == Row::Frequency) {
            double out = NAN;
            value(row, column, &out);
            return out;
        }
        return value(row, column);
    case Qt::TextAlignmentRole:
        if (row == Row::Component || row == Row::Type)
            return QVariant::fromValue(Qt::AlignCenter);
        return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
    case Qt::FontRole:
        if (row == Row::Component) {
            QFont font;
            font.setBold(true);
            return font;
        }
        return {};
    case Qt::BackgroundRole:
        if (isBad(row, column))
            return QBrush(badBackground(m_dark));
        return {};
    case Qt::ForegroundRole:
        if (isBad(row, column))
            return QBrush(badForeground(m_dark));
        return {};
    case Qt::ToolTipRole: {
        QStringList lines;
        if (column >= 1) {
            const rf::BudgetStage &s = m_budget->stages[static_cast<size_t>(column - 1)];
            if (isBad(row, column))
                lines.append(row == Row::AdcHeadroom ? tr("The signal exceeds the ADC full scale.")
                                                     : tr("The stage is in compression (headroom below 0 dB)."));
            lines.append(s.notes);
        }
        return lines.isEmpty() ? QVariant() : QVariant(lines.join(QLatin1Char('\n')));
    }
    default:
        return {};
    }
}

QVariant BudgetTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (!m_budget)
        return QAbstractTableModel::headerData(section, orientation, role);
    if (orientation == Qt::Vertical) {
        if (section < 0 || section >= m_rows.size())
            return {};
        const Row row = m_rows.at(section);
        if (role == Qt::DisplayRole)
            return withUnit(rowName(row), rowUnit(row));
        if (role == Qt::TextAlignmentRole)
            return QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter);
        if (role == Qt::ToolTipRole) {
            switch (row) {
            case Row::Ip3: return tr("Cumulative third-order intercept, referred to the chain input / at the stage output");
            case Row::P1db: return tr("Cumulative 1 dB compression point, referred to the chain input / at the stage output");
            case Row::Iip2: return tr("Cumulative second-order intercept, referred to the chain input");
            case Row::Sfdr: return tr("Input-referred third-order spurious-free dynamic range");
            case Row::Headroom: return tr("Cumulative OP1dB minus the signal level");
            case Row::InputReturnLoss: return tr("Return loss of the stage input alone");
            default: return {};
            }
        }
        return {};
    }
    if (section < 0 || section > static_cast<int>(m_budget->stages.size()))
        return {};
    if (section == 0) {
        if (role == Qt::DisplayRole)
            return tr("Input");
        if (role == Qt::ToolTipRole)
            return tr("Chain input at %1").arg(m_budget->start);
        return {};
    }
    const rf::BudgetStage &s = m_budget->stages[static_cast<size_t>(section - 1)];
    if (role == Qt::DisplayRole)
        return QStringLiteral("%1 · %2%3").arg(section).arg(s.path, s.notes.isEmpty() ? QString() : QStringLiteral(" *"));
    if (role == Qt::ToolTipRole) {
        QStringList lines{QStringLiteral("%1 – %2").arg(s.path, s.name)};
        lines.append(s.notes);
        return lines.join(QLatin1Char('\n'));
    }
    return {};
}

QString BudgetTableModel::text(QChar separator, const QList<int> &rows) const
{
    if (!m_budget || m_rows.isEmpty())
        return {};
    QStringList lines;
    QStringList header{quoted(tr("Quantity"), separator), quoted(tr("Input"), separator)};
    for (const rf::BudgetStage &s : m_budget->stages)
        header << quoted(s.path, separator);
    lines << header.join(separator);

    QList<int> order = rows;
    if (order.isEmpty()) {
        for (int i = 0; i < m_rows.size(); ++i)
            order.append(i);
    }
    const int columns = columnCount();
    for (int r : order) {
        if (r < 0 || r >= m_rows.size())
            continue;
        const Row row = m_rows.at(r);
        const auto line = [&](const QString &name, const QString &unit, auto cell) {
            QStringList fields{quoted(withUnit(name, unit), separator)};
            for (int c = 0; c < columns; ++c)
                fields << quoted(cell(c), separator);
            lines << fields.join(separator);
        };
        switch (row) {
        case Row::Component:
        case Row::Type:
            line(rowName(row), QString(), [&](int c) { return cellText(row, c, true); });
            break;
        case Row::Frequency:
            line(tr("Input frequency"), QStringLiteral("Hz"), [&](int c) { return exactNumber(value(row, c)); });
            line(tr("Output frequency"), QStringLiteral("Hz"), [&](int c) {
                double out = NAN;
                value(row, c, &out);
                return exactNumber(out);
            });
            break;
        case Row::Ip3:
        case Row::P1db:
            line(row == Row::Ip3 ? tr("IIP3") : tr("IP1dB"), QStringLiteral("dBm"),
                 [&](int c) { return exactNumber(value(row, c)); });
            line(row == Row::Ip3 ? tr("OIP3 (cumulative)") : tr("OP1dB (cumulative)"), QStringLiteral("dBm"), [&](int c) {
                double out = NAN;
                value(row, c, &out);
                return exactNumber(out);
            });
            break;
        case Row::AdcAlias:
            line(rowName(row), QStringLiteral("Hz"), [&](int c) { return exactNumber(value(row, c)); });
            break;
        default:
            line(rowName(row), rowUnit(row), [&](int c) { return exactNumber(value(row, c)); });
            break;
        }
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

// ---------------------------------------------------------------- spur table

SpurTableModel::SpurTableModel(QObject *parent) : QAbstractTableModel(parent) {}

void SpurTableModel::setSpurs(std::shared_ptr<const rf::BudgetRun> run, const rf::SpurResult *spurs)
{
    beginResetModel();
    m_run = std::move(run);
    m_spurs = m_run ? spurs : nullptr;
    endResetModel();
}

void SpurTableModel::setDarkTheme(bool dark)
{
    if (dark == m_dark)
        return;
    m_dark = dark;
    if (rowCount() > 0)
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
}

const rf::Spur *SpurTableModel::spur(int row) const
{
    if (!m_spurs || row < 0 || row >= static_cast<int>(m_spurs->spurs.size()))
        return nullptr;
    return &m_spurs->spurs[static_cast<size_t>(row)];
}

QString SpurTableModel::kindName(rf::Spur::Kind kind)
{
    switch (kind) {
    case rf::Spur::Kind::Product: return tr("Product");
    case rf::Spur::Kind::SpuriousResponse: return tr("Spurious response");
    case rf::Spur::Kind::Image: return tr("Image");
    case rf::Spur::Kind::HalfIf: return tr("Half-IF");
    case rf::Spur::Kind::LoLeakage: return tr("LO leakage");
    }
    return {};
}

int SpurTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() || !m_spurs ? 0 : static_cast<int>(m_spurs->spurs.size());
}

int SpurTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant SpurTableModel::data(const QModelIndex &index, int role) const
{
    const rf::Spur *s = index.isValid() ? spur(index.row()) : nullptr;
    if (!s)
        return {};
    const auto numeric = [](double v) { return std::isnan(v) ? -1e300 : v; };
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case Kind: return kindName(s->kind);
        case Mixer: return s->mixer;
        case Order: return s->kind == rf::Spur::Kind::LoLeakage && s->m == 0 && s->n == 0
                               ? QString()
                               : QStringLiteral("%1×%2").arg(s->m).arg(s->n);
        case Frequency: return hertz(s->frequency);
        case InputFrequency: return hertz(s->inputFrequency);
        case Level: return decibels(s->levelDbm);
        case Dbc: return decibels(s->levelDbc);
        case Rejection: return decibels(s->rejectionDb);
        case InBand: return s->inBand ? tr("in band") : QString();
        case Estimated: return s->estimated ? tr("estimated") : QString();
        case Note: return s->note;
        default: return {};
        }
    case Qt::UserRole:
        switch (index.column()) {
        case Kind: return static_cast<int>(s->kind);
        case Mixer: return s->mixer;
        case Order: return std::abs(s->m) + std::abs(s->n);
        case Frequency: return numeric(s->frequency);
        case InputFrequency: return numeric(s->inputFrequency);
        case Level: return numeric(s->levelDbm);
        case Dbc: return numeric(s->levelDbc);
        case Rejection: return numeric(s->rejectionDb);
        case InBand: return s->inBand ? 1 : 0;
        case Estimated: return s->estimated ? 1 : 0;
        case Note: return s->note;
        default: return {};
        }
    case Qt::EditRole:
        switch (index.column()) {
        case Frequency: return s->frequency;
        case InputFrequency: return s->inputFrequency;
        case Level: return s->levelDbm;
        case Dbc: return s->levelDbc;
        case Rejection: return s->rejectionDb;
        case InBand: return s->inBand;
        case Estimated: return s->estimated;
        default: return data(index, Qt::DisplayRole);
        }
    case Qt::TextAlignmentRole:
        if (index.column() >= Frequency && index.column() <= Rejection)
            return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
        return QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter);
    // Only the flag cells are highlighted: spurious responses are in band by
    // definition, and a fully coloured table would hide which rows matter.
    case Qt::BackgroundRole:
        if (s->inBand && (index.column() == InBand || index.column() == Kind))
            return QBrush(badBackground(m_dark));
        return {};
    case Qt::ForegroundRole:
        if (s->inBand && (index.column() == InBand || index.column() == Kind))
            return QBrush(badForeground(m_dark));
        return {};
    case Qt::FontRole:
        if (s->inBand && index.column() == InBand) {
            QFont font;
            font.setBold(true);
            return font;
        }
        return {};
    case Qt::ToolTipRole: {
        QStringList lines;
        if (s->inBand)
            lines.append(tr("Falls inside the wanted output band."));
        if (s->estimated)
            lines.append(tr("The mixer level is estimated: the mixer has no spur table."));
        if (!s->note.isEmpty())
            lines.append(s->note);
        return lines.isEmpty() ? QVariant() : QVariant(lines.join(QLatin1Char('\n')));
    }
    default:
        return {};
    }
}

QVariant SpurTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return QAbstractTableModel::headerData(section, orientation, role);
    if (role == Qt::DisplayRole) {
        switch (section) {
        case Kind: return tr("Kind");
        case Mixer: return tr("Mixer");
        case Order: return tr("m×n");
        case Frequency: return tr("Frequency");
        case InputFrequency: return tr("Input frequency");
        case Level: return tr("Level [dBm]");
        case Dbc: return tr("Level [dBc]");
        case Rejection: return tr("Rejection [dB]");
        case InBand: return tr("In band");
        case Estimated: return tr("Estimated");
        case Note: return tr("Note");
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (section) {
        case Order: return tr("RF harmonic m × LO harmonic n");
        case Frequency: return tr("At the chain output (LO leakage to the input: at the input)");
        case InputFrequency: return tr("Spurious responses: the input frequency that produces output in the wanted band");
        case Dbc: return tr("Below the wanted output (positive = weaker)");
        case Rejection: return tr("Responses: how much weaker than a wanted signal of equal input power");
        default: return {};
        }
    }
    return {};
}

QString SpurTableModel::text(QChar separator, const QList<int> &rows) const
{
    if (!m_spurs)
        return {};
    QStringList header{tr("Kind"), tr("Mixer"), QStringLiteral("m"), QStringLiteral("n"), tr("Frequency [Hz]"),
                       tr("Input frequency [Hz]"), tr("Level [dBm]"), tr("Level [dBc]"), tr("Rejection [dB]"),
                       tr("In band"), tr("Estimated"), tr("Note")};
    QStringList lines;
    QStringList quotedHeader;
    for (const QString &field : header)
        quotedHeader << quoted(field, separator);
    lines << quotedHeader.join(separator);
    QList<int> order = rows;
    if (order.isEmpty()) {
        for (int i = 0; i < rowCount(); ++i)
            order.append(i);
    }
    for (int r : order) {
        const rf::Spur *s = spur(r);
        if (!s)
            continue;
        QStringList fields{kindName(s->kind),          s->mixer,
                           QString::number(s->m),      QString::number(s->n),
                           exactNumber(s->frequency),  exactNumber(s->inputFrequency),
                           exactNumber(s->levelDbm),   exactNumber(s->levelDbc),
                           exactNumber(s->rejectionDb), s->inBand ? QStringLiteral("1") : QStringLiteral("0"),
                           s->estimated ? QStringLiteral("1") : QStringLiteral("0"), s->note};
        for (QString &field : fields)
            field = quoted(field, separator);
        lines << fields.join(separator);
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

// ---------------------------------------------------------------- spur filter

SpurFilterModel::SpurFilterModel(QObject *parent) : QSortFilterProxyModel(parent)
{
    setSortRole(Qt::UserRole);
}

void SpurFilterModel::setInBandOnly(bool inBandOnly)
{
    if (inBandOnly == m_inBandOnly)
        return;
    beginFilterChange();
    m_inBandOnly = inBandOnly;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool SpurFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (!m_inBandOnly)
        return true;
    const QModelIndex index = sourceModel()->index(sourceRow, SpurTableModel::InBand, sourceParent);
    return sourceModel()->data(index, Qt::EditRole).toBool();
}

} // namespace lvd::sim

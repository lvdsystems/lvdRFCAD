#pragma once

#include "rfdata/RfTypes.h"

#include <QAbstractTableModel>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace lvd {

// Network representation shown in the table.
enum class SParamRepresentation { S, Z, Y, Abcd, T };
// Number pair shown per parameter.
enum class SParamTableForm { RealImaginary, MagnitudeAngle, DecibelAngle };

// Table of a network: one row per frequency, two columns (in the chosen form)
// per selected parameter, in S, Z, Y, ABCD or T. S cells are editable; an
// edit is handed to the edit handler (the editor turns it into an undoable
// command). Qt::UserRole gives the unrounded number.
class SParamTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    using EditHandler = std::function<bool(int frequencyIndex, int row, int column, rf::Complex value)>;

    explicit SParamTableModel(QObject *parent = nullptr);

    // The network must outlive the model or be replaced first.
    void setNetwork(const rf::NetworkData *network);
    void setParameters(const std::vector<std::pair<int, int>> &parameters);
    void setRepresentation(SParamRepresentation representation);
    SParamRepresentation representation() const { return m_representation; }
    void setForm(SParamTableForm form);
    SParamTableForm form() const { return m_form; }
    void setEditHandler(EditHandler handler) { m_editHandler = std::move(handler); }

    // The whole network changed (same layout or not).
    void networkReset();
    // The values of one frequency changed.
    void frequencyChanged(int frequencyIndex);

    // Whether the representation exists for the network (ABCD and T: two-ports).
    static bool isAvailable(SParamRepresentation representation, int portCount);
    // Element name such as "S21", "Z11", "B", "T12".
    QString elementName(int row, int column) const;
    // Parameter shown in a column (-1 for the frequency column) and which of the pair.
    int parameterIndexOfColumn(int column, int *part = nullptr) const;
    // Value of an element at a frequency in the current representation;
    // nullopt when it does not exist (singular conversion).
    std::optional<rf::Complex> element(int frequencyIndex, int row, int column) const;

    QString frequencyUnit() const { return m_unit; }
    // The whole table as CSV (frequencies in Hz, full precision).
    QString csvText() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

private:
    const std::optional<rf::CMatrix> &matrix(int frequencyIndex) const;
    QString columnTitle(int column, bool withUnit) const;
    QString elementUnit(int row, int column) const;
    double partValue(rf::Complex value, int part) const;
    void updateUnit();

    const rf::NetworkData *m_network = nullptr;
    std::vector<std::pair<int, int>> m_parameters;
    SParamRepresentation m_representation = SParamRepresentation::S;
    SParamTableForm m_form = SParamTableForm::DecibelAngle;
    EditHandler m_editHandler;
    QString m_unit = QStringLiteral("Hz");
    double m_unitScale = 1.0;

    // Converted matrices, computed when first shown.
    mutable std::vector<std::optional<rf::CMatrix>> m_cache;
    mutable std::vector<char> m_cached;
};

} // namespace lvd

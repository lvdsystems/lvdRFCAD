#pragma once

#include "antenna/PatternEngine.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonObject>
#include <QSpinBox>
#include <QString>
#include <QUndoCommand>

#include <functional>
#include <vector>

class QFormLayout;
class QLabel;
class QUndoStack;

namespace lvd {
class PlotWidget;
class Pattern3DView;
}

namespace lvd::antui {

// Form widgets that do not react to the mouse wheel unless focused, so a
// long settings panel scrolls safely.
QDoubleSpinBox *doubleSpin(double min, double max, int decimals, const QString &suffix = {}, double step = 1.0);
QSpinBox *intSpin(int min, int max, const QString &suffix = {});
// A combo box of (id, label) pairs; the id is the item data.
QComboBox *idCombo(const std::vector<std::pair<QString, QString>> &items);
void setComboId(QComboBox *combo, const QString &id);
QString comboId(const QComboBox *combo);
QLabel *noteLabel(const QString &text = {});

// Undo step holding the document content before and after an edit; edits
// with the same merge key that follow each other become one step.
class ContentCommand : public QUndoCommand
{
public:
    ContentCommand(const QString &text, QJsonObject before, QJsonObject after, int mergeKey,
                   std::function<void(const QJsonObject &)> apply);
    void undo() override;
    void redo() override;
    int id() const override { return m_mergeKey; }
    bool mergeWith(const QUndoCommand *other) override;

private:
    QJsonObject m_before;
    QJsonObject m_after;
    int m_mergeKey;
    std::function<void(const QJsonObject &)> m_apply;
    bool m_first = true;
};

// Pattern values for plots (dB), one component.
std::vector<double> cutValuesDb(const ant::PatternCut &cut, ant::PatternComponent component, ant::CoPolarisation co);
// Fills a rectangular or polar plot with cuts; returns the largest value.
struct CutTrace {
    const ant::PatternCut *cut;
    QString name;
    bool dashed = false;
};
double plotCuts(PlotWidget *plot, const std::vector<CutTrace> &cuts, ant::PatternComponent component, ant::CoPolarisation co,
                bool polar, double dynamicRangeDb, const QString &unit, bool compass = true);
// 3D view of a spherical grid.
void show3d(Pattern3DView *view, const ant::SphericalGrid &grid, ant::PatternComponent component, ant::CoPolarisation co,
            double dynamicRangeDb, const QString &label, const QString &unit);

QString formatDb(double value, const QString &unit, int decimals = 2);
QString formatFrequency(double hz);

} // namespace lvd::antui

#pragma once

#include "spice/SpiceAnalysis.h"

#include <QDialog>
#include <QHash>

#include <functional>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStackedWidget;

namespace lvd::spiceui {

// Edits one SPICE analysis (requirement SP-2): its type (for a new analysis),
// name and the fields of its type, with the temperature, .options and extra
// cards common to all. Net and source fields offer the schematic's nets and
// sources. On OK the validator (when set) checks the setup against the
// schematic; its messages keep the dialog open.
class SpiceAnalysisDialog : public QDialog
{
    Q_OBJECT
public:
    // isNew: the type can be chosen.
    SpiceAnalysisDialog(const spice::SpiceAnalysisSetup &setup, bool isNew, QWidget *parent = nullptr);

    void setNets(const QStringList &nets);
    void setSources(const QStringList &references);
    using Validator = std::function<QStringList(const spice::SpiceAnalysisSetup &)>;
    void setValidator(Validator validator) { m_validator = std::move(validator); }

    spice::SpiceAnalysisSetup setup() const;

    // For tests: the editor of a field ("stop", "source", ...) of the shown type.
    QWidget *fieldEditor(const QString &key) const;
    void setKind(spice::AnalysisKind kind);
    spice::AnalysisKind kind() const;
    QString errorText() const;

    void accept() override;

private:
    struct Field {
        QString key;
        QWidget *editor = nullptr;
        enum class Type { Text, Net, Source, Choice, Flag, List } type = Type::Text;
    };
    QWidget *buildPage(spice::AnalysisKind kind);
    void addField(QFormLayout *form, spice::AnalysisKind kind, const QString &key, const QString &label, Field::Type type,
                  const QStringList &choices = {}, const QString &toolTip = {});
    void loadFields(const spice::SpiceAnalysisSetup &setup);
    void refreshCombos();

    spice::SpiceAnalysisSetup m_original;
    QLineEdit *m_name;
    QComboBox *m_kind;
    QStackedWidget *m_pages;
    QHash<spice::AnalysisKind, QWidget *> m_pageOf;
    QHash<spice::AnalysisKind, QList<Field>> m_fields;
    QLineEdit *m_temperature;
    QLineEdit *m_options;
    QPlainTextEdit *m_cards;
    QLabel *m_error;
    QStringList m_nets;
    QStringList m_sources;
    Validator m_validator;
};

} // namespace lvd::spiceui

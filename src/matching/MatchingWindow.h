#pragma once

#include "matching/MatchingSynthesis.h"

#include <QPointer>
#include <QWidget>

#include <complex>
#include <optional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace lvd {

class Document;
class MatchLadderView;
class PlotWidget;
class Project;

// Matching-network synthesis tool (requirement RF-10): enter source and load
// impedances, pick a topology, browse the solutions with their Smith chart
// trajectory and |Gamma| response, and insert one into a schematic.
// Usable as a top-level, non-modal window.
class MatchingWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MatchingWindow(QWidget *parent = nullptr);
    ~MatchingWindow() override;

    // Where "Insert into schematic" places the network; nullptr disables it.
    void setTarget(lvd::Project *project, lvd::Document *document);
    void setImpedances(std::complex<double> zs, std::complex<double> zl, double frequency);

    void setTopology(rf::MatchTopology topology);
    rf::MatchTopology topology() const;
    void setLoadedQ(double q);
    void setSections(int sections);
    void setLineImpedance(double z0); // 0: Re(Zs)
    void setStubSpacing(double degrees);
    void setStandardSeries(std::optional<ESeries> series); // E12, E24, E48 or E96

    // Reads the load at the design frequency from a one-port (Gamma) or the
    // S11 of a two-port Touchstone file.
    bool loadFromTouchstone(const QString &path, QString *error = nullptr);

    // Results of the last synthesis (snapped when a series is chosen).
    const rf::MatchResult &result() const { return m_result; }
    // The same solutions before snapping.
    const std::vector<rf::MatchSolution> &idealSolutions() const { return m_ideal; }
    int selectedSolution() const;
    void selectSolution(int index);
    QString message() const;

    // Inserts the selected solution into the target document.
    bool insertSelected(QString *error = nullptr);
    bool canInsert() const;

    PlotWidget *smithPlot() const { return m_smith; }
    PlotWidget *responsePlot() const { return m_response; }

    QSize sizeHint() const override;

public Q_SLOTS:
    void recompute();

Q_SIGNALS:
    void statusMessage(const QString &text);
    void networkInserted(const QString &inputLabel, const QString &outputLabel);

protected:
    void changeEvent(QEvent *event) override;

private:
    struct ImpedanceEditor {
        QComboBox *mode = nullptr; // R + jX, reflection
        QLineEdit *first = nullptr;
        QLineEdit *second = nullptr;
        QLabel *firstUnit = nullptr;
        QLabel *secondUnit = nullptr;
    };

    QWidget *createInputs();
    void createImpedanceEditor(ImpedanceEditor &editor);
    std::optional<std::complex<double>> readImpedance(const ImpedanceEditor &editor, QString *error) const;
    void writeImpedance(ImpedanceEditor &editor, std::complex<double> z);
    void impedanceModeChanged(ImpedanceEditor &editor);
    double referenceZ0() const;
    void updateTopologyFields();
    void fillResults();
    void showSelection();
    void setMessage(const QString &text, bool error);
    void browseTouchstone();
    void insertClicked();
    void updateInsertButton();

    QLineEdit *m_frequency = nullptr;
    QLineEdit *m_z0 = nullptr;
    ImpedanceEditor m_source;
    ImpedanceEditor m_load;
    QPushButton *m_touchstoneButton = nullptr;
    QComboBox *m_topology = nullptr;
    QDoubleSpinBox *m_q = nullptr;
    QSpinBox *m_sections = nullptr;
    QLineEdit *m_lineZ0 = nullptr;
    QDoubleSpinBox *m_spacing = nullptr;
    QComboBox *m_series = nullptr;
    QLabel *m_qLabel = nullptr;
    QLabel *m_sectionsLabel = nullptr;
    QLabel *m_lineZ0Label = nullptr;
    QLabel *m_spacingLabel = nullptr;
    QLabel *m_message = nullptr;

    QTreeWidget *m_solutions = nullptr;
    MatchLadderView *m_ladder = nullptr;
    PlotWidget *m_smith = nullptr;
    PlotWidget *m_response = nullptr;
    QPushButton *m_insertButton = nullptr;

    QPointer<Project> m_project;
    QPointer<Document> m_document;

    rf::MatchResult m_result;
    std::vector<rf::MatchSolution> m_ideal;
    bool m_updating = false;
};

} // namespace lvd

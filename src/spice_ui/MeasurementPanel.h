#pragma once

#include "spice/Fourier.h"
#include "spice/WaveformMath.h"

#include <QString>
#include <QWidget>

#include <optional>

class QAbstractItemModel;
class QDialog;
class QLabel;
class QLineEdit;
class QPushButton;
class QStandardItemModel;
class QTableView;
class QToolButton;

namespace lvd::spiceui {

// Measurements of one trace over a range of its scale: min, max, peak-to-
// peak, mean, RMS and, on time traces, frequency, period, duty cycle, rise
// and fall time, overshoot and THD, with a Fourier table on request.
// Failed measurements show a dash with the reason as tooltip.
class MeasurementPanel : public QWidget
{
    Q_OBJECT
public:
    struct Source {
        QString name;           // trace name, "v(out)"
        spice::Waveform waveform; // real values as shown (complex part applied)
        QString xUnit;          // "s" for transient traces: enables the time measurements
        spice::XWindow window;  // range measured
        QString rangeText;      // "visible range", "between cursors A and B"
        bool transient = false; // THD and Fourier
    };

    // Rows of the model, in order; the key is in Qt::UserRole + 1 of column 0.
    static QStringList measurementKeys(); // "min", "max", "pkpk", "mean", "rms", "frequency", "period",
                                          // "duty", "rise", "fall", "overshoot", "thd"

    explicit MeasurementPanel(QWidget *parent = nullptr);
    ~MeasurementPanel() override;

    void setSource(const Source &source);
    void clearSource(const QString &reason = QString());
    bool hasSource() const { return m_source.has_value(); }

    // THD and Fourier fundamental in Hz; 0: measured from the trace.
    void setFundamental(double hertz);
    double fundamental() const { return m_fundamental; }

    void setCollapsed(bool collapsed);
    bool isCollapsed() const;

    // Two columns: measurement, value (Qt::DisplayRole text, Qt::UserRole the
    // number in SI units when valid, Qt::ToolTipRole the reason of a failure).
    QAbstractItemModel *model() const;
    QString valueText(const QString &key) const;
    std::optional<double> value(const QString &key) const;
    bool isRowShown(const QString &key) const;

    // Fourier analysis of the source like SPICE .four: 9 harmonics of the last
    // period of the fundamental in the measured range.
    std::optional<spice::FourierResult> fourier(QString *error = nullptr) const;
    // Shows the harmonics table (non-modal; returns the dialog).
    QDialog *showFourierDialog();

Q_SIGNALS:
    void collapsedChanged(bool collapsed);

private:
    void recompute();
    int rowOf(const QString &key) const;
    void setRow(const QString &key, std::optional<double> value, const QString &unit, const QString &error,
                int digits = 4);

    std::optional<Source> m_source;
    double m_fundamental = 0;

    QToolButton *m_toggle = nullptr;
    QWidget *m_body = nullptr;
    QLabel *m_sourceLabel = nullptr;
    QStandardItemModel *m_model = nullptr;
    QTableView *m_table = nullptr;
    QWidget *m_fundamentalRow = nullptr;
    QLineEdit *m_fundamentalEdit = nullptr;
    QPushButton *m_fourierButton = nullptr;
    QDialog *m_fourierDialog = nullptr;
};

} // namespace lvd::spiceui

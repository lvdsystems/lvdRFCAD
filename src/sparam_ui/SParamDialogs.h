#pragma once

// Dialogs of the S-parameter editor operations. Each only collects input; the
// editor validates it and applies the operation as an undoable command.

#include "rfdata/NetworkOps.h"
#include "rfdata/Touchstone.h"

#include <QDialog>

#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QStackedWidget;
class QDialogButtonBox;

namespace lvd {

// Reference impedance per port, or one value for all ports.
class ReferenceImpedanceDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ReferenceImpedanceDialog(const std::vector<double> &z0, QWidget *parent = nullptr);
    std::vector<double> impedances() const;

private:
    void setAll();

    QDoubleSpinBox *m_all = nullptr;
    std::vector<QDoubleSpinBox *> m_ports;
};

// New port order: the list shows the old ports in their new positions.
class RenumberPortsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RenumberPortsDialog(int portCount, QWidget *parent = nullptr);
    // order[i] = old port index (0-based) that becomes new port i.
    std::vector<int> order() const;
    void setOrder(const std::vector<int> &order);

private:
    void move(int delta);
    void reverse();
    void updateButtons();

    QListWidget *m_list = nullptr;
    QWidget *m_up = nullptr;
    QWidget *m_down = nullptr;
};

// New frequency grid: linear or logarithmic sweep, or a list.
class ResampleDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ResampleDialog(double minimumHz, double maximumHz, int points, QWidget *parent = nullptr);
    std::optional<std::vector<double>> frequencies(QString *error = nullptr) const;
    rf::Extrapolation extrapolation() const;

private:
    void updatePreview();

    QComboBox *m_mode = nullptr;
    QWidget *m_sweep = nullptr;
    QLineEdit *m_start = nullptr;
    QLineEdit *m_stop = nullptr;
    QSpinBox *m_points = nullptr;
    QPlainTextEdit *m_list = nullptr;
    QComboBox *m_extrapolation = nullptr;
    QLabel *m_preview = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

class CropDialog : public QDialog
{
    Q_OBJECT
public:
    CropDialog(double minimumHz, double maximumHz, QWidget *parent = nullptr);
    std::optional<std::pair<double, double>> range(QString *error = nullptr) const;

private:
    QLineEdit *m_start = nullptr;
    QLineEdit *m_stop = nullptr;
};

// Cascade with a two-port file, before or after this network.
class CascadeDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CascadeDialog(const QString &directory, QWidget *parent = nullptr);
    QString path() const;
    bool otherAfter() const; // true: this network, then the file

private:
    QLineEdit *m_path = nullptr;
    QComboBox *m_position = nullptr;
};

// De-embedding fixture files, left and/or right.
class DeembedDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DeembedDialog(const QString &directory, QWidget *parent = nullptr);
    QString leftPath() const;  // empty: none
    QString rightPath() const; // empty: none

private:
    QLineEdit *m_left = nullptr;
    QLineEdit *m_right = nullptr;
};

// Touchstone write options; version 1 is offered only with equal references.
class SaveOptionsDialog : public QDialog
{
    Q_OBJECT
public:
    SaveOptionsDialog(const rf::TouchstoneWriteOptions &options, bool version1Allowed, QWidget *parent = nullptr);
    rf::TouchstoneWriteOptions options() const;

private:
    QComboBox *m_version = nullptr;
    QComboBox *m_format = nullptr;
    QComboBox *m_unit = nullptr;
    QSpinBox *m_precision = nullptr;
};

// A line edit with a "Browse..." button that opens a Touchstone file.
QWidget *touchstoneFileField(QLineEdit *edit, const QString &directory, QWidget *parent);
// File dialog filter for Touchstone files.
QString touchstoneFileFilter();

} // namespace lvd

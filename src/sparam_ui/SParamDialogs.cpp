#include "sparam_ui/SParamDialogs.h"

#include "sparam_ui/SParamFormat.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace lvd {

namespace {

QDialogButtonBox *okCancel(QDialog *dialog)
{
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

QDoubleSpinBox *impedanceBox(double value, QWidget *parent)
{
    auto *box = new QDoubleSpinBox(parent);
    box->setRange(1e-3, 1e6);
    box->setDecimals(4);
    box->setSuffix(QStringLiteral(" Ω"));
    box->setValue(value);
    return box;
}

} // namespace

QString touchstoneFileFilter()
{
    return QCoreApplication::translate("lvd::TouchstoneEditor", "Touchstone files (*.s*p *.ts);;All files (*)");
}

QWidget *touchstoneFileField(QLineEdit *edit, const QString &directory, QWidget *parent)
{
    auto *field = new QWidget(parent);
    auto *layout = new QHBoxLayout(field);
    layout->setContentsMargins(0, 0, 0, 0);
    edit->setParent(field);
    edit->setMinimumWidth(320);
    auto *browse = new QPushButton(QCoreApplication::translate("lvd::TouchstoneEditor", "Browse…"), field);
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, field, [edit, directory, field]() {
        const QString start = edit->text().isEmpty() ? directory : edit->text();
        const QString path = QFileDialog::getOpenFileName(
            field, QCoreApplication::translate("lvd::TouchstoneEditor", "Open Touchstone file"), start,
            touchstoneFileFilter());
        if (!path.isEmpty())
            edit->setText(path);
    });
    return field;
}

// ------------------------------------------------------------------ references

ReferenceImpedanceDialog::ReferenceImpedanceDialog(const std::vector<double> &z0, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Reference Impedances"));
    auto *layout = new QVBoxLayout(this);
    auto *note = new QLabel(tr("The S-parameters are renormalised to the new reference impedances."), this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *allRow = new QHBoxLayout;
    allRow->addWidget(new QLabel(tr("All ports:"), this));
    m_all = impedanceBox(z0.empty() ? rf::DefaultZ0 : z0.front(), this);
    allRow->addWidget(m_all, 1);
    auto *apply = new QPushButton(tr("Set All"), this);
    allRow->addWidget(apply);
    layout->addLayout(allRow);
    connect(apply, &QPushButton::clicked, this, &ReferenceImpedanceDialog::setAll);

    auto *form = new QFormLayout;
    for (std::size_t i = 0; i < z0.size(); ++i) {
        QDoubleSpinBox *box = impedanceBox(z0[i], this);
        box->setObjectName(QStringLiteral("port%1").arg(i + 1));
        m_ports.push_back(box);
        form->addRow(tr("Port %1:").arg(i + 1), box);
    }
    layout->addLayout(form);
    layout->addWidget(okCancel(this));
}

void ReferenceImpedanceDialog::setAll()
{
    for (QDoubleSpinBox *box : m_ports)
        box->setValue(m_all->value());
}

std::vector<double> ReferenceImpedanceDialog::impedances() const
{
    std::vector<double> z0;
    for (const QDoubleSpinBox *box : m_ports)
        z0.push_back(box->value());
    return z0;
}

// ------------------------------------------------------------------ renumber

RenumberPortsDialog::RenumberPortsDialog(int portCount, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Renumber Ports"));
    auto *layout = new QVBoxLayout(this);
    auto *note = new QLabel(tr("Arrange the ports in their new order: the first row becomes port 1."), this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *row = new QHBoxLayout;
    m_list = new QListWidget(this);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    for (int i = 0; i < portCount; ++i) {
        auto *item = new QListWidgetItem(tr("Port %1").arg(i + 1), m_list);
        item->setData(Qt::UserRole, i);
    }
    row->addWidget(m_list, 1);
    auto *buttons = new QVBoxLayout;
    auto *up = new QPushButton(tr("Move Up"), this);
    auto *down = new QPushButton(tr("Move Down"), this);
    auto *reverse = new QPushButton(tr("Reverse"), this);
    m_up = up;
    m_down = down;
    buttons->addWidget(up);
    buttons->addWidget(down);
    buttons->addWidget(reverse);
    buttons->addStretch(1);
    row->addLayout(buttons);
    layout->addLayout(row);
    layout->addWidget(okCancel(this));
    connect(up, &QPushButton::clicked, this, [this]() { move(-1); });
    connect(down, &QPushButton::clicked, this, [this]() { move(1); });
    connect(reverse, &QPushButton::clicked, this, &RenumberPortsDialog::reverse);
    connect(m_list, &QListWidget::currentRowChanged, this, &RenumberPortsDialog::updateButtons);
    m_list->setCurrentRow(0);
    updateButtons();
}

void RenumberPortsDialog::move(int delta)
{
    const int row = m_list->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_list->count())
        return;
    QListWidgetItem *item = m_list->takeItem(row);
    m_list->insertItem(target, item);
    m_list->setCurrentRow(target);
}

void RenumberPortsDialog::reverse()
{
    std::vector<int> o = order();
    std::reverse(o.begin(), o.end());
    setOrder(o);
}

void RenumberPortsDialog::updateButtons()
{
    const int row = m_list->currentRow();
    m_up->setEnabled(row > 0);
    m_down->setEnabled(row >= 0 && row + 1 < m_list->count());
}

std::vector<int> RenumberPortsDialog::order() const
{
    std::vector<int> result;
    for (int i = 0; i < m_list->count(); ++i)
        result.push_back(m_list->item(i)->data(Qt::UserRole).toInt());
    return result;
}

void RenumberPortsDialog::setOrder(const std::vector<int> &order)
{
    m_list->clear();
    for (const int old : order) {
        auto *item = new QListWidgetItem(tr("Port %1").arg(old + 1), m_list);
        item->setData(Qt::UserRole, old);
    }
    m_list->setCurrentRow(0);
    updateButtons();
}

// ------------------------------------------------------------------ resample

ResampleDialog::ResampleDialog(double minimumHz, double maximumHz, int points, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Resample"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_mode = new QComboBox(this);
    m_mode->addItem(tr("Linear sweep"));
    m_mode->addItem(tr("Logarithmic sweep"));
    m_mode->addItem(tr("List of frequencies"));
    form->addRow(tr("Grid:"), m_mode);
    layout->addLayout(form);

    auto *sweep = new QWidget(this);
    m_sweep = sweep;
    auto *sweepForm = new QFormLayout(sweep);
    sweepForm->setContentsMargins(0, 0, 0, 0);
    m_start = new QLineEdit(sparam::formatFrequency(minimumHz, 10), sweep);
    m_stop = new QLineEdit(sparam::formatFrequency(maximumHz, 10), sweep);
    m_points = new QSpinBox(sweep);
    m_points->setRange(1, 10000000);
    m_points->setValue(std::max(points, 2));
    sweepForm->addRow(tr("Start:"), m_start);
    sweepForm->addRow(tr("Stop:"), m_stop);
    sweepForm->addRow(tr("Points:"), m_points);
    layout->addWidget(sweep);
    m_list = new QPlainTextEdit(this);
    m_list->setFixedHeight(m_list->fontMetrics().lineSpacing() * 4 + 12);
    m_list->setPlaceholderText(tr("Frequencies separated by spaces or new lines, e.g. 1 GHz 1.5 GHz 2.2 GHz"));
    m_list->setVisible(false);
    layout->addWidget(m_list);

    auto *bottom = new QFormLayout;
    m_extrapolation = new QComboBox(this);
    m_extrapolation->addItem(tr("Error (refuse frequencies outside the data)"));
    m_extrapolation->addItem(tr("Hold the end values"));
    bottom->addRow(tr("Outside the data:"), m_extrapolation);
    m_preview = new QLabel(this);
    m_preview->setWordWrap(true);
    bottom->addRow(m_preview);
    layout->addLayout(bottom);
    m_buttons = okCancel(this);
    layout->addWidget(m_buttons);

    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_sweep->setVisible(index != 2);
        m_list->setVisible(index == 2);
        adjustSize();
        updatePreview();
    });
    connect(m_start, &QLineEdit::textChanged, this, &ResampleDialog::updatePreview);
    connect(m_stop, &QLineEdit::textChanged, this, &ResampleDialog::updatePreview);
    connect(m_points, &QSpinBox::valueChanged, this, &ResampleDialog::updatePreview);
    connect(m_list, &QPlainTextEdit::textChanged, this, &ResampleDialog::updatePreview);
    updatePreview();
}

std::optional<std::vector<double>> ResampleDialog::frequencies(QString *error) const
{
    if (m_mode->currentIndex() == 2)
        return sparam::parseFrequencyList(m_list->toPlainText(), 1e9, error);
    const std::optional<double> start = sparam::parseFrequency(m_start->text(), 1e9);
    const std::optional<double> stop = sparam::parseFrequency(m_stop->text(), 1e9);
    if (!start || !stop) {
        if (error)
            *error = tr("Enter start and stop frequencies such as \"1 GHz\".");
        return std::nullopt;
    }
    if (!(*stop > *start) && m_points->value() > 1) {
        if (error)
            *error = tr("The stop frequency must be above the start frequency.");
        return std::nullopt;
    }
    if (m_mode->currentIndex() == 1) {
        if (!(*start > 0)) {
            if (error)
                *error = tr("A logarithmic sweep needs a start frequency above 0 Hz.");
            return std::nullopt;
        }
        return sparam::logGrid(*start, *stop, m_points->value());
    }
    return sparam::linearGrid(*start, *stop, m_points->value());
}

rf::Extrapolation ResampleDialog::extrapolation() const
{
    return m_extrapolation->currentIndex() == 1 ? rf::Extrapolation::Hold : rf::Extrapolation::Error;
}

void ResampleDialog::updatePreview()
{
    QString error;
    const auto grid = frequencies(&error);
    if (!grid) {
        m_preview->setText(error);
    } else {
        m_preview->setText(tr("%1 frequencies from %2 to %3")
                               .arg(grid->size())
                               .arg(sparam::formatFrequency(grid->front()), sparam::formatFrequency(grid->back())));
    }
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(grid.has_value());
}

// ------------------------------------------------------------------ crop

CropDialog::CropDialog(double minimumHz, double maximumHz, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Crop Frequency Range"));
    auto *layout = new QVBoxLayout(this);
    auto *note = new QLabel(tr("Keeps the frequencies inside the range (inclusive); no interpolation."), this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *form = new QFormLayout;
    m_start = new QLineEdit(sparam::formatFrequency(minimumHz, 10), this);
    m_stop = new QLineEdit(sparam::formatFrequency(maximumHz, 10), this);
    form->addRow(tr("From:"), m_start);
    form->addRow(tr("To:"), m_stop);
    layout->addLayout(form);
    layout->addWidget(okCancel(this));
}

std::optional<std::pair<double, double>> CropDialog::range(QString *error) const
{
    const std::optional<double> start = sparam::parseFrequency(m_start->text(), 1e9);
    const std::optional<double> stop = sparam::parseFrequency(m_stop->text(), 1e9);
    if (!start || !stop || *stop < *start) {
        if (error)
            *error = tr("Enter a valid frequency range such as \"1 GHz\" to \"3 GHz\".");
        return std::nullopt;
    }
    return std::make_pair(*start, *stop);
}

// ------------------------------------------------------------------ cascade

CascadeDialog::CascadeDialog(const QString &directory, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Cascade with Two-Port"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_path = new QLineEdit;
    form->addRow(tr("Two-port file:"), touchstoneFileField(m_path, directory, this));
    m_position = new QComboBox(this);
    m_position->addItem(tr("After this network (this → file)"));
    m_position->addItem(tr("Before this network (file → this)"));
    form->addRow(tr("Connection:"), m_position);
    layout->addLayout(form);
    layout->addWidget(okCancel(this));
}

QString CascadeDialog::path() const
{
    return m_path->text().trimmed();
}

bool CascadeDialog::otherAfter() const
{
    return m_position->currentIndex() == 0;
}

// ------------------------------------------------------------------ de-embed

DeembedDialog::DeembedDialog(const QString &directory, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("De-embed Fixtures"));
    auto *layout = new QVBoxLayout(this);
    auto *note = new QLabel(tr("Removes two-port fixtures from port 1 (left) and/or port 2 (right). "
                               "Leave a field empty to de-embed one side only."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *form = new QFormLayout;
    m_left = new QLineEdit;
    m_right = new QLineEdit;
    form->addRow(tr("Left fixture:"), touchstoneFileField(m_left, directory, this));
    form->addRow(tr("Right fixture:"), touchstoneFileField(m_right, directory, this));
    layout->addLayout(form);
    layout->addWidget(okCancel(this));
}

QString DeembedDialog::leftPath() const
{
    return m_left->text().trimmed();
}

QString DeembedDialog::rightPath() const
{
    return m_right->text().trimmed();
}

// ------------------------------------------------------------------ save options

SaveOptionsDialog::SaveOptionsDialog(const rf::TouchstoneWriteOptions &options, bool version1Allowed,
                                     QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Touchstone Options"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_version = new QComboBox(this);
    m_version->addItem(tr("Version 1.1"), 1);
    m_version->addItem(tr("Version 2.0"), 2);
    m_version->setCurrentIndex(options.version >= 2 || !version1Allowed ? 1 : 0);
    if (!version1Allowed) {
        m_version->setItemData(0, false, Qt::UserRole - 1); // disable the item
        m_version->setToolTip(tr("Version 1 needs the same reference impedance at every port."));
    }
    form->addRow(tr("Version:"), m_version);
    m_format = new QComboBox(this);
    for (const auto format : {rf::TouchstoneFormat::RealImaginary, rf::TouchstoneFormat::MagnitudeAngle,
                              rf::TouchstoneFormat::DecibelAngle})
        m_format->addItem(sparam::touchstoneFormatName(format), static_cast<int>(format));
    m_format->setCurrentIndex(static_cast<int>(options.format));
    form->addRow(tr("Data format:"), m_format);
    m_unit = new QComboBox(this);
    m_unit->addItems({QStringLiteral("Hz"), QStringLiteral("kHz"), QStringLiteral("MHz"), QStringLiteral("GHz")});
    m_unit->setCurrentText(options.frequencyUnit);
    form->addRow(tr("Frequency unit:"), m_unit);
    m_precision = new QSpinBox(this);
    m_precision->setRange(3, 17);
    m_precision->setValue(options.precision);
    form->addRow(tr("Significant digits:"), m_precision);
    layout->addLayout(form);
    layout->addWidget(okCancel(this));
}

rf::TouchstoneWriteOptions SaveOptionsDialog::options() const
{
    rf::TouchstoneWriteOptions options;
    options.version = m_version->currentData().toInt();
    options.format = static_cast<rf::TouchstoneFormat>(m_format->currentData().toInt());
    options.frequencyUnit = m_unit->currentText();
    options.precision = m_precision->value();
    return options;
}

} // namespace lvd

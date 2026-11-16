#include "spice_ui/MeasurementPanel.h"

#include "core/Expression.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lvd::spiceui {

namespace {

constexpr int KeyRole = Qt::UserRole + 1;

struct Row {
    const char *key;
    const char *label;
};

constexpr Row kRows[] = {
    {"min", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "Minimum")},
    {"max", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "Maximum")},
    {"pkpk", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "Peak to peak")},
    {"mean", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "Mean")},
    {"rms", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "RMS")},
    {"frequency", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "Frequency")},
    {"period", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "Period")},
    {"duty", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "Duty cycle")},
    {"rise", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "Rise time 10-90 %")},
    {"fall", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "Fall time 90-10 %")},
    {"overshoot", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "Overshoot")},
    {"thd", QT_TRANSLATE_NOOP("lvd::spiceui::MeasurementPanel", "THD (9 harmonics)")},
};

const QString kDash = QString(QChar(0x2014));

QString formatted(double value, const QString &unit, int digits = 4)
{
    if (!std::isfinite(value))
        return kDash;
    return formatEngineering(value, unit, digits);
}

} // namespace

QStringList MeasurementPanel::measurementKeys()
{
    QStringList keys;
    for (const Row &row : kRows)
        keys << QLatin1String(row.key);
    return keys;
}

MeasurementPanel::MeasurementPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("measurementPanel"));

    m_toggle = new QToolButton(this);
    m_toggle->setObjectName(QStringLiteral("measurementsToggle"));
    m_toggle->setText(tr("Measurements"));
    m_toggle->setCheckable(true);
    m_toggle->setChecked(true);
    m_toggle->setAutoRaise(true);
    m_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_toggle->setArrowType(Qt::DownArrow);
    QFont bold = m_toggle->font();
    bold.setBold(true);
    m_toggle->setFont(bold);

    m_body = new QWidget(this);
    m_sourceLabel = new QLabel(m_body);
    m_sourceLabel->setObjectName(QStringLiteral("measurementSource"));
    m_sourceLabel->setWordWrap(true);
    m_sourceLabel->setTextFormat(Qt::RichText);

    m_model = new QStandardItemModel(0, 2, this);
    m_model->setHorizontalHeaderLabels({tr("Measurement"), tr("Value")});
    for (const Row &row : kRows) {
        auto *name = new QStandardItem(tr(row.label));
        name->setData(QLatin1String(row.key), KeyRole);
        name->setEditable(false);
        auto *value = new QStandardItem(kDash);
        value->setEditable(false);
        value->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_model->appendRow({name, value});
    }

    m_table = new QTableView(m_body);
    m_table->setObjectName(QStringLiteral("measurements"));
    m_table->setModel(m_model);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setMinimumSectionSize(m_table->fontMetrics().height() + 4);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_fundamentalEdit = new QLineEdit(m_body);
    m_fundamentalEdit->setObjectName(QStringLiteral("thdFundamental"));
    m_fundamentalEdit->setPlaceholderText(tr("measured"));
    m_fundamentalEdit->setToolTip(tr("Fundamental frequency of the THD and the Fourier analysis, such as 1k or "
                                     "50 Hz; empty: measured from the trace"));
    m_fundamentalEdit->setClearButtonEnabled(true);
    m_fourierButton = new QPushButton(tr("Fourier..."), m_body);
    m_fourierButton->setObjectName(QStringLiteral("fourierButton"));
    m_fourierButton->setToolTip(tr("Harmonics of the trace (9 harmonics of the last period, as SPICE .four)"));

    m_fundamentalRow = new QWidget(m_body);
    auto *fundamentalRow = new QHBoxLayout(m_fundamentalRow);
    fundamentalRow->setContentsMargins(0, 0, 0, 0);
    fundamentalRow->setSpacing(4);
    fundamentalRow->addWidget(new QLabel(tr("Fundamental:"), m_fundamentalRow));
    fundamentalRow->addWidget(m_fundamentalEdit, 1);
    fundamentalRow->addWidget(m_fourierButton);

    auto *bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(4);
    bodyLayout->addWidget(m_sourceLabel);
    bodyLayout->addWidget(m_table, 1);
    bodyLayout->addWidget(m_fundamentalRow);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_toggle, 0, Qt::AlignLeft);
    layout->addWidget(m_body, 1);

    connect(m_toggle, &QToolButton::toggled, this, [this](bool open) {
        m_body->setVisible(open);
        m_toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        Q_EMIT collapsedChanged(!open);
    });
    connect(m_fundamentalEdit, &QLineEdit::editingFinished, this, [this] {
        const QString text = m_fundamentalEdit->text().trimmed();
        if (text.isEmpty()) {
            setFundamental(0);
            return;
        }
        const auto value = parseNumber(text);
        if (value && *value > 0)
            setFundamental(*value);
        else
            m_fundamentalEdit->setText(m_fundamental > 0 ? formatEngineering(m_fundamental, QStringLiteral("Hz"))
                                                         : QString());
    });
    connect(m_fourierButton, &QPushButton::clicked, this, [this] { showFourierDialog(); });

    clearSource();
}

MeasurementPanel::~MeasurementPanel() = default;

void MeasurementPanel::setSource(const Source &source)
{
    m_source = source;
    recompute();
}

void MeasurementPanel::clearSource(const QString &reason)
{
    m_source.reset();
    m_sourceLabel->setText(reason.isEmpty() ? tr("No trace selected") : reason.toHtmlEscaped());
    for (int r = 0; r < m_model->rowCount(); ++r) {
        QStandardItem *item = m_model->item(r, 1);
        item->setText(kDash);
        item->setData(QVariant(), Qt::UserRole);
        item->setToolTip(QString());
        m_table->setRowHidden(r, false);
    }
    m_table->setRowHidden(rowOf(QStringLiteral("thd")), true);
    m_fourierButton->setEnabled(false);
    m_fundamentalEdit->setEnabled(false);
    m_fundamentalRow->setVisible(false);
}

void MeasurementPanel::setFundamental(double hertz)
{
    m_fundamental = hertz > 0 ? hertz : 0;
    const QString text = m_fundamental > 0 ? formatEngineering(m_fundamental, QStringLiteral("Hz")) : QString();
    if (m_fundamentalEdit->text() != text)
        m_fundamentalEdit->setText(text);
    recompute();
}

void MeasurementPanel::setCollapsed(bool collapsed)
{
    m_toggle->setChecked(!collapsed);
}

bool MeasurementPanel::isCollapsed() const
{
    return !m_toggle->isChecked();
}

QAbstractItemModel *MeasurementPanel::model() const
{
    return m_model;
}

int MeasurementPanel::rowOf(const QString &key) const
{
    for (int r = 0; r < m_model->rowCount(); ++r)
        if (m_model->item(r, 0)->data(KeyRole).toString() == key)
            return r;
    return -1;
}

QString MeasurementPanel::valueText(const QString &key) const
{
    const int r = rowOf(key);
    return r >= 0 ? m_model->item(r, 1)->text() : QString();
}

std::optional<double> MeasurementPanel::value(const QString &key) const
{
    const int r = rowOf(key);
    if (r < 0)
        return std::nullopt;
    const QVariant v = m_model->item(r, 1)->data(Qt::UserRole);
    if (!v.isValid())
        return std::nullopt;
    return v.toDouble();
}

bool MeasurementPanel::isRowShown(const QString &key) const
{
    const int r = rowOf(key);
    return r >= 0 && !m_table->isRowHidden(r);
}

void MeasurementPanel::setRow(const QString &key, std::optional<double> value, const QString &unit,
                              const QString &error, int digits)
{
    const int r = rowOf(key);
    if (r < 0)
        return;
    QStandardItem *item = m_model->item(r, 1);
    if (value && std::isfinite(*value)) {
        item->setText(formatted(*value, unit, digits));
        item->setData(*value, Qt::UserRole);
        item->setToolTip(QString());
    } else {
        item->setText(kDash);
        item->setData(QVariant(), Qt::UserRole);
        item->setToolTip(error.isEmpty() ? tr("not available") : error);
    }
}

std::optional<spice::FourierResult> MeasurementPanel::fourier(QString *error) const
{
    if (!m_source || !m_source->transient) {
        if (error)
            *error = tr("Fourier analysis needs a transient trace");
        return std::nullopt;
    }
    double fundamental = m_fundamental;
    // Measured over the range, or over the whole trace when the range is too
    // short for that (one period between the cursors).
    if (fundamental <= 0)
        fundamental = spice::frequency(m_source->waveform, m_source->window).value_or(0.0);
    if (fundamental <= 0) {
        QString frequencyError;
        if (const auto f = spice::frequency(m_source->waveform, {}, &frequencyError)) {
            fundamental = *f;
        } else {
            if (error)
                *error = tr("cannot measure the fundamental: %1").arg(frequencyError);
            return std::nullopt;
        }
    }
    // Like .four: the last period ending at the end of the range (the range
    // may hold just one period, which rounding could otherwise cut short).
    spice::XWindow window;
    window.to = m_source->window.to;
    return spice::fourier(m_source->waveform, fundamental, 9, 1, window, error);
}

void MeasurementPanel::recompute()
{
    if (!m_source) {
        clearSource();
        return;
    }
    const Source &s = *m_source;
    const spice::Waveform &w = s.waveform;
    const spice::XWindow window = s.window;
    const QString yUnit = w.unit;
    const QString xUnit = s.xUnit;
    m_sourceLabel->setText(QStringLiteral("<b>%1</b><br>%2").arg(s.name.toHtmlEscaped(), s.rangeText.toHtmlEscaped()));

    QString error;
    const auto maximum = spice::maximum(w, window, &error);
    setRow(QStringLiteral("max"), maximum ? std::optional(maximum->value) : std::nullopt, yUnit, error);
    if (maximum)
        m_model->item(rowOf(QStringLiteral("max")), 1)
            ->setToolTip(tr("at %1").arg(formatEngineering(maximum->x, xUnit, 6)));
    error.clear();
    const auto minimum = spice::minimum(w, window, &error);
    setRow(QStringLiteral("min"), minimum ? std::optional(minimum->value) : std::nullopt, yUnit, error);
    if (minimum)
        m_model->item(rowOf(QStringLiteral("min")), 1)
            ->setToolTip(tr("at %1").arg(formatEngineering(minimum->x, xUnit, 6)));
    error.clear();
    const auto pkpk = spice::peakToPeak(w, window, &error);
    setRow(QStringLiteral("pkpk"), pkpk, yUnit, error);
    error.clear();
    const auto mean = spice::mean(w, window, &error);
    setRow(QStringLiteral("mean"), mean, yUnit, error);
    error.clear();
    const auto rms = spice::rms(w, window, &error);
    setRow(QStringLiteral("rms"), rms, yUnit, error);

    const bool time = xUnit == QLatin1String("s");
    for (const char *key : {"frequency", "period", "duty", "rise", "fall", "overshoot"})
        m_table->setRowHidden(rowOf(QLatin1String(key)), !time);
    m_table->setRowHidden(rowOf(QStringLiteral("thd")), !s.transient);
    if (time) {
        error.clear();
        const auto frequency = spice::frequency(w, window, &error);
        setRow(QStringLiteral("frequency"), frequency, QStringLiteral("Hz"), error, 5);
        error.clear();
        const auto period = spice::period(w, window, &error);
        setRow(QStringLiteral("period"), period, QStringLiteral("s"), error, 5);
        error.clear();
        const auto duty = spice::dutyCycle(w, window, &error);
        setRow(QStringLiteral("duty"), duty ? std::optional(*duty * 100.0) : std::nullopt, QStringLiteral("%"), error);
        error.clear();
        const auto rise = spice::riseTime(w, window, 0.1, 0.9, &error);
        setRow(QStringLiteral("rise"), rise, QStringLiteral("s"), error);
        error.clear();
        const auto fall = spice::fallTime(w, window, 0.1, 0.9, &error);
        setRow(QStringLiteral("fall"), fall, QStringLiteral("s"), error);
        error.clear();
        const auto overshoot = spice::overshoot(w, window, &error);
        setRow(QStringLiteral("overshoot"), overshoot, QStringLiteral("%"), error);
    }
    m_fourierButton->setEnabled(s.transient);
    m_fundamentalEdit->setEnabled(s.transient);
    m_fundamentalRow->setVisible(s.transient);
    if (s.transient) {
        error.clear();
        const auto result = fourier(&error);
        setRow(QStringLiteral("thd"), result ? std::optional(result->thdPercent) : std::nullopt, QStringLiteral("%"),
               error);
        if (result)
            m_model->item(rowOf(QStringLiteral("thd")), 1)
                ->setToolTip(tr("Fundamental %1, last period from %2 to %3")
                                 .arg(formatEngineering(result->fundamental, QStringLiteral("Hz"), 5),
                                      formatEngineering(result->from, QStringLiteral("s"), 5),
                                      formatEngineering(result->to, QStringLiteral("s"), 5)));
    }
    if (m_fourierDialog && m_fourierDialog->isVisible())
        showFourierDialog();
}

QDialog *MeasurementPanel::showFourierDialog()
{
    if (!m_fourierDialog) {
        m_fourierDialog = new QDialog(this);
        m_fourierDialog->setObjectName(QStringLiteral("fourierDialog"));
        m_fourierDialog->setWindowTitle(tr("Fourier Analysis"));
        auto *summary = new QLabel(m_fourierDialog);
        summary->setObjectName(QStringLiteral("fourierSummary"));
        summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        summary->setWordWrap(true);
        auto *table = new QTableWidget(m_fourierDialog);
        table->setObjectName(QStringLiteral("harmonics"));
        table->setColumnCount(6);
        table->setHorizontalHeaderLabels({tr("n"), tr("Frequency"), tr("Magnitude"), tr("Phase"),
                                          tr("Normalised magnitude"), tr("Normalised phase")});
        table->verticalHeader()->hide();
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, m_fourierDialog);
        connect(buttons, &QDialogButtonBox::rejected, m_fourierDialog, &QDialog::close);
        auto *layout = new QVBoxLayout(m_fourierDialog);
        layout->addWidget(summary);
        layout->addWidget(table, 1);
        layout->addWidget(buttons);
        m_fourierDialog->resize(640, 400);
    }
    auto *summary = m_fourierDialog->findChild<QLabel *>(QStringLiteral("fourierSummary"));
    auto *table = m_fourierDialog->findChild<QTableWidget *>(QStringLiteral("harmonics"));
    QString error;
    const auto result = fourier(&error);
    table->setRowCount(0);
    if (!result) {
        summary->setText(tr("Fourier analysis not available: %1").arg(error));
    } else {
        const QString unit = m_source ? m_source->waveform.unit : QString();
        summary->setText(tr("<b>%1</b><br>Fundamental %2, DC component %3, THD %4<br>Analysed interval %5 to %6")
                             .arg(m_source->name.toHtmlEscaped(),
                                  formatEngineering(result->fundamental, QStringLiteral("Hz"), 6),
                                  formatEngineering(result->dc, unit, 5),
                                  formatEngineering(result->thdPercent, QStringLiteral("%"), 4),
                                  formatEngineering(result->from, QStringLiteral("s"), 5),
                                  formatEngineering(result->to, QStringLiteral("s"), 5)));
        table->setRowCount(int(result->harmonics.size()));
        int row = 0;
        for (const spice::Harmonic &h : result->harmonics) {
            const QStringList cells{QString::number(h.n),
                                    formatEngineering(h.frequency, QStringLiteral("Hz"), 6),
                                    formatEngineering(h.magnitude, unit, 5),
                                    QStringLiteral("%1°").arg(h.phaseDeg, 0, 'f', 2),
                                    QString::number(h.normalizedMagnitude, 'g', 5),
                                    QStringLiteral("%1°").arg(h.normalizedPhaseDeg, 0, 'f', 2)};
            for (int c = 0; c < cells.size(); ++c) {
                auto *item = new QTableWidgetItem(cells[c]);
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                table->setItem(row, c, item);
            }
            ++row;
        }
    }
    m_fourierDialog->show();
    m_fourierDialog->raise();
    return m_fourierDialog;
}

} // namespace lvd::spiceui

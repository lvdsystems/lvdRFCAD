#include "simulation_ui/AnalysisDialog.h"

#include "core/Document.h"
#include "core/Netlist.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "simulation_ui/PlotDefinitions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <cmath>

namespace lvd {

namespace {

QStringList splitList(const QString &text)
{
    static const QRegularExpression separators(QStringLiteral("[\\n;]"));
    QStringList items;
    for (const QString &part : text.split(separators)) {
        const QString item = part.trimmed();
        if (!item.isEmpty())
            items.append(item);
    }
    return items;
}

} // namespace

AnalysisDialog::AnalysisDialog(Project *project, Document *document, const QString &analysisId, QWidget *parent)
    : QDialog(parent), m_project(project), m_document(document)
{
    setObjectName(QStringLiteral("analysisDialog"));
    QStringList variableErrors;
    m_scope = document ? rf::sheetScope(*document, &variableErrors) : Expression::Lookup();

    rf::LinearAnalysisSetup initial;
    initial.name = tr("S-parameters");
    if (document && document->schematic()) {
        for (const rf::LinearAnalysisSetup &existing : rf::linearAnalyses(document->schematic()->simulation())) {
            if (!analysisId.isEmpty() && existing.id == analysisId) {
                initial = existing;
                m_isNew = false;
            }
        }
    }
    m_id = m_isNew ? QString() : initial.id;
    setWindowTitle(m_isNew ? tr("New linear analysis") : tr("Edit analysis – %1").arg(initial.name));

    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("name"));
    m_sweep = new QComboBox(this);
    m_sweep->setObjectName(QStringLiteral("sweep"));
    m_sweep->addItem(tr("Linear"), QStringLiteral("linear"));
    m_sweep->addItem(tr("Logarithmic"), QStringLiteral("log"));
    m_sweep->addItem(tr("Frequency list"), QStringLiteral("list"));

    const auto field = [this](const QString &name, const QString &tip) {
        auto *edit = new QLineEdit(this);
        edit->setObjectName(name);
        edit->setToolTip(tip);
        return edit;
    };
    const auto valueLabel = [this](const QString &name) {
        auto *label = new QLabel(this);
        label->setObjectName(name);
        label->setMinimumWidth(110);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };
    const QString expressionTip = tr("A value or an expression with the schematic's variables, such as 2.4GHz or f0*3");
    m_start = field(QStringLiteral("start"), expressionTip);
    m_stop = field(QStringLiteral("stop"), expressionTip);
    m_points = field(QStringLiteral("points"), tr("Number of frequency points, 1 to 100,000"));
    m_startValue = valueLabel(QStringLiteral("startValue"));
    m_stopValue = valueLabel(QStringLiteral("stopValue"));
    m_pointsValue = valueLabel(QStringLiteral("pointsValue"));

    auto *rangePage = new QWidget(this);
    auto *grid = new QGridLayout(rangePage);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->addWidget(new QLabel(tr("Start:"), rangePage), 0, 0);
    grid->addWidget(m_start, 0, 1);
    grid->addWidget(m_startValue, 0, 2);
    grid->addWidget(new QLabel(tr("Stop:"), rangePage), 1, 0);
    grid->addWidget(m_stop, 1, 1);
    grid->addWidget(m_stopValue, 1, 2);
    grid->addWidget(new QLabel(tr("Points:"), rangePage), 2, 0);
    grid->addWidget(m_points, 2, 1);
    grid->addWidget(m_pointsValue, 2, 2);
    grid->setColumnStretch(1, 1);
    grid->setRowStretch(3, 1);

    auto *listPage = new QWidget(this);
    auto *listLayout = new QVBoxLayout(listPage);
    listLayout->setContentsMargins(0, 0, 0, 0);
    m_list = new QPlainTextEdit(listPage);
    m_list->setObjectName(QStringLiteral("list"));
    m_list->setPlaceholderText(tr("One frequency per line, for example\n900MHz\n1.8GHz\nf0"));
    m_list->setTabChangesFocus(true);
    m_listHint = new QLabel(listPage);
    m_listHint->setWordWrap(true);
    listLayout->addWidget(m_list);
    listLayout->addWidget(m_listHint);

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(rangePage);
    m_pages->addWidget(listPage);

    m_noise = new QCheckBox(tr("Compute noise (noise figure and noise parameters)"), this);
    m_noise->setObjectName(QStringLiteral("noise"));

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setObjectName(QStringLiteral("summary"));
    m_summaryLabel->setWordWrap(true);
    m_variablesLabel = new QLabel(this);
    m_variablesLabel->setWordWrap(true);
    m_variablesLabel->setVisible(!variableErrors.isEmpty());
    m_variablesLabel->setText(tr("Variable problems: %1").arg(variableErrors.join(QStringLiteral("; "))));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(m_isNew ? tr("Create") : tr("OK"));
    connect(buttons, &QDialogButtonBox::accepted, this, &AnalysisDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &AnalysisDialog::reject);

    auto *form = new QFormLayout;
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Sweep:"), m_sweep);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_pages);
    layout->addWidget(m_noise);
    layout->addSpacing(6);
    layout->addWidget(m_summaryLabel);
    layout->addWidget(m_variablesLabel);
    layout->addStretch();
    layout->addWidget(buttons);

    setSetup(initial);

    connect(m_name, &QLineEdit::textChanged, this, &AnalysisDialog::validate);
    connect(m_sweep, &QComboBox::currentIndexChanged, this, &AnalysisDialog::validate);
    connect(m_start, &QLineEdit::textChanged, this, &AnalysisDialog::validate);
    connect(m_stop, &QLineEdit::textChanged, this, &AnalysisDialog::validate);
    connect(m_points, &QLineEdit::textChanged, this, &AnalysisDialog::validate);
    connect(m_list, &QPlainTextEdit::textChanged, this, &AnalysisDialog::validate);
    resize(460, 340);
}

rf::LinearAnalysisSetup AnalysisDialog::setup() const
{
    rf::LinearAnalysisSetup setup;
    setup.id = m_id;
    setup.name = m_name->text().trimmed();
    setup.sweep = m_sweep->currentData().toString();
    setup.start = m_start->text().trimmed();
    setup.stop = m_stop->text().trimmed();
    setup.points = m_points->text().trimmed();
    setup.list = splitList(m_list->toPlainText());
    setup.noise = m_noise->isChecked();
    return setup;
}

void AnalysisDialog::setSetup(const rf::LinearAnalysisSetup &setup)
{
    m_name->setText(setup.name);
    m_sweep->setCurrentIndex(std::max(0, m_sweep->findData(setup.sweep)));
    m_start->setText(setup.start);
    m_stop->setText(setup.stop);
    m_points->setText(setup.points);
    m_list->setPlainText(setup.list.join(QLatin1Char('\n')));
    m_noise->setChecked(setup.noise);
    validate();
}

QString AnalysisDialog::summary() const
{
    return m_summary;
}

void AnalysisDialog::showValue(QLabel *label, const QString &text, const QString &unit, bool integer)
{
    QString error;
    const auto value = Expression::parse(text).evaluate(m_scope, &error);
    if (!value) {
        label->setText(error.isEmpty() ? tr("invalid") : error);
        label->setStyleSheet(errorStyle());
        label->setToolTip(error);
        return;
    }
    label->setStyleSheet(QString());
    label->setToolTip(QString());
    if (integer)
        label->setText(QStringLiteral("= %1").arg(QString::number(*value, 'g', 10)));
    else
        label->setText(QStringLiteral("= %1").arg(formatEngineering(*value, unit, 6)));
}

void AnalysisDialog::validate()
{
    const rf::LinearAnalysisSetup current = setup();
    const bool list = current.sweep == QLatin1String("list");
    m_pages->setCurrentIndex(list ? 1 : 0);
    if (!list) {
        showValue(m_startValue, current.start, QStringLiteral("Hz"), false);
        showValue(m_stopValue, current.stop, QStringLiteral("Hz"), false);
        showValue(m_pointsValue, current.points, QString(), true);
    } else {
        QStringList values;
        for (const QString &item : current.list) {
            QString error;
            const auto value = Expression::parse(item).evaluate(m_scope, &error);
            values.append(value ? formatEngineering(*value, QStringLiteral("Hz"), 6) : QStringLiteral("%1: %2").arg(item, error));
            if (values.size() == 6) {
                values.append(QStringLiteral("…"));
                break;
            }
        }
        m_listHint->setText(values.join(QStringLiteral(", ")));
    }

    QString error;
    const auto frequencies = rf::sweepFrequencies(current, m_scope, &error);
    m_valid = frequencies.has_value();
    if (m_valid) {
        const size_t n = frequencies->size();
        if (n == 1)
            m_summary = tr("1 point at %1").arg(formatEngineering(frequencies->front(), QStringLiteral("Hz"), 6));
        else
            m_summary = tr("%n points, %1 to %2", nullptr, static_cast<int>(n))
                            .arg(formatEngineering(frequencies->front(), QStringLiteral("Hz"), 6),
                                 formatEngineering(frequencies->back(), QStringLiteral("Hz"), 6));
        m_summaryLabel->setStyleSheet(QString());
    } else {
        m_summary = error;
        m_summaryLabel->setStyleSheet(errorStyle());
    }
    m_summaryLabel->setText(m_summary);
    m_ok->setEnabled(m_valid && m_document && m_document->schematic());
}

QString AnalysisDialog::store()
{
    validate();
    if (!m_valid || !m_document || !m_document->schematic())
        return {};
    Schematic *schematic = m_document->schematic();
    rf::LinearAnalysisSetup current = setup();
    QString id;
    QJsonObject simulation = rf::withLinearAnalysis(schematic->simulation(), current, &id);
    if (m_isNew && m_defaultPlots) {
        std::vector<int> ports;
        if (m_project)
            ports = sim::netlistPortNumbers(buildNetlist(*m_project, *m_document));
        for (const sim::PlotDefinition &plot : sim::defaultPlots(id, ports, current.noise))
            simulation = sim::withPlot(simulation, plot);
    }
    schematic->setSimulation(simulation);
    m_id = id;
    m_isNew = false;
    return id;
}

void AnalysisDialog::accept()
{
    if (store().isEmpty())
        return;
    QDialog::accept();
}

void AnalysisDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_ok)
        validate();
}

QString AnalysisDialog::errorStyle() const
{
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    return QStringLiteral("color: %1;").arg(dark ? QStringLiteral("#f08080") : QStringLiteral("#b02020"));
}

} // namespace lvd

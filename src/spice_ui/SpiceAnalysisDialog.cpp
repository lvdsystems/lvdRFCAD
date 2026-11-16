#include "spice_ui/SpiceAnalysisDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace lvd::spiceui {

using spice::AnalysisKind;

SpiceAnalysisDialog::SpiceAnalysisDialog(const spice::SpiceAnalysisSetup &setup, bool isNew, QWidget *parent)
    : QDialog(parent), m_original(setup)
{
    setWindowTitle(isNew ? tr("New SPICE Analysis") : tr("Edit SPICE Analysis"));
    auto *layout = new QVBoxLayout(this);

    auto *top = new QFormLayout;
    m_kind = new QComboBox(this);
    for (AnalysisKind kind : spice::allAnalysisKinds())
        m_kind->addItem(spice::analysisDisplayName(kind), int(kind));
    m_kind->setEnabled(isNew);
    top->addRow(tr("&Type:"), m_kind);
    m_name = new QLineEdit(setup.name, this);
    top->addRow(tr("&Name:"), m_name);
    layout->addLayout(top);

    m_pages = new QStackedWidget(this);
    for (AnalysisKind kind : spice::allAnalysisKinds()) {
        QWidget *page = buildPage(kind);
        m_pageOf.insert(kind, page);
        m_pages->addWidget(page);
    }
    layout->addWidget(m_pages);

    auto *common = new QGroupBox(tr("Conditions"), this);
    auto *commonForm = new QFormLayout(common);
    m_temperature = new QLineEdit(this);
    m_temperature->setPlaceholderText(tr("27 (SPICE default)"));
    m_temperature->setToolTip(tr("Circuit temperature in °C; an expression of the schematic variables."));
    commonForm->addRow(tr("&Temperature (°C):"), m_temperature);
    m_options = new QLineEdit(this);
    m_options->setPlaceholderText(tr("e.g. reltol=1e-4 gmin=1e-13"));
    m_options->setToolTip(tr("Simulator options, written to a .options card."));
    commonForm->addRow(tr("&Options:"), m_options);
    m_cards = new QPlainTextEdit(this);
    m_cards->setPlaceholderText(tr("Extra SPICE cards, e.g. .ic v(out)=0 or .nodeset"));
    m_cards->setMaximumHeight(70);
    commonForm->addRow(tr("Extra &cards:"), m_cards);
    layout->addWidget(common);

    m_error = new QLabel(this);
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color: palette(highlighted-text); background: #c62828; padding: 4px;"));
    m_error->hide();
    layout->addWidget(m_error);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SpiceAnalysisDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_kind, &QComboBox::currentIndexChanged, this, [this] {
        const AnalysisKind k = kind();
        m_pages->setCurrentWidget(m_pageOf.value(k));
        // A new analysis named after its type follows the type.
        bool defaultName = m_name->text().isEmpty();
        for (AnalysisKind other : spice::allAnalysisKinds())
            defaultName = defaultName || m_name->text() == spice::analysisDisplayName(other);
        if (defaultName)
            m_name->setText(spice::analysisDisplayName(k));
        m_error->hide();
    });

    // Every page starts with the defaults of its type, then the setup's
    // own values fill its type.
    for (AnalysisKind kind : spice::allAnalysisKinds())
        loadFields(spice::SpiceAnalysisSetup::create(kind));
    loadFields(setup);
    m_temperature->setText(setup.text(QStringLiteral("temperature")));
    m_options->setText(setup.text(QStringLiteral("options")));
    m_cards->setPlainText(setup.text(QStringLiteral("cards")));
    setKind(setup.kind);
    m_name->setText(setup.name);
}

void SpiceAnalysisDialog::addField(QFormLayout *form, AnalysisKind kind, const QString &key, const QString &label,
                                   Field::Type type, const QStringList &choices, const QString &toolTip)
{
    Field field;
    field.key = key;
    field.type = type;
    switch (type) {
    case Field::Type::Text:
    case Field::Type::List:
        field.editor = new QLineEdit(this);
        break;
    case Field::Type::Net:
    case Field::Type::Source: {
        auto *combo = new QComboBox(this);
        combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert);
        field.editor = combo;
        break;
    }
    case Field::Type::Choice: {
        auto *combo = new QComboBox(this);
        for (const QString &choice : choices)
            combo->addItem(choice, choice);
        field.editor = combo;
        break;
    }
    case Field::Type::Flag:
        field.editor = new QCheckBox(label, this);
        break;
    }
    field.editor->setObjectName(key);
    if (!toolTip.isEmpty())
        field.editor->setToolTip(toolTip);
    if (type == Field::Type::Flag)
        form->addRow(QString(), field.editor);
    else
        form->addRow(label, field.editor);
    m_fields[kind].append(field);
}

QWidget *SpiceAnalysisDialog::buildPage(AnalysisKind kind)
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    using T = Field::Type;
    const QStringList sweeps{QStringLiteral("dec"), QStringLiteral("oct"), QStringLiteral("lin")};
    const QString sweepTip = tr("dec: points per decade; oct: points per octave; lin: points in total.");
    const auto acSweep = [&] {
        addField(form, kind, QStringLiteral("sweep"), tr("Sweep:"), T::Choice, sweeps, sweepTip);
        addField(form, kind, QStringLiteral("points"), tr("Points:"), T::Text, {}, sweepTip);
        addField(form, kind, QStringLiteral("start"), tr("Start frequency:"), T::Text);
        addField(form, kind, QStringLiteral("stop"), tr("Stop frequency:"), T::Text);
    };
    const auto transient = [&] {
        addField(form, kind, QStringLiteral("stop"), tr("Stop time:"), T::Text);
        addField(form, kind, QStringLiteral("step"), tr("Print step:"), T::Text, {},
                 tr("Output interval; the simulator chooses its own internal steps."));
        addField(form, kind, QStringLiteral("start"), tr("Save from:"), T::Text, {},
                 tr("Results before this time are not kept (optional)."));
        addField(form, kind, QStringLiteral("maxStep"), tr("Maximum step:"), T::Text, {},
                 tr("Upper limit of the internal time step (optional)."));
        addField(form, kind, QStringLiteral("uic"), tr("Use initial conditions (skip the operating point)"), T::Flag);
    };
    switch (kind) {
    case AnalysisKind::OperatingPoint: {
        auto *note = new QLabel(tr("Node voltages, currents and powers at the DC operating point. "
                                   "They are also shown on the schematic."),
                                page);
        note->setWordWrap(true);
        form->addRow(note);
        break;
    }
    case AnalysisKind::DcSweep:
        addField(form, kind, QStringLiteral("source"), tr("Swept source:"), T::Source, {},
                 tr("A voltage or current source, or \"temp\" for the temperature."));
        addField(form, kind, QStringLiteral("start"), tr("Start:"), T::Text);
        addField(form, kind, QStringLiteral("stop"), tr("Stop:"), T::Text);
        addField(form, kind, QStringLiteral("step"), tr("Step:"), T::Text);
        addField(form, kind, QStringLiteral("source2"), tr("Second source:"), T::Source, {},
                 tr("Optional outer sweep: one curve per value."));
        addField(form, kind, QStringLiteral("start2"), tr("Second start:"), T::Text);
        addField(form, kind, QStringLiteral("stop2"), tr("Second stop:"), T::Text);
        addField(form, kind, QStringLiteral("step2"), tr("Second step:"), T::Text);
        break;
    case AnalysisKind::Ac:
        acSweep();
        break;
    case AnalysisKind::Transient:
        transient();
        addField(form, kind, QStringLiteral("saveCurrents"), tr("Save device currents and powers"), T::Flag);
        break;
    case AnalysisKind::Noise:
        addField(form, kind, QStringLiteral("output"), tr("Output:"), T::Net, {},
                 tr("A net, or v(a,b) for a differential output."));
        addField(form, kind, QStringLiteral("source"), tr("Input source:"), T::Source, {},
                 tr("Input-referred noise is given at this source."));
        acSweep();
        break;
    case AnalysisKind::Fourier:
        transient();
        addField(form, kind, QStringLiteral("fundamental"), tr("Fundamental:"), T::Text, {},
                 tr("Frequency of the fundamental; empty: measured from each signal."));
        addField(form, kind, QStringLiteral("harmonics"), tr("Harmonics:"), T::Text);
        addField(form, kind, QStringLiteral("periods"), tr("Periods analysed:"), T::Text, {},
                 tr("The last periods of the transient are analysed (default 1)."));
        addField(form, kind, QStringLiteral("signals"), tr("Signals:"), T::List, {},
                 tr("Trace expressions separated by commas, e.g. v(out), i(V1)."));
        break;
    case AnalysisKind::TransferFunction:
        addField(form, kind, QStringLiteral("output"), tr("Output:"), T::Net, {},
                 tr("A net, v(a,b), or i(V1) for the current of a source."));
        addField(form, kind, QStringLiteral("source"), tr("Input source:"), T::Source);
        break;
    case AnalysisKind::PoleZero:
        addField(form, kind, QStringLiteral("inputPositive"), tr("Input +:"), T::Net);
        addField(form, kind, QStringLiteral("inputNegative"), tr("Input −:"), T::Net);
        addField(form, kind, QStringLiteral("outputPositive"), tr("Output +:"), T::Net);
        addField(form, kind, QStringLiteral("outputNegative"), tr("Output −:"), T::Net);
        addField(form, kind, QStringLiteral("transfer"), tr("Transfer:"), T::Choice,
                 {QStringLiteral("voltage"), QStringLiteral("current")},
                 tr("voltage: Vout/Vin; current: Vout/Iin (transimpedance)."));
        addField(form, kind, QStringLiteral("find"), tr("Find:"), T::Choice,
                 {QStringLiteral("both"), QStringLiteral("poles"), QStringLiteral("zeros")});
        break;
    case AnalysisKind::Sensitivity:
        addField(form, kind, QStringLiteral("output"), tr("Output:"), T::Net, {},
                 tr("A net, v(a,b), or i(V1) for the current of a source."));
        addField(form, kind, QStringLiteral("ac"), tr("AC sensitivity over a frequency sweep"), T::Flag);
        acSweep();
        break;
    }
    return page;
}

void SpiceAnalysisDialog::loadFields(const spice::SpiceAnalysisSetup &setup)
{
    for (const Field &field : m_fields.value(setup.kind)) {
        if (!setup.data.contains(field.key))
            continue;
        switch (field.type) {
        case Field::Type::Text:
            static_cast<QLineEdit *>(field.editor)->setText(setup.text(field.key));
            break;
        case Field::Type::List:
            static_cast<QLineEdit *>(field.editor)->setText(setup.list(field.key).join(QStringLiteral(", ")));
            break;
        case Field::Type::Net:
        case Field::Type::Source:
            static_cast<QComboBox *>(field.editor)->setEditText(setup.text(field.key));
            break;
        case Field::Type::Choice: {
            auto *combo = static_cast<QComboBox *>(field.editor);
            const int index = combo->findData(setup.text(field.key));
            if (index >= 0)
                combo->setCurrentIndex(index);
            break;
        }
        case Field::Type::Flag:
            static_cast<QCheckBox *>(field.editor)->setChecked(setup.flag(field.key));
            break;
        }
    }
}

void SpiceAnalysisDialog::setNets(const QStringList &nets)
{
    m_nets = nets;
    refreshCombos();
}

void SpiceAnalysisDialog::setSources(const QStringList &references)
{
    m_sources = references;
    refreshCombos();
}

void SpiceAnalysisDialog::refreshCombos()
{
    for (const QList<Field> &fields : std::as_const(m_fields)) {
        for (const Field &field : fields) {
            if (field.type != Field::Type::Net && field.type != Field::Type::Source)
                continue;
            auto *combo = static_cast<QComboBox *>(field.editor);
            const QString text = combo->currentText();
            combo->clear();
            QStringList items = field.type == Field::Type::Net ? m_nets : m_sources;
            if (field.type == Field::Type::Source && field.key.startsWith(QLatin1String("source")))
                items << QStringLiteral("temp");
            combo->addItems(items);
            combo->setEditText(text);
        }
    }
}

void SpiceAnalysisDialog::setKind(AnalysisKind kind)
{
    const int index = m_kind->findData(int(kind));
    if (index >= 0)
        m_kind->setCurrentIndex(index);
    m_pages->setCurrentWidget(m_pageOf.value(kind));
}

AnalysisKind SpiceAnalysisDialog::kind() const
{
    return AnalysisKind(m_kind->currentData().toInt());
}

QWidget *SpiceAnalysisDialog::fieldEditor(const QString &key) const
{
    for (const Field &field : m_fields.value(kind()))
        if (field.key == key)
            return field.editor;
    return nullptr;
}

QString SpiceAnalysisDialog::errorText() const
{
    return m_error->isHidden() ? QString() : m_error->text();
}

spice::SpiceAnalysisSetup SpiceAnalysisDialog::setup() const
{
    spice::SpiceAnalysisSetup setup = m_original;
    const AnalysisKind k = kind();
    if (setup.kind != k) {
        // A different type: start from that type, keeping the id.
        const QString id = setup.id;
        setup = spice::SpiceAnalysisSetup::create(k);
        setup.id = id;
    }
    setup.name = m_name->text().trimmed().isEmpty() ? spice::analysisDisplayName(k) : m_name->text().trimmed();
    for (const Field &field : m_fields.value(k)) {
        switch (field.type) {
        case Field::Type::Text: {
            const QString text = static_cast<QLineEdit *>(field.editor)->text().trimmed();
            if (text.isEmpty())
                setup.data.remove(field.key);
            else
                setup.set(field.key, text);
            break;
        }
        case Field::Type::List: {
            QJsonArray list;
            for (const QString &item : static_cast<QLineEdit *>(field.editor)->text().split(QLatin1Char(',')))
                if (!item.trimmed().isEmpty())
                    list.append(item.trimmed());
            setup.set(field.key, list);
            break;
        }
        case Field::Type::Net:
        case Field::Type::Source: {
            const QString text = static_cast<QComboBox *>(field.editor)->currentText().trimmed();
            if (text.isEmpty())
                setup.data.remove(field.key);
            else
                setup.set(field.key, text);
            break;
        }
        case Field::Type::Choice:
            setup.set(field.key, static_cast<QComboBox *>(field.editor)->currentData().toString());
            break;
        case Field::Type::Flag:
            setup.set(field.key, static_cast<QCheckBox *>(field.editor)->isChecked());
            break;
        }
    }
    const auto putText = [&](const char *key, const QString &text) {
        if (text.trimmed().isEmpty())
            setup.data.remove(QLatin1String(key));
        else
            setup.set(QLatin1String(key), text.trimmed());
    };
    putText("temperature", m_temperature->text());
    putText("options", m_options->text());
    putText("cards", m_cards->toPlainText());
    return setup;
}

void SpiceAnalysisDialog::accept()
{
    if (m_validator) {
        const QStringList problems = m_validator(setup());
        if (!problems.isEmpty()) {
            m_error->setText(problems.join(QLatin1Char('\n')));
            m_error->show();
            return;
        }
    }
    QDialog::accept();
}

} // namespace lvd::spiceui

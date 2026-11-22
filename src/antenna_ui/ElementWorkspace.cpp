#include "antenna_ui/ElementWorkspace.h"

#include "antenna/AntennaLibrary.h"
#include "antenna/PatternFile.h"
#include "antenna_ui/AntennaUiCommon.h"
#include "fullwave/FwElement.h"
#include "fullwave/StlFile.h"
#include "fullwave/Templates.h"
#include "core/Document.h"
#include "core/Project.h"
#include "plot/PlotWidget.h"
#include "plot3d/Pattern3DView.h"

#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGroupBox>
#include <QSpinBox>
#include <QPointer>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QThread>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

namespace lvd::antui {

using namespace ant;

namespace {
constexpr double GHz = 1e9;
constexpr double mm = 1e-3;
} // namespace

ElementWorkspace::ElementWorkspace(Project *project, Document *document, AntennaLibraryManager *library, QWidget *parent)
    : QSplitter(Qt::Horizontal, parent), m_project(project), m_document(document), m_library(library)
{
    m_undo = new QUndoStack(this);
    m_undoAction = m_undo->createUndoAction(this, tr("&Undo"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = m_undo->createRedoAction(this, tr("&Redo"));
    m_redoAction->setShortcut(QKeySequence::Redo);

    m_content = document->content();
    m_design = ElementDesign::fromJson(m_content);
    if (m_content.isEmpty()) {
        m_content = m_design.toJson();
        m_writing = true;
        document->setContent(m_content);
        document->setModified(false);
        m_writing = false;
    }
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(150);
    connect(m_debounce, &QTimer::timeout, this, &ElementWorkspace::recompute);

    buildUi();
    designToForm();
    loadSimulationResults();
    connect(document, &Document::contentChanged, this, [this] {
        if (!m_writing && m_document)
            applyContent(m_document->content());
    });
    recompute();
}

QList<QAction *> ElementWorkspace::editActions() const
{
    return {m_undoAction, m_redoAction};
}

void ElementWorkspace::buildUi()
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(300);
    auto *panel = new QWidget;
    auto *f = new QFormLayout(panel);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scroll->setWidget(panel);
    addWidget(scroll);

    std::vector<std::pair<QString, QString>> models;
    for (ElementModelKind k : elementModelKinds())
        models.emplace_back(elementModelId(k), elementModelName(k));
    m_model = idCombo(models);
    std::vector<std::pair<QString, QString>> pols;
    for (Polarisation p : polarisations())
        pols.emplace_back(polarisationId(p), polarisationName(p));
    m_polarisation = idCombo(pols);
    m_frequency = doubleSpin(0.001, 1000, 4, QStringLiteral("GHz"), 0.1);
    m_efficiency = doubleSpin(-30, 0, 2, QStringLiteral("dB"), 0.1);
    m_exponent = doubleSpin(0, 30, 2, {}, 0.1);
    m_backLobe = doubleSpin(-100, 0, 1, QStringLiteral("dB"));
    m_length = doubleSpin(0, 100000, 3, QStringLiteral("mm"), 0.5);
    m_length->setSpecialValueText(tr("automatic"));
    m_height = doubleSpin(0, 100000, 3, QStringLiteral("mm"), 0.5);
    m_height->setSpecialValueText(tr("quarter wavelength"));
    m_patchW = doubleSpin(0, 100000, 3, QStringLiteral("mm"), 0.1);
    m_patchW->setSpecialValueText(tr("automatic"));
    m_patchL = doubleSpin(0, 100000, 3, QStringLiteral("mm"), 0.1);
    m_patchL->setSpecialValueText(tr("automatic"));
    m_substrateH = doubleSpin(0.001, 1000, 3, QStringLiteral("mm"), 0.1);
    m_permittivity = doubleSpin(1, 100, 3, {}, 0.1);
    m_apA = doubleSpin(0, 100000, 2, QStringLiteral("mm"), 1);
    m_apA->setSpecialValueText(tr("automatic"));
    m_apB = doubleSpin(0, 100000, 2, QStringLiteral("mm"), 1);
    m_apB->setSpecialValueText(tr("automatic"));
    m_distribution = idCombo({{QStringLiteral("cosine"), tr("Cosine (TE10)")}, {QStringLiteral("uniform"), tr("Uniform")}});
    m_patternFile = new QLineEdit;
    m_patternFile->setReadOnly(true);
    auto *importButton = new QPushButton(tr("Import Pattern File..."));
    auto *libraryButton = new QPushButton(tr("Save to Antenna Library..."));
    m_derived = noteLabel();
    m_notes = new QPlainTextEdit;
    m_notes->setMaximumHeight(90);

    f->addRow(tr("Model"), m_model);
    f->addRow(tr("Polarisation"), m_polarisation);
    f->addRow(tr("Design frequency"), m_frequency);
    f->addRow(tr("Efficiency"), m_efficiency);
    f->addRow(tr("cos^n exponent"), m_exponent);
    f->addRow(tr("Back radiation"), m_backLobe);
    f->addRow(tr("Length"), m_length);
    f->addRow(tr("Height over ground"), m_height);
    f->addRow(tr("Patch width"), m_patchW);
    f->addRow(tr("Patch length"), m_patchL);
    f->addRow(tr("Substrate height"), m_substrateH);
    f->addRow(tr("Permittivity"), m_permittivity);
    f->addRow(tr("Aperture a (y, broad)"), m_apA);
    f->addRow(tr("Aperture b (x)"), m_apB);
    f->addRow(tr("Distribution"), m_distribution);
    f->addRow(tr("Pattern file"), m_patternFile);
    f->addRow(QString(), importButton);
    buildFullWavePanel(f);
    f->addRow(m_derived);
    f->addRow(tr("Notes"), m_notes);
    f->addRow(QString(), libraryButton);

    int key = 1;
    auto edited = [this](int k) {
        return [this, k] {
            if (!m_updatingForm)
                commit(tr("Edit element"), k);
        };
    };
    for (QComboBox *c : {m_model, m_polarisation, m_distribution})
        connect(c, &QComboBox::currentIndexChanged, this, edited(-1));
    for (QDoubleSpinBox *s : {m_frequency, m_efficiency, m_exponent, m_backLobe, m_length, m_height, m_patchW, m_patchL,
                              m_substrateH, m_permittivity, m_apA, m_apB})
        connect(s, &QDoubleSpinBox::valueChanged, this, edited(key++));
    connect(m_notes, &QPlainTextEdit::textChanged, this, edited(1000));
    connect(importButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Pattern File"), m_project ? m_project->directory() : QString(),
                                                          tr("Pattern files (*.csv *.txt *.ffe *.dat);;All files (*)"));
        if (path.isEmpty())
            return;
        QString error;
        if (!importPattern(path, &error))
            QMessageBox::warning(this, tr("Import Pattern File"), error);
    });
    connect(libraryButton, &QPushButton::clicked, this, &ElementWorkspace::saveToLibrary);

    auto *right = new QWidget;
    auto *v = new QVBoxLayout(right);
    v->setContentsMargins(0, 0, 0, 0);
    auto *bar = new QHBoxLayout;
    std::vector<std::pair<QString, QString>> components;
    for (PatternComponent c : patternComponents())
        components.emplace_back(patternComponentId(c), patternComponentName(c));
    m_component = idCombo(components);
    m_copol = idCombo({{QStringLiteral("x"), tr("Reference x")}, {QStringLiteral("y"), tr("Reference y")}});
    m_range = doubleSpin(10, 120, 0, QStringLiteral("dB"), 5);
    m_range->setValue(40);
    m_plotFrequency = doubleSpin(0, 1000, 4, QStringLiteral("GHz"), 0.1);
    m_plotFrequency->setSpecialValueText(tr("design frequency"));
    m_polar = new QCheckBox(tr("Polar"));
    m_polar->setChecked(true);
    bar->addWidget(new QLabel(tr("Show")));
    bar->addWidget(m_component);
    bar->addWidget(m_copol);
    bar->addWidget(new QLabel(tr("Range")));
    bar->addWidget(m_range);
    bar->addWidget(new QLabel(tr("at")));
    bar->addWidget(m_plotFrequency);
    bar->addWidget(m_polar);
    bar->addStretch();
    v->addLayout(bar);
    auto *tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    m_tabs = tabs;
    m_cutPlot = new PlotWidget;
    m_view3d = new Pattern3DView;
    m_summary = new QTableWidget(0, 2);
    m_summary->setHorizontalHeaderLabels({tr("Quantity"), tr("Value")});
    m_summary->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_summary->horizontalHeader()->setStretchLastSection(true);
    m_summary->verticalHeader()->setVisible(false);
    m_summary->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tabs->addTab(m_cutPlot, tr("Cuts"));
    tabs->addTab(m_view3d, tr("3D"));
    m_geometryView = new Pattern3DView;
    m_portPlot = new PlotWidget;
    m_portPlot->setTitle(tr("Input match"));
    tabs->addTab(m_geometryView, tr("Geometry"));
    tabs->addTab(m_portPlot, tr("S11 / Zin"));
    tabs->addTab(m_summary, tr("Summary"));
    v->addWidget(tabs);
    addWidget(right);
    setStretchFactor(1, 1);
    setSizes({360, 870});

    connect(m_component, &QComboBox::currentIndexChanged, this, [this] { refreshViews(); });
    connect(m_copol, &QComboBox::currentIndexChanged, this, [this] { refreshViews(); });
    connect(m_range, &QDoubleSpinBox::valueChanged, this, [this] { refreshViews(); });
    connect(m_polar, &QCheckBox::toggled, this, [this] { refreshViews(); });
    connect(m_plotFrequency, &QDoubleSpinBox::valueChanged, this, [this] { m_debounce->start(); });
    connect(m_cutPlot, &PlotWidget::hoverReadoutChanged, this, &ElementWorkspace::statusMessage);
    connect(m_view3d, &Pattern3DView::hoverReadoutChanged, this, &ElementWorkspace::statusMessage);
}

void ElementWorkspace::designToForm()
{
    m_updatingForm = true;
    const ElementSpec &s = m_design.element;
    setComboId(m_model, elementModelId(s.model));
    setComboId(m_polarisation, polarisationId(s.polarisation));
    m_frequency->setValue(s.frequency / GHz);
    m_efficiency->setValue(s.efficiencyDb);
    m_exponent->setValue(s.exponent);
    m_backLobe->setValue(s.backLobeDb);
    m_length->setValue(s.length / mm);
    m_height->setValue(s.height / mm);
    m_patchW->setValue(s.patchWidth / mm);
    m_patchL->setValue(s.patchLength / mm);
    m_substrateH->setValue(s.substrateHeight / mm);
    m_permittivity->setValue(s.permittivity);
    m_apA->setValue(s.apertureA / mm);
    m_apB->setValue(s.apertureB / mm);
    setComboId(m_distribution, s.cosineDistribution ? QStringLiteral("cosine") : QStringLiteral("uniform"));
    m_patternFile->setText(s.patternFile);
    fullWaveToForm();
    if (m_notes->toPlainText() != m_design.notes)
        m_notes->setPlainText(m_design.notes);
    m_updatingForm = false;
    updateVisibility();
}

void ElementWorkspace::formToDesign()
{
    ElementSpec &s = m_design.element;
    for (ElementModelKind k : elementModelKinds())
        if (elementModelId(k) == comboId(m_model))
            s.model = k;
    for (Polarisation p : polarisations())
        if (polarisationId(p) == comboId(m_polarisation))
            s.polarisation = p;
    s.frequency = m_frequency->value() * GHz;
    s.efficiencyDb = m_efficiency->value();
    s.exponent = m_exponent->value();
    s.backLobeDb = m_backLobe->value();
    s.length = m_length->value() * mm;
    s.height = m_height->value() * mm;
    s.patchWidth = m_patchW->value() * mm;
    s.patchLength = m_patchL->value() * mm;
    s.substrateHeight = m_substrateH->value() * mm;
    s.permittivity = m_permittivity->value();
    s.apertureA = m_apA->value() * mm;
    s.apertureB = m_apB->value() * mm;
    s.cosineDistribution = comboId(m_distribution) == QLatin1String("cosine");
    s.patternFile = m_patternFile->text();
    formToFullWave();
    m_design.notes = m_notes->toPlainText();
}

void ElementWorkspace::updateVisibility()
{
    auto *f = qobject_cast<QFormLayout *>(m_model->parentWidget()->layout());
    const QString m = comboId(m_model);
    auto show = [&](QWidget *w, bool visible) { f->setRowVisible(w, visible); };
    show(m_polarisation, m != QLatin1String("monopole") && m != QLatin1String("imported"));
    show(m_exponent, m == QLatin1String("cosn"));
    show(m_backLobe, m == QLatin1String("cosn") || m == QLatin1String("patch"));
    show(m_length, m == QLatin1String("dipole") || m == QLatin1String("dipole-ground") || m == QLatin1String("monopole"));
    show(m_height, m == QLatin1String("dipole-ground"));
    for (QWidget *w : {static_cast<QWidget *>(m_patchW), static_cast<QWidget *>(m_patchL), static_cast<QWidget *>(m_substrateH),
                       static_cast<QWidget *>(m_permittivity)})
        show(w, m == QLatin1String("patch"));
    for (QWidget *w : {static_cast<QWidget *>(m_apA), static_cast<QWidget *>(m_apB), static_cast<QWidget *>(m_distribution)})
        show(w, m == QLatin1String("aperture"));
    show(m_patternFile, m == QLatin1String("imported") || m == QLatin1String("fullwave"));
    show(m_fwGroup, m == QLatin1String("fullwave"));
    show(m_efficiency, m != QLatin1String("fullwave"));
    show(m_polarisation, m != QLatin1String("monopole") && m != QLatin1String("imported") && m != QLatin1String("fullwave"));
}

void ElementWorkspace::commit(const QString &text, int mergeKey)
{
    if (!m_document)
        return;
    const QJsonObject before = m_content;
    formToDesign();
    const QJsonObject after = m_design.toJson();
    updateVisibility();
    if (after == before)
        return;
    m_content = after;
    m_writing = true;
    m_document->setContent(after);
    m_writing = false;
    m_undo->push(new ContentCommand(text, before, after, mergeKey, [this](const QJsonObject &c) { applyContent(c); }));
    m_debounce->start();
}

void ElementWorkspace::applyContent(const QJsonObject &content)
{
    m_content = content;
    m_design = ElementDesign::fromJson(content);
    if (m_document && m_document->content() != content) {
        m_writing = true;
        m_document->setContent(content);
        m_writing = false;
    }
    designToForm();
    m_debounce->start();
}

bool ElementWorkspace::importPattern(const QString &path, QString *error)
{
    const auto read = readPatternFile(path, error, m_design.element.frequency);
    if (!read)
        return false;
    QString target = path;
    if (m_project) {
        // Keep a copy in the project, in the application's own CSV format.
        QDir dir(m_project->directory());
        dir.mkpath(QStringLiteral("patterns"));
        target = dir.filePath(QStringLiteral("patterns/%1.csv").arg(QFileInfo(path).completeBaseName()));
        if (!writePatternCsv(read->table, target, error))
            return false;
        target = dir.relativeFilePath(target);
    }
    m_design.element.model = ElementModelKind::Imported;
    m_design.element.patternFile = target;
    if (read->table.frequencies.size() == 1 && read->table.frequencies.front() > 0)
        m_design.element.frequency = read->table.frequencies.front();
    designToForm();
    commit(tr("Import pattern"));
    Q_EMIT statusMessage(read->warnings.isEmpty() ? tr("Imported %1.").arg(QFileInfo(path).fileName())
                                                  : read->warnings.join(QStringLiteral("; ")));
    return true;
}

void ElementWorkspace::applyLibraryEntry(const QString &key, const AntennaLibraryEntry &entry)
{
    if (entry.kind != AntennaEntryKind::Element && entry.kind != AntennaEntryKind::Pattern)
        return;
    ElementSpec spec = ElementSpec::fromJson(entry.data);
    if (spec.model == ElementModelKind::Imported && m_library) {
        // The library's pattern file is relative to the library: make it absolute.
        if (const AntennaLibraryManager::Found found = m_library->find(key); found.library)
            spec.patternFile = QDir(found.library->directory()).absoluteFilePath(spec.patternFile);
    }
    m_design.element = spec;
    designToForm();
    commit(tr("Use library element"));
}

void ElementWorkspace::saveToLibrary()
{
    if (!m_library)
        return;
    formToDesign();
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save to Antenna Library"), tr("Entry name:"), QLineEdit::Normal,
                                               m_document ? m_document->name() : QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    QStringList libraries{QStringLiteral("my-antennas")};
    for (const AntennaLibrary &l : m_library->libraries())
        if (!libraries.contains(l.name))
            libraries << l.name;
    const QString libraryName =
        QInputDialog::getItem(this, tr("Save to Antenna Library"), tr("Library:"), libraries, 0, true, &ok);
    if (!ok || libraryName.isEmpty())
        return;
    AntennaLibrary library;
    if (const AntennaLibrary *existing = m_library->library(libraryName))
        library = *existing;
    else
        library.name = libraryName;
    AntennaLibraryEntry entry;
    entry.id = name.trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('-'));
    entry.name = name.trimmed();
    entry.description = m_design.notes.isEmpty() ? m_design.element.summary() : m_design.notes;
    ElementSpec spec = m_design.element;
    entry.kind = spec.model == ElementModelKind::Imported ? AntennaEntryKind::Pattern : AntennaEntryKind::Element;
    QString error;
    if (spec.model == ElementModelKind::Imported && m_results.pattern && m_results.pattern->spec().pattern) {
        // Pattern files travel with the library, next to it.
        const QString dir = library.filePath.isEmpty() ? QString() : library.directory();
        if (!dir.isEmpty()) {
            QDir(dir).mkpath(QStringLiteral("patterns"));
            const QString file = QStringLiteral("patterns/%1.csv").arg(entry.id);
            if (!writePatternCsv(*m_results.pattern->spec().pattern, QDir(dir).filePath(file), &error)) {
                QMessageBox::warning(this, tr("Save to Antenna Library"), error);
                return;
            }
            spec.patternFile = file;
        } else if (m_project) {
            spec.patternFile = QDir(m_project->directory()).absoluteFilePath(spec.patternFile);
        }
    }
    entry.data = spec.toJson();
    library.setEntry(entry);
    if (!m_library->saveLibrary(library, &error))
        QMessageBox::warning(this, tr("Save to Antenna Library"), error);
    else
        Q_EMIT statusMessage(tr("Saved %1 to the antenna library %2.").arg(entry.name, library.name));
}

void ElementWorkspace::recompute()
{
    ElementResults r;
    const ElementResolution resolved = elementFromSpec(m_design.element, m_project ? m_project->directory() : QString());
    r.warnings = resolved.warnings;
    if (!resolved.pattern) {
        r.error = resolved.error;
        m_results = r;
        refreshViews();
        return;
    }
    r.pattern = resolved.pattern;
    const double f = m_plotFrequency->value() > 0 ? m_plotFrequency->value() * GHz : m_design.element.frequency;
    const ArrayDesign single = [&] {
        ArrayDesign d;
        d.frequency = d.designFrequency = f;
        d.geometry.kind = LatticeKind::Linear;
        d.geometry.nx = 1;
        return d;
    }();
    const ArrayEvaluation e = evaluateArray(single, r.pattern, f);
    r.cutE = computePhiCut(*e.engine, 0, 0.5);
    r.cutH = computePhiCut(*e.engine, 90, 0.5);
    r.sphere = computeSphericalGrid(*e.engine, 2.0);
    double theta = 0, phi = 0;
    r.directivityDbi = r.pattern->peakDirectivityDbi(f, &theta, &phi);
    r.gainDbi = r.directivityDbi + m_design.element.efficiencyDb;
    r.peakThetaDeg = theta;
    r.peakPhiDeg = phi;
    auto width = [](const PatternCut &cut) {
        std::vector<double> db(cut.fields.size());
        std::size_t peak = 0;
        for (std::size_t i = 0; i < db.size(); ++i) {
            db[i] = powerDb(cut.fields[i].power());
            if (db[i] > db[peak])
                peak = i;
        }
        return analyseCut(cut.angle, db, peak).beamwidth;
    };
    r.beamwidthEDeg = width(r.cutE);
    r.beamwidthHDeg = width(r.cutH);
    const Vec3 dir = radialUnit(toRadians(theta), toRadians(phi));
    const double back = e.engine->fieldAt(-dir).power();
    r.frontToBackDb = r.directivityDbi + m_design.element.efficiencyDb - powerDb(back);
    m_results = r;

    if (m_design.element.model == ElementModelKind::Patch) {
        const ElementSpec::PatchSize p = m_design.element.patchSize();
        m_derived->setText(tr("Patch %1 x %2 mm (effective length %3 mm, effective permittivity %4).")
                               .arg(p.width / mm, 0, 'f', 2)
                               .arg(p.length / mm, 0, 'f', 2)
                               .arg(p.effectiveLength / mm, 0, 'f', 2)
                               .arg(p.effectivePermittivity, 0, 'f', 3));
    } else {
        m_derived->setText(m_design.element.summary());
    }
    refreshViews();
}

void ElementWorkspace::refreshViews()
{
    refreshGeometry();
    refreshPortPlot();
    const ElementResults &r = m_results;
    PatternComponent component = PatternComponent::Total;
    for (PatternComponent p : patternComponents())
        if (patternComponentId(p) == comboId(m_component))
            component = p;
    const CoPolarisation co = comboId(m_copol) == QLatin1String("y") ? CoPolarisation::Y : CoPolarisation::X;
    if (r.pattern) {
        plotCuts(m_cutPlot, {{&r.cutE, tr("phi = 0 (xz-plane)")}, {&r.cutH, tr("phi = 90 (yz-plane)")}}, component, co,
                 m_polar->isChecked(), m_range->value(), QStringLiteral("dBi"));
        m_cutPlot->setTitle(tr("Gain cuts (signed theta)"));
        show3d(m_view3d, r.sphere, component, co, m_range->value(), patternComponentName(component), QStringLiteral("dBi"));
    } else {
        m_cutPlot->clear();
        m_view3d->clearPattern();
    }
    QList<QPair<QString, QString>> rows;
    auto deg = [](double v) { return std::isfinite(v) ? QStringLiteral("%1 deg").arg(v, 0, 'f', 2) : QStringLiteral("-"); };
    if (!r.error.isEmpty())
        rows << qMakePair(tr("Error"), r.error);
    rows << qMakePair(tr("Model"), m_design.element.summary());
    rows << qMakePair(tr("Peak directivity"), formatDb(r.directivityDbi, QStringLiteral("dBi")));
    rows << qMakePair(tr("Peak gain"), formatDb(r.gainDbi, QStringLiteral("dBi")));
    rows << qMakePair(tr("Peak direction"), tr("theta %1, phi %2").arg(deg(r.peakThetaDeg), deg(r.peakPhiDeg)));
    rows << qMakePair(tr("Half-power beamwidth"), tr("%1 (phi = 0), %2 (phi = 90)").arg(deg(r.beamwidthEDeg), deg(r.beamwidthHDeg)));
    rows << qMakePair(tr("Front-to-back ratio"), formatDb(r.frontToBackDb, QStringLiteral("dB")));
    if (m_design.element.model == ElementModelKind::FullWave && m_fwResults) {
        const fw::FwResults &f = *m_fwResults;
        double bestDb = 0;
        for (const auto &v : f.port.s11)
            bestDb = std::min(bestDb, 20 * std::log10(std::max(std::abs(v), 1e-12)));
        rows << qMakePair(tr("Resonance (smallest S11)"), formatFrequency(f.resonance()));
        rows << qMakePair(tr("Smallest S11"), formatDb(bestDb, QStringLiteral("dB")));
        for (std::size_t k = 0; k < f.farFieldFrequencies.size(); ++k) {
            // The efficiency is undefined where the port accepts almost no power.
            const double eta = k < f.radiationEfficiency.size() ? f.radiationEfficiency[k] : NAN;
            rows << qMakePair(tr("At %1").arg(formatFrequency(f.farFieldFrequencies[k])),
                              tr("directivity %1, radiation efficiency %2")
                                  .arg(formatDb(k < f.directivityDbi.size() ? f.directivityDbi[k] : NAN, QStringLiteral("dBi")))
                                  .arg(std::isfinite(eta) && eta >= 0 && eta <= 1.05 ? QStringLiteral("%1 %").arg(100 * eta, 0, 'f', 1)
                                                                                     : tr("- (port mismatched)")));
        }
        rows << qMakePair(tr("Simulation"), tr("%1 cells, %2 time steps, %3 s").arg(f.cells).arg(f.timesteps).arg(f.seconds, 0, 'f', 1));
        for (const QString &w : f.warnings)
            rows << qMakePair(tr("Warning"), w);
    }
    for (const QString &w : r.warnings)
        rows << qMakePair(tr("Warning"), w);
    m_summary->setRowCount(int(rows.size()));
    for (int i = 0; i < int(rows.size()); ++i) {
        m_summary->setItem(i, 0, new QTableWidgetItem(rows[i].first));
        m_summary->setItem(i, 1, new QTableWidgetItem(rows[i].second));
    }
}

// ---- Full-wave

void ElementWorkspace::buildFullWavePanel(QFormLayout *form)
{
    m_fwGroup = new QGroupBox(tr("Full-wave model (openEMS)"));
    auto *g = new QFormLayout(m_fwGroup);
    g->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    std::vector<std::pair<QString, QString>> templates;
    for (const fw::TemplateInfo &t : fw::fullWaveTemplates())
        templates.emplace_back(t.id, t.name);
    m_fwTemplate = idCombo(templates);
    g->addRow(tr("Template"), m_fwTemplate);
    auto *params = new QWidget;
    m_fwParams = new QFormLayout(params);
    m_fwParams->setContentsMargins(0, 0, 0, 0);
    m_fwParams->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    g->addRow(params);

    // Imported STL bodies (template "stl").
    m_fwBodiesBox = new QWidget;
    auto *bodiesLayout = new QVBoxLayout(m_fwBodiesBox);
    bodiesLayout->setContentsMargins(0, 0, 0, 0);
    m_fwBodies = new QTableWidget(0, 5);
    m_fwBodies->setHorizontalHeaderLabels({tr("File"), tr("To mm"), tr("Metal"), tr("εr"), tr("tan δ")});
    m_fwBodies->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fwBodies->verticalHeader()->setVisible(false);
    m_fwBodies->setMinimumHeight(90);
    m_fwBodies->setMaximumHeight(160);
    auto *bodyButtons = new QHBoxLayout;
    auto *addBody = new QPushButton(tr("Add STL..."));
    auto *removeBody = new QPushButton(tr("Remove"));
    bodyButtons->addWidget(addBody);
    bodyButtons->addWidget(removeBody);
    bodyButtons->addStretch();
    bodiesLayout->addWidget(m_fwBodies);
    bodiesLayout->addLayout(bodyButtons);
    g->addRow(tr("Bodies"), m_fwBodiesBox);
    connect(addBody, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Add STL Body"), m_project ? m_project->directory() : QString(),
                                                          tr("STL files (*.stl);;All files (*)"));
        if (path.isEmpty())
            return;
        bool ok = false;
        const QStringList units = {tr("millimetres"), tr("metres"), tr("inches"), tr("micrometres")};
        const QString unit = QInputDialog::getItem(this, tr("Add STL Body"), tr("Units of the file:"), units, 0, false, &ok);
        if (!ok)
            return;
        const double scale = unit == units[1] ? 1000.0 : unit == units[2] ? 25.4 : unit == units[3] ? 1e-3 : 1.0;
        const bool metal = QMessageBox::question(this, tr("Add STL Body"), tr("Is the body metal? (No: a dielectric; set its "
                                                                               "permittivity in the table.)"))
                           == QMessageBox::Yes;
        QString error;
        if (!addStlBody(path, scale, metal, &error))
            QMessageBox::warning(this, tr("Add STL Body"), error);
    });
    connect(removeBody, &QPushButton::clicked, this, [this] {
        const int row = m_fwBodies->currentRow();
        if (row < 0)
            return;
        m_fwBodies->removeRow(row);
        commit(tr("Remove STL body"));
    });
    connect(m_fwBodies, &QTableWidget::itemChanged, this, [this] {
        if (!m_updatingForm)
            commit(tr("Edit STL body"));
    });

    // Embedded element pattern: the element in a small array.
    auto *arrayRow = new QHBoxLayout;
    m_fwNx = intSpin(1, 16);
    m_fwNy = intSpin(1, 16);
    m_fwDx = doubleSpin(0, 1e4, 1, QString(), 1);
    m_fwDy = doubleSpin(0, 1e4, 1, QString(), 1);
    arrayRow->addWidget(m_fwNx, 1);
    arrayRow->addWidget(new QLabel(QStringLiteral("×")));
    arrayRow->addWidget(m_fwNy, 1);
    g->addRow(tr("Embedded in an array"), arrayRow);
    // Let the pairs share the narrow panel.
    for (QAbstractSpinBox *spin : std::initializer_list<QAbstractSpinBox *>{m_fwNx, m_fwNy, m_fwDx, m_fwDy}) {
        spin->setMinimumWidth(40);
        spin->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    auto *spacingRow = new QHBoxLayout;
    spacingRow->addWidget(m_fwDx, 1);
    spacingRow->addWidget(new QLabel(QStringLiteral("×")));
    spacingRow->addWidget(m_fwDy, 1);
    g->addRow(tr("Array spacing (mm)"), spacingRow);
    for (QSpinBox *spin : {m_fwNx, m_fwNy})
        connect(spin, &QSpinBox::valueChanged, this, [this] {
            if (!m_updatingForm)
                commit(tr("Edit embedded array"));
        });
    for (QDoubleSpinBox *spin : {m_fwDx, m_fwDy})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] {
            if (!m_updatingForm)
                commit(tr("Edit embedded array"));
        });

    m_fwStart = doubleSpin(0.001, 1000, 4, QStringLiteral("GHz"), 0.1);
    m_fwStop = doubleSpin(0.001, 1000, 4, QStringLiteral("GHz"), 0.1);
    m_fwCells = doubleSpin(5, 200, 0, tr("per wavelength"), 5);
    m_fwEnd = doubleSpin(-100, -10, 0, QStringLiteral("dB"), 5);
    m_fwStep = doubleSpin(0.5, 15, 1, QStringLiteral("deg"), 0.5);
    m_fwBoundary = idCombo({{QStringLiteral("mur"), tr("Absorbing (Mur)")}, {QStringLiteral("pml"), tr("Perfectly matched layer (8 cells)")}});
    g->addRow(tr("Band start"), m_fwStart);
    g->addRow(tr("Band stop"), m_fwStop);
    g->addRow(tr("Mesh"), m_fwCells);
    g->addRow(tr("Stop when energy falls"), m_fwEnd);
    g->addRow(tr("Boundary"), m_fwBoundary);
    g->addRow(tr("Far-field step"), m_fwStep);
    auto *buttons = new QHBoxLayout;
    m_fwRun = new QPushButton(tr("Run Simulation"));
    m_fwAbort = new QPushButton(tr("Abort"));
    m_fwAbort->setEnabled(false);
    buttons->addWidget(m_fwRun);
    buttons->addWidget(m_fwAbort);
    g->addRow(buttons);
    m_fwProgress = new QProgressBar;
    m_fwProgress->setRange(0, 1000);
    m_fwProgress->setValue(0);
    // Long entries must not widen the panel.
    for (QComboBox *combo : {m_fwTemplate, m_fwBoundary}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(10);
    }
    m_fwStatus = noteLabel();
    g->addRow(m_fwProgress);
    g->addRow(m_fwStatus);
    form->addRow(m_fwGroup);

    connect(m_fwTemplate, &QComboBox::currentIndexChanged, this, [this] {
        if (m_updatingForm)
            return;
        // A new template starts from its own defaults and band.
        const fw::FwElementSetup setup = fw::FwElementSetup::forTemplate(comboId(m_fwTemplate));
        m_design.element.fullWave = setup.toJson();
        fullWaveToForm();
        commit(tr("Change full-wave template"));
    });
    int key = 2000;
    for (QDoubleSpinBox *spin : {m_fwStart, m_fwStop, m_fwCells, m_fwEnd, m_fwStep})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, k = key++] {
            if (!m_updatingForm)
                commit(tr("Edit full-wave settings"), k);
        });
    connect(m_fwBoundary, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_updatingForm)
            commit(tr("Edit full-wave settings"));
    });
    connect(m_fwRun, &QPushButton::clicked, this, [this] {
        QString error;
        if (!startSimulation(&error))
            QMessageBox::warning(this, tr("Run Simulation"), error);
    });
    connect(m_fwAbort, &QPushButton::clicked, this, &ElementWorkspace::abortSimulation);
}

void ElementWorkspace::rebuildParameterForm()
{
    while (m_fwParams->rowCount() > 0)
        m_fwParams->removeRow(0);
    m_fwParamSpins.clear();
    m_fwSubstrate = nullptr;
    const fw::TemplateInfo *info = fw::findTemplate(comboId(m_fwTemplate));
    if (!info)
        return;
    // Substrate picker: fills the permittivity, the loss tangent and the
    // height (nearest standard thickness) from libraries/substrates.json.
    if (fw::hasSubstrate(*info) && !SubstrateCatalog::builtIn().materials.isEmpty()) {
        m_fwSubstrate = new QComboBox;
        m_fwSubstrate->setObjectName(QStringLiteral("fullWaveSubstrate"));
        m_fwSubstrate->addItem(tr("Fill from a material..."));
        for (const SubstrateMaterial &material : SubstrateCatalog::builtIn().materials) {
            m_fwSubstrate->addItem(material.name, material.id);
            m_fwSubstrate->setItemData(m_fwSubstrate->count() - 1,
                                       tr("er %1, tan d %2 at %3 GHz. %4")
                                           .arg(material.er)
                                           .arg(material.tand)
                                           .arg(material.frequency / 1e9)
                                           .arg(material.source),
                                       Qt::ToolTipRole);
        }
        m_fwSubstrate->setToolTip(tr("Typical data-sheet values; verify them for your material lot"));
        m_fwSubstrate->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_fwParams->addRow(tr("Substrate"), m_fwSubstrate);
        connect(m_fwSubstrate, &QComboBox::activated, this, [this](int index) {
            if (index > 0)
                applySubstrateMaterial(m_fwSubstrate->itemData(index).toString());
        });
    }
    int key = 3000;
    for (const fw::TemplateParameter &p : info->parameters) {
        const bool length = p.unit == QLatin1String("mm");
        const int decimals = length ? 3 : 4;
        const QString suffix = p.unit == QLatin1String("ohm") ? QStringLiteral("Ω") : p.unit;
        auto *spin = doubleSpin(p.min, p.max, decimals, suffix, length ? 0.1 : 0.01);
        spin->setValue(p.value);
        if (!p.help.isEmpty())
            spin->setToolTip(p.help);
        // Wide ranges would size the box (and the panel) for "1000000.000 mm".
        spin->setMinimumWidth(90);
        spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_fwParams->addRow(p.label, spin);
        m_fwParamSpins.emplace_back(p.id, spin);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, k = key++] {
            if (!m_updatingForm)
                commit(tr("Edit full-wave parameter"), k);
        });
    }
}

bool ElementWorkspace::applySubstrateMaterial(const QString &materialId)
{
    const fw::TemplateInfo *info = fw::findTemplate(comboId(m_fwTemplate));
    const SubstrateMaterial *material = SubstrateCatalog::builtIn().material(materialId);
    if (!info || !material)
        return false;
    fw::ParameterValues values;
    for (const auto &[id, spin] : m_fwParamSpins)
        values[id] = spin->value();
    const QStringList changed = fw::applySubstrate(*info, values, *material);
    const bool wasUpdating = m_updatingForm;
    m_updatingForm = true;
    for (const auto &[id, spin] : m_fwParamSpins) {
        if (changed.contains(id))
            spin->setValue(values[id]);
    }
    m_updatingForm = wasUpdating;
    if (m_fwSubstrate) {
        const QSignalBlocker blocker(m_fwSubstrate);
        m_fwSubstrate->setCurrentIndex(0);
    }
    if (!changed.isEmpty())
        commit(tr("Choose substrate %1").arg(material->name));
    return true;
}

void ElementWorkspace::fullWaveToForm()
{
    if (!m_fwGroup)
        return;
    const bool wasUpdating = m_updatingForm;
    m_updatingForm = true;
    const fw::FwElementSetup setup = m_design.element.fullWave.isEmpty() ? fw::FwElementSetup::forTemplate(QStringLiteral("patch-probe"))
                                                                         : fw::FwElementSetup::fromJson(m_design.element.fullWave);
    if (comboId(m_fwTemplate) != setup.templateId || m_fwParamSpins.empty()) {
        setComboId(m_fwTemplate, setup.templateId);
        rebuildParameterForm();
    }
    for (auto &[id, spin] : m_fwParamSpins) {
        const auto it = setup.values.find(id);
        if (it != setup.values.end())
            spin->setValue(it->second);
    }
    m_fwStart->setValue(setup.settings.fStart / GHz);
    m_fwStop->setValue(setup.settings.fStop / GHz);
    m_fwCells->setValue(setup.settings.cellsPerWavelength);
    m_fwEnd->setValue(setup.settings.endCriteriaDb);
    m_fwStep->setValue(setup.settings.farFieldStepDeg);
    setComboId(m_fwBoundary, setup.settings.boundary == fw::Boundary::Pml8 ? QStringLiteral("pml") : QStringLiteral("mur"));
    m_fwNx->setValue(setup.arrayNx);
    m_fwNy->setValue(setup.arrayNy);
    m_fwDx->setValue(setup.arrayDx);
    m_fwDy->setValue(setup.arrayDy);
    m_fwBodies->setRowCount(int(setup.bodies.size()));
    for (int i = 0; i < int(setup.bodies.size()); ++i) {
        const fw::FwStlBody &b = setup.bodies[std::size_t(i)];
        auto *file = new QTableWidgetItem(b.file);
        file->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_fwBodies->setItem(i, 0, file);
        m_fwBodies->setItem(i, 1, new QTableWidgetItem(QString::number(b.scale, 'g', 6)));
        auto *metal = new QTableWidgetItem;
        metal->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        metal->setCheckState(b.metal ? Qt::Checked : Qt::Unchecked);
        m_fwBodies->setItem(i, 2, metal);
        m_fwBodies->setItem(i, 3, new QTableWidgetItem(QString::number(b.epsR, 'g', 6)));
        m_fwBodies->setItem(i, 4, new QTableWidgetItem(QString::number(b.tanD, 'g', 6)));
    }
    if (auto *form = qobject_cast<QFormLayout *>(m_fwGroup->layout()))
        form->setRowVisible(m_fwBodiesBox, setup.templateId == QLatin1String("stl"));
    m_updatingForm = wasUpdating;
}

void ElementWorkspace::formToFullWave()
{
    ElementSpec &s = m_design.element;
    if (s.model != ElementModelKind::FullWave || !m_fwGroup)
        return;
    fw::FwElementSetup setup = s.fullWave.isEmpty() ? fw::FwElementSetup::forTemplate(comboId(m_fwTemplate))
                                                    : fw::FwElementSetup::fromJson(s.fullWave);
    setup.templateId = comboId(m_fwTemplate);
    for (const auto &[id, spin] : m_fwParamSpins)
        setup.values[id] = spin->value();
    setup.settings.fStart = m_fwStart->value() * GHz;
    setup.settings.fStop = std::max(m_fwStop->value(), m_fwStart->value() * 1.01) * GHz;
    setup.settings.cellsPerWavelength = m_fwCells->value();
    setup.settings.endCriteriaDb = m_fwEnd->value();
    setup.settings.farFieldStepDeg = m_fwStep->value();
    setup.settings.boundary = comboId(m_fwBoundary) == QLatin1String("pml") ? fw::Boundary::Pml8 : fw::Boundary::Mur;
    setup.arrayNx = m_fwNx->value();
    setup.arrayNy = m_fwNy->value();
    setup.arrayDx = m_fwDx->value();
    setup.arrayDy = m_fwDy->value();
    std::vector<fw::FwStlBody> bodies;
    for (int i = 0; i < m_fwBodies->rowCount(); ++i) {
        fw::FwStlBody b;
        b.file = m_fwBodies->item(i, 0) ? m_fwBodies->item(i, 0)->text() : QString();
        b.scale = m_fwBodies->item(i, 1) ? m_fwBodies->item(i, 1)->text().toDouble() : 1.0;
        if (b.scale <= 0)
            b.scale = 1;
        b.metal = m_fwBodies->item(i, 2) && m_fwBodies->item(i, 2)->checkState() == Qt::Checked;
        b.epsR = std::max(1.0, m_fwBodies->item(i, 3) ? m_fwBodies->item(i, 3)->text().toDouble() : 1.0);
        b.tanD = std::max(0.0, m_fwBodies->item(i, 4) ? m_fwBodies->item(i, 4)->text().toDouble() : 0.0);
        if (!b.file.isEmpty())
            bodies.push_back(b);
    }
    setup.bodies = bodies;
    s.fullWave = setup.toJson();
    // Until a run gives the resonance, the design frequency is the band centre.
    if (s.patternFile.isEmpty())
        s.frequency = setup.settings.centre();
}

QString ElementWorkspace::simulationFolder() const
{
    return m_project && m_document ? fw::simulationFolder(m_project->directory(), m_document->name()) : QString();
}

bool ElementWorkspace::addStlBody(const QString &path, double scale, bool metal, QString *error)
{
    if (!m_project) {
        if (error)
            *error = tr("The element must belong to a project.");
        return false;
    }
    QString readError;
    if (!fw::readStl(path, scale, &readError)) {
        if (error)
            *error = readError;
        return false;
    }
    QDir dir(m_project->directory());
    dir.mkpath(QStringLiteral("cad"));
    const QString target = dir.filePath(QStringLiteral("cad/") + QFileInfo(path).fileName());
    if (QFileInfo(path).absoluteFilePath() != QFileInfo(target).absoluteFilePath()) {
        QFile::remove(target);
        if (!QFile::copy(path, target)) {
            if (error)
                *error = tr("Cannot copy %1 into the project.").arg(path);
            return false;
        }
    }
    fw::FwElementSetup setup = m_design.element.fullWave.isEmpty() ? fw::FwElementSetup::forTemplate(QStringLiteral("stl"))
                                                                   : fw::FwElementSetup::fromJson(m_design.element.fullWave);
    if (setup.templateId != QLatin1String("stl")) {
        const fw::FwElementSetup fresh = fw::FwElementSetup::forTemplate(QStringLiteral("stl"));
        setup.templateId = fresh.templateId;
        setup.values = fresh.values;
    }
    fw::FwStlBody body;
    body.file = dir.relativeFilePath(target);
    body.scale = scale;
    body.metal = metal;
    setup.bodies.push_back(body);
    m_design.element.model = ElementModelKind::FullWave;
    m_design.element.fullWave = setup.toJson();
    designToForm();
    commit(tr("Add STL body"));
    return true;
}

void ElementWorkspace::loadSimulationResults()
{
    m_fwResults.reset();
    const QString folder = simulationFolder();
    if (!folder.isEmpty() && m_design.element.model == ElementModelKind::FullWave)
        if (auto r = fw::loadResults(folder))
            m_fwResults = std::move(*r);
}

bool ElementWorkspace::startSimulation(QString *error)
{
    if (m_simulating)
        return false;
    if (!m_project || !m_document) {
        if (error)
            *error = tr("The element must belong to a project.");
        return false;
    }
    if (!fw::openEmsAvailable()) {
        if (error)
            *error = tr("openEMS was not found in %1.").arg(fw::openEmsDirectory());
        return false;
    }
    if (m_design.element.model != ElementModelKind::FullWave) {
        m_design.element.model = ElementModelKind::FullWave;
        designToForm();
        commit(tr("Use the full-wave model"));
    }
    formToDesign();
    const fw::FwElementSetup setup = fw::FwElementSetup::fromJson(m_design.element.fullWave);
    QString buildError;
    const fw::FwModel model = fw::buildModel(setup, m_project->directory(), &buildError);
    if (!buildError.isEmpty()) {
        if (error)
            *error = buildError;
        return false;
    }
    const QString folder = simulationFolder();
    m_fwCancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_fwCancel;
    m_simulating = true;
    m_fwRun->setEnabled(false);
    m_fwAbort->setEnabled(true);
    m_fwProgress->setValue(0);
    m_fwStatus->setText(tr("Starting openEMS..."));
    QPointer<ElementWorkspace> self(this);
    auto results = std::make_shared<fw::FwResults>();
    QThread *thread = QThread::create([model, settings = setup.settings, folder, cancel, results, self] {
        *results = fw::runFullWave(model, settings, folder, cancel.get(), [self](double fraction, const QString &message) {
            QMetaObject::invokeMethod(
                self.data(),
                [self, fraction, message] {
                    if (!self)
                        return;
                    self->m_fwProgress->setValue(int(fraction * 1000));
                    self->m_fwStatus->setText(message.left(160));
                },
                Qt::QueuedConnection);
        });
    });
    connect(thread, &QThread::finished, this, [this, thread, results] {
        thread->deleteLater();
        onSimulationFinished(*results);
    });
    thread->setParent(this);
    thread->start();
    return true;
}

void ElementWorkspace::showView(const QString &key)
{
    const QStringList keys = {QStringLiteral("cuts"), QStringLiteral("3d"), QStringLiteral("geometry"), QStringLiteral("port"),
                              QStringLiteral("summary")};
    const int index = int(keys.indexOf(key));
    if (index >= 0 && index < m_tabs->count())
        m_tabs->setCurrentIndex(index);
}

void ElementWorkspace::abortSimulation()
{
    if (m_fwCancel)
        *m_fwCancel = true;
}

bool ElementWorkspace::waitForSimulation(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (m_simulating && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    return !m_simulating;
}

void ElementWorkspace::onSimulationFinished(const fw::FwResults &results)
{
    m_simulating = false;
    m_fwRun->setEnabled(true);
    m_fwAbort->setEnabled(false);
    if (!results.ok) {
        m_fwStatus->setText(results.cancelled ? tr("Simulation aborted.") : tr("Simulation failed: %1").arg(results.error));
        m_fwProgress->setValue(0);
        Q_EMIT statusMessage(m_fwStatus->text());
        Q_EMIT simulationFinished(false);
        return;
    }
    m_fwResults = results;
    m_fwProgress->setValue(1000);
    m_fwStatus->setText(tr("Done in %1 s: resonance %2.").arg(results.seconds, 0, 'f', 1).arg(formatFrequency(results.resonance())));
    // The element now uses the simulated pattern, with the radiation
    // efficiency at the far-field frequency nearest the resonance.
    fw::applyResults(m_design.element, results, m_document->name());
    designToForm();
    commit(tr("Full-wave simulation"));
    recompute();
    Q_EMIT statusMessage(m_fwStatus->text());
    Q_EMIT simulationFinished(true);
}

void ElementWorkspace::refreshGeometry()
{
    if (!m_geometryView)
        return;
    m_geometryView->clearGeometry();
    m_geometryView->clearPattern();
    if (m_design.element.model != ElementModelKind::FullWave)
        return;
    const fw::FwElementSetup setup = fw::FwElementSetup::fromJson(m_design.element.fullWave);
    QString error;
    const fw::FwModel model = fw::buildModel(setup, m_project ? m_project->directory() : QString(), &error);
    for (const fw::FwDisplayMesh &d : fw::displayMeshes(model)) {
        Scene3DMesh mesh;
        for (const Vec3 &v : d.vertices)
            mesh.vertices.emplace_back(float(v.x() * 1e-3), float(v.y() * 1e-3), float(v.z() * 1e-3));
        mesh.indices.assign(d.indices.begin(), d.indices.end());
        mesh.color = d.color.isValid() ? d.color : QColor(160, 160, 160);
        mesh.opacity = d.metal ? 1.0f : 0.35f;
        m_geometryView->addMesh(mesh);
    }
}

void ElementWorkspace::refreshPortPlot()
{
    if (!m_portPlot)
        return;
    m_portPlot->clear();
    if (!m_fwResults || m_design.element.model != ElementModelKind::FullWave)
        return;
    const fw::PortResult &p = m_fwResults->port;
    std::vector<double> s11, zr, zi;
    for (std::size_t k = 0; k < p.frequency.size() && k < p.s11.size(); ++k) {
        s11.push_back(20 * std::log10(std::max(std::abs(p.s11[k]), 1e-12)));
        zr.push_back(p.zin[k].real());
        zi.push_back(p.zin[k].imag());
    }
    m_portPlot->setXAxis(tr("Frequency"), QStringLiteral("Hz"));
    m_portPlot->setYAxis(tr("S11"), QStringLiteral("dB"));
    m_portPlot->setYAxis(tr("Input impedance"), QStringLiteral("Ω"), false, PlotYAxis::Right);
    m_portPlot->setTraceData(m_portPlot->addTrace(tr("S11"), QStringLiteral("dB")), p.frequency, s11);
    TraceStyle right;
    right.yAxis = PlotYAxis::Right;
    m_portPlot->setTraceData(m_portPlot->addTrace(tr("Re Zin"), QStringLiteral("Ω"), right), p.frequency, zr);
    right.lineStyle = Qt::DashLine;
    m_portPlot->setTraceData(m_portPlot->addTrace(tr("Im Zin"), QStringLiteral("Ω"), right), p.frequency, zi);
    m_portPlot->autoscale();
}

} // namespace lvd::antui

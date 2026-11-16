#include "antenna_ui/ElementWorkspace.h"

#include "antenna/AntennaLibrary.h"
#include "antenna/PatternFile.h"
#include "antenna_ui/AntennaUiCommon.h"
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
#include <QPlainTextEdit>
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
    tabs->addTab(m_summary, tr("Summary"));
    v->addWidget(tabs);
    addWidget(right);
    setStretchFactor(1, 1);
    setSizes({330, 900});

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
    show(m_patternFile, m == QLatin1String("imported"));
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
    for (const QString &w : r.warnings)
        rows << qMakePair(tr("Warning"), w);
    m_summary->setRowCount(int(rows.size()));
    for (int i = 0; i < int(rows.size()); ++i) {
        m_summary->setItem(i, 0, new QTableWidgetItem(rows[i].first));
        m_summary->setItem(i, 1, new QTableWidgetItem(rows[i].second));
    }
}

} // namespace lvd::antui

#include "antenna_ui/ArrayWorkspace.h"

#include "antenna/AntennaLibrary.h"
#include "antenna_ui/AntennaUiCommon.h"
#include "antenna_ui/PositionEditor.h"
#include "core/Document.h"
#include "core/Project.h"
#include "plot/ColorMapPlot.h"
#include "plot/PlotWidget.h"
#include "plot3d/Pattern3DView.h"
#include "rfsim/Budget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolBox>
#include <QUndoStack>
#include <QVBoxLayout>

namespace lvd::antui {

using namespace ant;

namespace {

constexpr double GHz = 1e9;

QWidget *page(QFormLayout **form)
{
    auto *w = new QWidget;
    *form = new QFormLayout(w);
    (*form)->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    (*form)->setLabelAlignment(Qt::AlignLeft);
    return w;
}

void setRowVisible(QFormLayout *form, QWidget *field, bool visible)
{
    form->setRowVisible(field, visible);
}

std::vector<std::pair<QString, QString>> taperItems()
{
    std::vector<std::pair<QString, QString>> items;
    for (TaperKind k : taperKinds())
        items.emplace_back(taperId(k), taperName(k));
    return items;
}

QTableWidget *directionTable(const QStringList &headers)
{
    auto *t = new QTableWidget(0, int(headers.size()));
    t->setHorizontalHeaderLabels(headers);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    t->verticalHeader()->setVisible(false);
    t->setMinimumHeight(90);
    t->setMaximumHeight(140);
    return t;
}

double cell(const QTableWidget *t, int row, int col)
{
    const QTableWidgetItem *item = t->item(row, col);
    return item ? item->text().toDouble() : 0.0;
}

void setCell(QTableWidget *t, int row, int col, double v)
{
    t->setItem(row, col, new QTableWidgetItem(QString::number(v, 'g', 6)));
}

std::shared_ptr<ArrayResults> compute(std::shared_ptr<ArrayResults> r, std::shared_ptr<const ElementPattern> element,
                                      CoPolarisation co, const std::atomic_bool *cancel)
{
    QElapsedTimer timer;
    timer.start();
    const ArrayDesign &d = r->design;
    r->evaluation = evaluateArray(d, element);
    r->warnings << r->evaluation.warnings;
    const auto engine = r->evaluation.engine;
    if (!engine || engine->weightPower() <= 0) {
        r->error = engine ? QObject::tr("All elements are off.") : QObject::tr("No element pattern.");
        return r;
    }
    const std::size_t n = engine->elementCount();
    const double bwDeg = toDegrees(engine->estimatedBeamwidth());
    const double step = std::clamp(bwDeg / 20, 0.02, 0.5);
    r->cutX = computePhiCut(*engine, 0, step, cancel);
    r->cutY = computePhiCut(*engine, 90, step, cancel);
    if (r->evaluation.idealEngine && r->evaluation.idealEngine != engine) {
        r->idealX = computePhiCut(*r->evaluation.idealEngine, 0, step, cancel);
        r->idealY = computePhiCut(*r->evaluation.idealEngine, 90, step, cancel);
    }
    r->horizon = computeThetaCut(*engine, 90, std::min(step, 0.25), cancel);
    if (r->evaluation.idealEngine && r->evaluation.idealEngine != engine)
        r->idealHorizon = computeThetaCut(*r->evaluation.idealEngine, 90, std::min(step, 0.25), cancel);
    r->uv = computeUvGrid(*engine, n > 2000 ? 151 : 201, cancel);
    r->sphere = computeSphericalGrid(*engine, std::clamp(bwDeg / 3, 1.0, 3.0), cancel);
    MetricsOptions options;
    options.steerThetaDeg = d.excitation.steerThetaDeg;
    options.steerPhiDeg = d.excitation.steerPhiDeg;
    options.co = co;
    options.uv = &r->uv;
    options.integrate = n <= 4096;
    r->metrics = computeMetrics(*engine, options, cancel);
    if (r->evaluation.idealEngine && r->evaluation.idealEngine != engine) {
        const Vec3 peak = radialUnit(toRadians(r->metrics.peakThetaDeg), toRadians(r->metrics.peakPhiDeg));
        const Vec3 along = (radialUnit(toRadians(r->metrics.cut1.theta.back()), toRadians(r->metrics.cut1.phi.back())));
        const Vec3 across = (radialUnit(toRadians(r->metrics.cut2.theta.back()), toRadians(r->metrics.cut2.phi.back())));
        r->idealSteer = computeGreatCircleCut(*r->evaluation.idealEngine, peak, along, 90, step, cancel);
        r->cutSteer = computeGreatCircleCut(*r->evaluation.idealEngine, peak, across, 90, step, cancel);
    }
    r->scan = scanSweep(d, element, r->tx, r->rx, cancel);
    r->band = bandSweep(d, element, cancel);
    r->budget = computeArrayBudget(r->evaluation, d, r->tx, r->rx);
    if (r->evaluation.layout.lattice && d.geometry.kind != LatticeKind::Cylinder) {
        r->lobes = gratingLobes(*r->evaluation.layout.lattice, d.frequency, 3);
        r->gratingFreeScan0 = gratingLobeFreeScan(*r->evaluation.layout.lattice, d.frequency, 0);
        r->gratingFreeScan90 = gratingLobeFreeScan(*r->evaluation.layout.lattice, d.frequency, 90);
    }
    r->cancelled = cancel && cancel->load();
    r->seconds = timer.elapsed() / 1000.0;
    return r;
}

} // namespace

ArrayWorkspace::ArrayWorkspace(Project *project, Document *document, AntennaLibraryManager *library, QWidget *parent)
    : QSplitter(Qt::Horizontal, parent), m_project(project), m_document(document), m_library(library)
{
    m_undo = new QUndoStack(this);
    m_undoAction = m_undo->createUndoAction(this, tr("&Undo"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = m_undo->createRedoAction(this, tr("&Redo"));
    m_redoAction->setShortcut(QKeySequence::Redo);

    m_content = document->content();
    m_design = ArrayDesign::fromJson(m_content);
    if (m_content.isEmpty()) {
        m_content = m_design.toJson();
        m_writing = true;
        document->setContent(m_content);
        document->setModified(false);
        m_writing = false;
    }

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, &ArrayWorkspace::startCompute);

    buildPanel();
    buildViews();
    refreshElementChoices();
    refreshTrChoices();
    designToForm();

    connect(document, &Document::contentChanged, this, [this] {
        if (!m_writing && m_document)
            applyContent(m_document->content());
    });
    if (m_library)
        connect(m_library, &AntennaLibraryManager::librariesChanged, this, [this] {
            refreshElementChoices();
            refreshTrChoices();
            designToForm();
        });
    if (m_project) {
        connect(m_project, &Project::documentAdded, this, [this] {
            refreshElementChoices();
            refreshTrChoices();
            designToForm();
        });
    }
    setStretchFactor(0, 0);
    setStretchFactor(1, 1);
    setSizes({360, 900});
    scheduleCompute();
}

ArrayWorkspace::~ArrayWorkspace()
{
    if (m_cancel)
        *m_cancel = true;
    // Results arrive through queued calls to this object; wait for the
    // worker so it never outlives the data it was given.
    for (QThread *t : findChildren<QThread *>())
        t->wait();
}

QList<QAction *> ArrayWorkspace::editActions() const
{
    return {m_undoAction, m_redoAction};
}

// ---- Panel

void ArrayWorkspace::buildPanel()
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(320);
    m_toolBox = new QToolBox;
    scroll->setWidget(m_toolBox);
    addWidget(scroll);

    int key = 1;
    auto edited = [this](int k) {
        return [this, k] {
            if (!m_updatingForm)
                commit(tr("Edit array"), k);
        };
    };
    auto watchSpin = [&](QDoubleSpinBox *s) { connect(s, &QDoubleSpinBox::valueChanged, this, edited(key++)); };
    auto watchInt = [&](QSpinBox *s) { connect(s, &QSpinBox::valueChanged, this, edited(key++)); };
    auto watchCombo = [&](QComboBox *c) { connect(c, &QComboBox::currentIndexChanged, this, edited(-1)); };
    auto watchCheck = [&](QCheckBox *c) { connect(c, &QCheckBox::toggled, this, edited(-1)); };
    auto watchLine = [&](QLineEdit *l) { connect(l, &QLineEdit::editingFinished, this, edited(-1)); };

    // Frequency and element
    QFormLayout *f;
    QWidget *p = page(&f);
    m_frequency = doubleSpin(0.001, 1000, 4, QStringLiteral("GHz"), 0.1);
    m_designFrequency = doubleSpin(0.001, 1000, 4, QStringLiteral("GHz"), 0.1);
    f->addRow(tr("Operating frequency"), m_frequency);
    f->addRow(tr("Design frequency"), m_designFrequency);
    m_elementSource = new QComboBox;
    f->addRow(tr("Element"), m_elementSource);
    std::vector<std::pair<QString, QString>> models;
    for (ElementModelKind k : elementModelKinds())
        if (k != ElementModelKind::Imported)
            models.emplace_back(elementModelId(k), elementModelName(k));
    m_elementModel = idCombo(models);
    std::vector<std::pair<QString, QString>> pols;
    for (Polarisation pol : polarisations())
        pols.emplace_back(polarisationId(pol), polarisationName(pol));
    m_elementPolarisation = idCombo(pols);
    m_elementExponent = doubleSpin(0, 20, 2, {}, 0.1);
    m_elementEfficiency = doubleSpin(-20, 0, 2, QStringLiteral("dB"), 0.1);
    f->addRow(tr("Model"), m_elementModel);
    f->addRow(tr("Polarisation"), m_elementPolarisation);
    f->addRow(tr("cos^n exponent"), m_elementExponent);
    f->addRow(tr("Efficiency"), m_elementEfficiency);
    m_elementNote = noteLabel();
    f->addRow(m_elementNote);
    f->addRow(noteLabel(tr("Element documents and the antenna library offer every model, imported patterns "
                           "and full parameters.")));
    m_toolBox->addItem(p, tr("Frequency and element"));
    watchSpin(m_frequency);
    watchSpin(m_designFrequency);
    watchCombo(m_elementSource);
    watchCombo(m_elementModel);
    watchCombo(m_elementPolarisation);
    watchSpin(m_elementExponent);
    watchSpin(m_elementEfficiency);

    // Geometry
    p = page(&f);
    std::vector<std::pair<QString, QString>> lattices;
    for (LatticeKind k : latticeKinds())
        lattices.emplace_back(latticeId(k), latticeName(k));
    m_lattice = idCombo(lattices);
    m_unit = idCombo({{QStringLiteral("lambda"), tr("wavelengths")}, {QStringLiteral("m"), tr("metres")}});
    m_nx = intSpin(1, 4096);
    m_ny = intSpin(1, 4096);
    m_dx = doubleSpin(0.001, 1000, 4, {}, 0.05);
    m_dy = doubleSpin(0.001, 1000, 4, {}, 0.05);
    m_aperture = idCombo({{QStringLiteral("none"), tr("Full lattice")},
                          {QStringLiteral("circle"), tr("Circle")},
                          {QStringLiteral("ellipse"), tr("Ellipse")}});
    m_apertureX = doubleSpin(0, 10000, 3, {}, 0.5);
    m_apertureY = doubleSpin(0, 10000, 3, {}, 0.5);
    m_rings = new QLineEdit;
    m_rings->setPlaceholderText(tr("radius:count[:rotation], ..."));
    m_centreElement = new QCheckBox(tr("Element at the centre"));
    m_cylRadius = doubleSpin(0.001, 10000, 3, {}, 0.5);
    m_columns = intSpin(1, 4096);
    m_rows = intSpin(1, 4096);
    m_arc = doubleSpin(1, 360, 1, QStringLiteral("deg"), 5);
    m_dz = doubleSpin(0.001, 1000, 4, {}, 0.05);
    m_importElements = new QPushButton(tr("Edit Positions..."));
    m_importElements->setToolTip(tr("Edit the element positions in a table and by dragging; any geometry becomes a custom element table."));
    m_geometryNote = noteLabel();
    f->addRow(tr("Geometry"), m_lattice);
    f->addRow(tr("Length unit"), m_unit);
    f->addRow(tr("Elements along x"), m_nx);
    f->addRow(tr("Elements along y"), m_ny);
    f->addRow(tr("Spacing x"), m_dx);
    f->addRow(tr("Spacing y (rows)"), m_dy);
    f->addRow(tr("Aperture"), m_aperture);
    f->addRow(tr("Semi-axis x"), m_apertureX);
    f->addRow(tr("Semi-axis y"), m_apertureY);
    f->addRow(tr("Rings"), m_rings);
    f->addRow(QString(), m_centreElement);
    f->addRow(tr("Cylinder radius"), m_cylRadius);
    f->addRow(tr("Columns"), m_columns);
    f->addRow(tr("Rows"), m_rows);
    f->addRow(tr("Arc"), m_arc);
    f->addRow(tr("Row spacing"), m_dz);
    f->addRow(QString(), m_importElements);
    f->addRow(m_geometryNote);
    m_toolBox->addItem(p, tr("Geometry"));
    for (QComboBox *c : {m_lattice, m_unit, m_aperture})
        watchCombo(c);
    for (QSpinBox *s : {m_nx, m_ny, m_columns, m_rows})
        watchInt(s);
    for (QDoubleSpinBox *s : {m_dx, m_dy, m_apertureX, m_apertureY, m_cylRadius, m_arc, m_dz})
        watchSpin(s);
    watchLine(m_rings);
    watchCheck(m_centreElement);
    connect(m_importElements, &QPushButton::clicked, this, &ArrayWorkspace::editPositions);

    // Excitation and beams
    p = page(&f);
    m_taperX = idCombo(taperItems());
    m_taperY = idCombo(taperItems());
    m_sllX = doubleSpin(10, 100, 1, QStringLiteral("dB"));
    m_sllY = doubleSpin(10, 100, 1, QStringLiteral("dB"));
    m_nbarX = intSpin(1, 15);
    m_nbarY = intSpin(1, 15);
    m_paramX = doubleSpin(-60, 10, 2, {}, 0.5);
    m_paramY = doubleSpin(-60, 10, 2, {}, 0.5);
    m_radial = new QCheckBox(tr("One taper over the radius"));
    m_steerTheta = doubleSpin(0, 180, 2, QStringLiteral("deg"));
    m_steerPhi = doubleSpin(-360, 360, 2, QStringLiteral("deg"));
    std::vector<std::pair<QString, QString>> modes;
    for (SteeringMode m : {SteeringMode::Phase, SteeringMode::TimeDelay, SteeringMode::SubarrayTimeDelay, SteeringMode::None})
        modes.emplace_back(steeringModeId(m), steeringModeName(m));
    m_steering = idCombo(modes);
    m_subX = intSpin(0, 4096);
    m_subY = intSpin(0, 4096);
    m_subTaper = new QCheckBox(tr("Amplitude per sub-array"));
    std::vector<std::pair<QString, QString>> patterns;
    for (BeamPattern b : {BeamPattern::Sum, BeamPattern::DeltaAzimuth, BeamPattern::DeltaElevation})
        patterns.emplace_back(beamPatternId(b), beamPatternName(b));
    m_pattern = idCombo(patterns);
    m_beams = directionTable({tr("Theta"), tr("Phi"), tr("Level dB"), tr("Phase")});
    m_nulls = directionTable({tr("Theta"), tr("Phi")});
    f->addRow(tr("Taper x"), m_taperX);
    f->addRow(tr("  Side lobes x"), m_sllX);
    f->addRow(tr("  n-bar x"), m_nbarX);
    f->addRow(tr("  Parameter x"), m_paramX);
    f->addRow(tr("Taper y"), m_taperY);
    f->addRow(tr("  Side lobes y"), m_sllY);
    f->addRow(tr("  n-bar y"), m_nbarY);
    f->addRow(tr("  Parameter y"), m_paramY);
    f->addRow(QString(), m_radial);
    f->addRow(tr("Steer theta"), m_steerTheta);
    f->addRow(tr("Steer phi"), m_steerPhi);
    f->addRow(tr("Steering"), m_steering);
    f->addRow(tr("Sub-array x"), m_subX);
    f->addRow(tr("Sub-array y"), m_subY);
    f->addRow(QString(), m_subTaper);
    f->addRow(tr("Pattern"), m_pattern);
    auto tableRow = [&](const QString &label, QTableWidget *table, int columns) {
        auto *box = new QWidget;
        auto *v = new QVBoxLayout(box);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(table);
        auto *buttons = new QHBoxLayout;
        auto *add = new QPushButton(tr("Add"));
        auto *remove = new QPushButton(tr("Remove"));
        buttons->addWidget(add);
        buttons->addWidget(remove);
        buttons->addStretch();
        v->addLayout(buttons);
        f->addRow(label, box);
        connect(add, &QPushButton::clicked, this, [this, table, columns] {
            m_updatingForm = true;
            const int row = table->rowCount();
            table->insertRow(row);
            for (int c = 0; c < columns; ++c)
                setCell(table, row, c, c == 0 ? 30.0 : 0.0);
            m_updatingForm = false;
            commit(tr("Add direction"));
        });
        connect(remove, &QPushButton::clicked, this, [this, table] {
            const int row = table->currentRow() >= 0 ? table->currentRow() : table->rowCount() - 1;
            if (row < 0)
                return;
            table->removeRow(row);
            commit(tr("Remove direction"));
        });
        connect(table, &QTableWidget::itemChanged, this, [this] {
            if (!m_updatingForm)
                commit(tr("Edit direction"));
        });
    };
    tableRow(tr("Extra beams"), m_beams, 4);
    tableRow(tr("Nulls"), m_nulls, 2);
    // Per-element amplitude and phase of the custom taper.
    m_weights = directionTable({tr("Element"), tr("Amplitude dB"), tr("Phase deg")});
    m_weights->setMaximumHeight(220);
    f->addRow(tr("Custom weights"), m_weights);
    connect(m_weights, &QTableWidget::itemChanged, this, [this] {
        if (!m_updatingForm)
            commit(tr("Edit element weight"));
    });
    m_toolBox->addItem(p, tr("Excitation and beams"));
    for (QComboBox *c : {m_taperX, m_taperY, m_steering, m_pattern})
        watchCombo(c);
    for (QDoubleSpinBox *s : {m_sllX, m_sllY, m_paramX, m_paramY, m_steerTheta, m_steerPhi})
        watchSpin(s);
    for (QSpinBox *s : {m_nbarX, m_nbarY, m_subX, m_subY})
        watchInt(s);
    watchCheck(m_radial);
    watchCheck(m_subTaper);

    // AESA
    p = page(&f);
    m_phaseBits = intSpin(0, 16, tr("bits"));
    m_phaseBits->setSpecialValueText(tr("continuous"));
    m_attBits = intSpin(0, 16, tr("bits"));
    m_attBits->setSpecialValueText(tr("continuous"));
    m_attStep = doubleSpin(0.01, 20, 2, QStringLiteral("dB"), 0.25);
    m_delayLsb = doubleSpin(0, 10000, 2, QStringLiteral("ps"));
    m_delayLsb->setSpecialValueText(tr("continuous"));
    m_ampError = doubleSpin(0, 10, 2, QStringLiteral("dB rms"), 0.1);
    m_phaseError = doubleSpin(0, 90, 2, QStringLiteral("deg rms"), 1);
    m_failedFraction = doubleSpin(0, 100, 1, QStringLiteral("%"), 1);
    m_seed = intSpin(0, 1000000);
    m_failedList = new QLineEdit;
    m_failedList->setPlaceholderText(tr("element numbers, e.g. 3, 17, 40"));
    f->addRow(tr("Phase shifter"), m_phaseBits);
    f->addRow(tr("Attenuator"), m_attBits);
    f->addRow(tr("Attenuator step"), m_attStep);
    f->addRow(tr("Delay resolution"), m_delayLsb);
    f->addRow(tr("Amplitude error"), m_ampError);
    f->addRow(tr("Phase error"), m_phaseError);
    f->addRow(tr("Random failures"), m_failedFraction);
    f->addRow(tr("Random seed"), m_seed);
    f->addRow(tr("Failed elements"), m_failedList);
    f->addRow(noteLabel(tr("Click an element in the Layout view to fail or restore it.")));
    m_toolBox->addItem(p, tr("AESA effects"));
    for (QSpinBox *s : {m_phaseBits, m_attBits, m_seed})
        watchInt(s);
    for (QDoubleSpinBox *s : {m_attStep, m_delayLsb, m_ampError, m_phaseError, m_failedFraction})
        watchSpin(s);
    watchLine(m_failedList);

    // T/R module
    p = page(&f);
    const std::vector<std::pair<QString, QString>> sources = {
        {QStringLiteral("typed"), tr("Typed values")}, {QStringLiteral("preset"), tr("Library preset")}, {QStringLiteral("linked"), tr("RF schematic budget")}};
    m_txSource = idCombo(sources);
    m_rxSource = idCombo(sources);
    m_txLink = new QComboBox;
    m_rxLink = new QComboBox;
    m_txPower = doubleSpin(-50, 100, 2, QStringLiteral("dBm"));
    m_txGain = doubleSpin(-50, 100, 2, QStringLiteral("dB"));
    m_rxGain = doubleSpin(-50, 100, 2, QStringLiteral("dB"));
    m_rxNf = doubleSpin(0, 50, 2, QStringLiteral("dB"), 0.1);
    m_frontLoss = doubleSpin(0, 30, 2, QStringLiteral("dB"), 0.1);
    m_bfLoss = doubleSpin(0, 60, 2, QStringLiteral("dB"), 0.5);
    m_antennaTemp = doubleSpin(0, 10000, 1, QStringLiteral("K"), 10);
    m_ambientTemp = doubleSpin(0, 10000, 1, QStringLiteral("K"), 10);
    m_txTaper = new QCheckBox(tr("Transmit with the amplitude taper"));
    m_budgetLabel = noteLabel();
    f->addRow(tr("Transmit source"), m_txSource);
    f->addRow(tr("  Preset or schematic"), m_txLink);
    f->addRow(tr("  Output power"), m_txPower);
    f->addRow(tr("  Gain"), m_txGain);
    f->addRow(tr("Receive source"), m_rxSource);
    f->addRow(tr("  Preset or schematic"), m_rxLink);
    f->addRow(tr("  Gain"), m_rxGain);
    f->addRow(tr("  Noise figure"), m_rxNf);
    f->addRow(tr("Front-end loss"), m_frontLoss);
    f->addRow(tr("Beamformer loss"), m_bfLoss);
    f->addRow(tr("Antenna temperature"), m_antennaTemp);
    f->addRow(tr("Ambient temperature"), m_ambientTemp);
    f->addRow(QString(), m_txTaper);
    f->addRow(m_budgetLabel);
    m_toolBox->addItem(p, tr("T/R module and budget"));
    for (QComboBox *c : {m_txSource, m_rxSource, m_txLink, m_rxLink})
        watchCombo(c);
    for (QDoubleSpinBox *s : {m_txPower, m_txGain, m_rxGain, m_rxNf, m_frontLoss, m_bfLoss, m_antennaTemp, m_ambientTemp})
        watchSpin(s);
    watchCheck(m_txTaper);

    // Synthesis
    p = page(&f);
    m_synMainBeam = doubleSpin(0, 90, 2, QStringLiteral("deg"), 0.5);
    m_synMainBeam->setSpecialValueText(tr("automatic"));
    m_synSll = doubleSpin(-100, 0, 1, QStringLiteral("dB"));
    m_synSegments = new QLineEdit;
    m_synSegments->setPlaceholderText(tr("from deg:level dB, e.g. 20:-40, 50:-45"));
    m_synPhaseOnly = new QCheckBox(tr("Phase only"));
    m_synIterations = intSpin(1, 100000);
    auto *run = new QPushButton(tr("Synthesise Weights"));
    m_synResult = noteLabel();
    f->addRow(tr("Main-beam half width"), m_synMainBeam);
    f->addRow(tr("Side-lobe mask"), m_synSll);
    f->addRow(tr("Further steps"), m_synSegments);
    f->addRow(QString(), m_synPhaseOnly);
    f->addRow(tr("Iterations"), m_synIterations);
    f->addRow(QString(), run);
    f->addRow(m_synResult);
    f->addRow(noteLabel(tr("The result becomes the custom taper (per element amplitude and phase); steering is applied on top.")));
    m_toolBox->addItem(p, tr("Pattern synthesis"));
    watchSpin(m_synMainBeam);
    watchSpin(m_synSll);
    watchLine(m_synSegments);
    watchCheck(m_synPhaseOnly);
    watchInt(m_synIterations);
    connect(run, &QPushButton::clicked, this, &ArrayWorkspace::synthesise);

    // Sweeps
    p = page(&f);
    m_scanPlane = doubleSpin(-360, 360, 1, QStringLiteral("deg"));
    m_scanMax = doubleSpin(1, 90, 1, QStringLiteral("deg"));
    m_scanStep = doubleSpin(0.1, 30, 1, QStringLiteral("deg"), 0.5);
    m_bandStart = doubleSpin(0, 1000, 4, QStringLiteral("GHz"), 0.1);
    m_bandStart->setSpecialValueText(tr("automatic"));
    m_bandStop = doubleSpin(0, 1000, 4, QStringLiteral("GHz"), 0.1);
    m_bandStop->setSpecialValueText(tr("automatic"));
    m_bandPoints = intSpin(2, 1001);
    f->addRow(tr("Scan plane phi"), m_scanPlane);
    f->addRow(tr("Scan to"), m_scanMax);
    f->addRow(tr("Scan step"), m_scanStep);
    f->addRow(tr("Band start"), m_bandStart);
    f->addRow(tr("Band stop"), m_bandStop);
    f->addRow(tr("Band points"), m_bandPoints);
    m_toolBox->addItem(p, tr("Scan and band sweeps"));
    for (QDoubleSpinBox *s : {m_scanPlane, m_scanMax, m_scanStep, m_bandStart, m_bandStop})
        watchSpin(s);
    watchInt(m_bandPoints);
}

void ArrayWorkspace::buildViews()
{
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
    m_polar = new QCheckBox(tr("Polar"));
    m_showIdeal = new QCheckBox(tr("Ideal reference"));
    m_showIdeal->setChecked(true);
    m_layoutValue = idCombo({{QStringLiteral("amplitude"), tr("Amplitude")}, {QStringLiteral("phase"), tr("Phase")}});
    m_cutMode = idCombo({{QStringLiteral("auto"), tr("Cuts: automatic")},
                         {QStringLiteral("planes"), tr("Cuts: phi = 0 and 90")},
                         {QStringLiteral("beam"), tr("Cuts: through the beam")},
                         {QStringLiteral("horizon"), tr("Cut: horizon (theta = 90)")}});
    m_statusLabel = new QLabel;
    bar->addWidget(new QLabel(tr("Show")));
    bar->addWidget(m_component);
    bar->addWidget(m_copol);
    bar->addWidget(new QLabel(tr("Range")));
    bar->addWidget(m_range);
    bar->addWidget(m_polar);
    bar->addWidget(m_showIdeal);
    bar->addWidget(m_layoutValue);
    bar->addWidget(m_cutMode);
    bar->addStretch();
    bar->addWidget(m_statusLabel);
    v->addLayout(bar);

    m_views = new QTabWidget;
    m_views->setDocumentMode(true);
    m_cutPlot = new PlotWidget;
    m_cutPlot->setTitle(tr("Pattern cuts"));
    m_uvPlot = new ColorMapPlot;
    m_uvPlot->setAspectLocked(true);
    m_uvPlot->setXAxis(QStringLiteral("u"));
    m_uvPlot->setYAxis(QStringLiteral("v"));
    m_view3d = new Pattern3DView;
    m_layoutPlot = new ColorMapPlot;
    m_layoutPlot->setAspectLocked(true);
    m_layoutPlot->setXAxis(QStringLiteral("x"), QStringLiteral("m"));
    m_layoutPlot->setYAxis(QStringLiteral("y"), QStringLiteral("m"));
    auto *sweeps = new QSplitter(Qt::Vertical);
    m_scanPlot = new PlotWidget;
    m_scanPlot->setTitle(tr("Scan"));
    m_bandPlot = new PlotWidget;
    m_bandPlot->setTitle(tr("Band"));
    sweeps->addWidget(m_scanPlot);
    sweeps->addWidget(m_bandPlot);
    m_summary = new QTableWidget(0, 2);
    m_summary->setHorizontalHeaderLabels({tr("Quantity"), tr("Value")});
    m_summary->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_summary->horizontalHeader()->setStretchLastSection(true);
    m_summary->verticalHeader()->setVisible(false);
    m_summary->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_views->addTab(m_cutPlot, tr("Cuts"));
    m_views->addTab(m_uvPlot, tr("UV Map"));
    m_views->addTab(m_view3d, tr("3D"));
    m_views->addTab(m_layoutPlot, tr("Layout"));
    m_views->addTab(sweeps, tr("Scan and Band"));
    m_views->addTab(m_summary, tr("Summary"));
    v->addWidget(m_views);
    addWidget(right);

    auto redraw = [this] { refreshViews(); };
    connect(m_component, &QComboBox::currentIndexChanged, this, redraw);
    connect(m_copol, &QComboBox::currentIndexChanged, this, [this] {
        refreshViews();
        scheduleCompute(); // the cross-polar metric depends on the reference
    });
    connect(m_range, &QDoubleSpinBox::valueChanged, this, redraw);
    connect(m_polar, &QCheckBox::toggled, this, redraw);
    connect(m_showIdeal, &QCheckBox::toggled, this, redraw);
    connect(m_layoutValue, &QComboBox::currentIndexChanged, this, [this] { refreshLayout(); });
    connect(m_cutMode, &QComboBox::currentIndexChanged, this, [this] {
        // A view setting, kept with the document.
        if (!m_updatingForm && m_document) {
            m_design.view.insert(QStringLiteral("cutMode"), comboId(m_cutMode));
            const QJsonObject before = m_content;
            m_content = m_design.toJson();
            m_writing = true;
            m_document->setContent(m_content);
            m_writing = false;
            m_undo->push(new ContentCommand(tr("Change cut"), before, m_content, -1, [this](const QJsonObject &c) { applyContent(c); }));
        }
        if (m_results)
            refreshCuts();
    });
    connect(m_views, &QTabWidget::currentChanged, this, [this] {
        m_layoutValue->setVisible(m_views->currentWidget() == m_layoutPlot);
        m_cutMode->setVisible(m_views->currentWidget() == m_cutPlot);
    });
    m_layoutValue->setVisible(false);
    connect(m_layoutPlot, &ColorMapPlot::pointClicked, this, [this](QPointF, int index) {
        if (index >= 0)
            toggleFailure(index);
    });
    for (auto *w : {static_cast<QObject *>(m_cutPlot), static_cast<QObject *>(m_scanPlot), static_cast<QObject *>(m_bandPlot)})
        connect(static_cast<PlotWidget *>(w), &PlotWidget::hoverReadoutChanged, this, &ArrayWorkspace::statusMessage);
    connect(m_uvPlot, &ColorMapPlot::hoverReadoutChanged, this, &ArrayWorkspace::statusMessage);
    connect(m_layoutPlot, &ColorMapPlot::hoverReadoutChanged, this, &ArrayWorkspace::statusMessage);
    connect(m_view3d, &Pattern3DView::hoverReadoutChanged, this, &ArrayWorkspace::statusMessage);
}

void ArrayWorkspace::refreshElementChoices()
{
    const QString current = m_design.elementRef;
    const QSignalBlocker block(m_elementSource);
    m_elementSource->clear();
    m_elementSource->addItem(tr("Inline model (below)"), QString());
    if (m_project)
        for (Document *d : m_project->documents())
            if (d->type() == DocumentType::AntennaElement)
                m_elementSource->addItem(tr("Element document: %1").arg(d->name()), QStringLiteral("element:") + d->name());
    if (m_library)
        for (const AntennaLibrary &lib : m_library->libraries())
            for (const AntennaLibraryEntry &e : lib.entries)
                if (e.kind == AntennaEntryKind::Element || e.kind == AntennaEntryKind::Pattern)
                    m_elementSource->addItem(QStringLiteral("%1: %2").arg(lib.name, e.name),
                                             QStringLiteral("library:") + AntennaLibraryManager::makeKey(lib.name, e.id));
    if (!current.isEmpty() && m_elementSource->findData(current) < 0)
        m_elementSource->addItem(tr("%1 (missing)").arg(current), current);
}

void ArrayWorkspace::refreshTrChoices()
{
    for (QComboBox *combo : {m_txLink, m_rxLink}) {
        const QSignalBlocker block(combo);
        combo->clear();
        combo->addItem(tr("(none)"), QString());
        if (m_library)
            for (const AntennaLibrary &lib : m_library->libraries())
                for (const AntennaLibraryEntry &e : lib.entries)
                    if (e.kind == AntennaEntryKind::TrModule)
                        combo->addItem(tr("Preset %1: %2").arg(lib.name, e.name),
                                       QStringLiteral("preset:") + AntennaLibraryManager::makeKey(lib.name, e.id));
        if (m_project)
            for (Document *d : m_project->documents())
                if (d->type() == DocumentType::RfSchematic)
                    combo->addItem(tr("Schematic %1").arg(d->name()), QStringLiteral("linked:") + d->name());
    }
}

// ---- Form <-> design

void ArrayWorkspace::designToForm()
{
    m_updatingForm = true;
    const ArrayDesign &d = m_design;
    const QList<QWidget *> widgets = findChildren<QWidget *>();
    for (QWidget *w : widgets)
        w->blockSignals(true);

    setComboId(m_cutMode, d.view.value(QStringLiteral("cutMode")).toString(QStringLiteral("auto")));
    if (d.view.contains(QStringLiteral("polar")))
        m_polar->setChecked(d.view.value(QStringLiteral("polar")).toBool());
    m_frequency->setValue(d.frequency / GHz);
    m_designFrequency->setValue(d.designFrequency / GHz);
    if (m_elementSource->findData(d.elementRef) < 0)
        refreshElementChoices();
    setComboId(m_elementSource, d.elementRef);
    setComboId(m_elementModel, elementModelId(d.inlineElement.model));
    setComboId(m_elementPolarisation, polarisationId(d.inlineElement.polarisation));
    m_elementExponent->setValue(d.inlineElement.exponent);
    m_elementEfficiency->setValue(d.inlineElement.efficiencyDb);

    const GeometrySpec &g = d.geometry;
    setComboId(m_lattice, latticeId(g.kind));
    setComboId(m_unit, g.unit);
    m_nx->setValue(g.nx);
    m_ny->setValue(g.ny);
    m_dx->setValue(g.dx);
    m_dy->setValue(g.dy);
    setComboId(m_aperture, g.aperture);
    m_apertureX->setValue(g.apertureX);
    m_apertureY->setValue(g.apertureY);
    QStringList rings;
    for (const RingSpec &r : g.rings)
        rings << (r.rotationDeg != 0 ? QStringLiteral("%1:%2:%3").arg(r.radius).arg(r.count).arg(r.rotationDeg)
                                     : QStringLiteral("%1:%2").arg(r.radius).arg(r.count));
    m_rings->setText(rings.join(QStringLiteral(", ")));
    m_centreElement->setChecked(g.centreElement);
    m_cylRadius->setValue(g.cylinderRadius);
    m_columns->setValue(g.columns);
    m_rows->setValue(g.rows);
    m_arc->setValue(g.arcDeg);
    m_dz->setValue(g.dz);

    const ExcitationSpec &x = d.excitation;
    auto taperToForm = [](const TaperSpec &t, QComboBox *kind, QDoubleSpinBox *sll, QSpinBox *nbar, QDoubleSpinBox *param) {
        setComboId(kind, taperId(t.kind));
        sll->setValue(t.sllDb);
        nbar->setValue(t.nbar);
        param->setValue(t.kind == TaperKind::CosinePedestal ? t.pedestalDb : t.exponent);
    };
    taperToForm(x.x, m_taperX, m_sllX, m_nbarX, m_paramX);
    taperToForm(x.y, m_taperY, m_sllY, m_nbarY, m_paramY);
    m_radial->setChecked(x.radial);
    m_steerTheta->setValue(x.steerThetaDeg);
    m_steerPhi->setValue(x.steerPhiDeg);
    setComboId(m_steering, steeringModeId(x.steering));
    m_subX->setValue(x.subarrayX);
    m_subY->setValue(x.subarrayY);
    m_subTaper->setChecked(x.subarrayTaper);
    setComboId(m_pattern, beamPatternId(x.pattern));
    m_beams->setRowCount(int(x.extraBeams.size()));
    for (int i = 0; i < int(x.extraBeams.size()); ++i) {
        const BeamSpec &b = x.extraBeams[std::size_t(i)];
        setCell(m_beams, i, 0, b.thetaDeg);
        setCell(m_beams, i, 1, b.phiDeg);
        setCell(m_beams, i, 2, b.levelDb);
        setCell(m_beams, i, 3, b.phaseDeg);
    }
    {
        // Custom weights: the stored ones, else the element table, else 0 dB.
        const ArrayLayout layout = buildLayout(g, d.designFrequency);
        const int n = int(layout.elements.size());
        m_weights->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            const bool stored = int(x.weights.size()) == n;
            const auto &e = layout.elements[std::size_t(i)];
            auto *number = new QTableWidgetItem(QString::number(i + 1));
            number->setFlags(Qt::ItemIsEnabled);
            m_weights->setItem(i, 0, number);
            setCell(m_weights, i, 1, stored ? x.weights[std::size_t(i)].first : e.amplitudeDb);
            setCell(m_weights, i, 2, stored ? x.weights[std::size_t(i)].second : e.phaseDeg);
        }
    }
    m_nulls->setRowCount(int(x.nulls.size()));
    for (int i = 0; i < int(x.nulls.size()); ++i) {
        setCell(m_nulls, i, 0, x.nulls[std::size_t(i)].thetaDeg);
        setCell(m_nulls, i, 1, x.nulls[std::size_t(i)].phiDeg);
    }

    const AesaSpec &a = d.aesa;
    m_phaseBits->setValue(a.phaseBits);
    m_attBits->setValue(a.attenuatorBits);
    m_attStep->setValue(a.attenuatorStepDb);
    m_delayLsb->setValue(a.delayLsbPs);
    m_ampError->setValue(a.amplitudeErrorDb);
    m_phaseError->setValue(a.phaseErrorDeg);
    m_failedFraction->setValue(a.failedFraction * 100);
    m_seed->setValue(int(a.seed));
    QStringList failed;
    for (int i : a.failed)
        failed << QString::number(i + 1);
    m_failedList->setText(failed.join(QStringLiteral(", ")));

    const TrModuleSpec &t = d.trModule;
    auto sideToForm = [](const TrSideSpec &s, QComboBox *source, QComboBox *link) {
        setComboId(source, s.source);
        if (s.source == QLatin1String("preset"))
            setComboId(link, QStringLiteral("preset:") + s.preset);
        else if (s.source == QLatin1String("linked"))
            setComboId(link, QStringLiteral("linked:") + s.document);
        else
            link->setCurrentIndex(0);
    };
    sideToForm(t.tx, m_txSource, m_txLink);
    sideToForm(t.rx, m_rxSource, m_rxLink);
    m_txPower->setValue(t.tx.powerDbm);
    m_txGain->setValue(t.tx.gainDb);
    m_rxGain->setValue(t.rx.gainDb);
    m_rxNf->setValue(t.rx.nfDb);
    m_frontLoss->setValue(t.frontLossDb);
    m_bfLoss->setValue(t.beamformerLossDb);
    m_antennaTemp->setValue(t.antennaTemperatureK);
    m_ambientTemp->setValue(t.ambientTemperatureK);
    m_txTaper->setChecked(t.txTaper);

    const SynthesisSpec &s = d.synthesis;
    m_synMainBeam->setValue(s.mainBeamDeg);
    m_synSll->setValue(s.sllDb);
    QStringList segments;
    for (const MaskSegment &m : s.segments)
        segments << QStringLiteral("%1:%2").arg(m.fromDeg).arg(m.levelDb);
    m_synSegments->setText(segments.join(QStringLiteral(", ")));
    m_synPhaseOnly->setChecked(s.phaseOnly);
    m_synIterations->setValue(s.iterations);

    const SweepSpec &w = d.sweeps;
    m_scanPlane->setValue(w.scanPlanePhiDeg);
    m_scanMax->setValue(w.scanMaxDeg);
    m_scanStep->setValue(w.scanStepDeg);
    m_bandStart->setValue(w.bandStart / GHz);
    m_bandStop->setValue(w.bandStop / GHz);
    m_bandPoints->setValue(w.bandPoints);

    const QString unit = g.unit == QLatin1String("m") ? QStringLiteral("m") : QStringLiteral("λ");
    for (QDoubleSpinBox *sp : {m_dx, m_dy, m_apertureX, m_apertureY, m_cylRadius, m_dz})
        sp->setSuffix(QLatin1Char(' ') + unit);

    for (QWidget *w2 : widgets)
        w2->blockSignals(false);
    m_updatingForm = false;
    updateVisibility();
}

void ArrayWorkspace::formToDesign()
{
    ArrayDesign &d = m_design;
    d.frequency = m_frequency->value() * GHz;
    d.designFrequency = m_designFrequency->value() * GHz;
    d.elementRef = comboId(m_elementSource);
    for (ElementModelKind k : elementModelKinds())
        if (elementModelId(k) == comboId(m_elementModel))
            d.inlineElement.model = k;
    for (Polarisation p : polarisations())
        if (polarisationId(p) == comboId(m_elementPolarisation))
            d.inlineElement.polarisation = p;
    d.inlineElement.exponent = m_elementExponent->value();
    d.inlineElement.efficiencyDb = m_elementEfficiency->value();
    d.inlineElement.frequency = d.designFrequency;

    GeometrySpec &g = d.geometry;
    for (LatticeKind k : latticeKinds())
        if (latticeId(k) == comboId(m_lattice))
            g.kind = k;
    g.unit = comboId(m_unit);
    g.nx = m_nx->value();
    g.ny = m_ny->value();
    g.dx = m_dx->value();
    g.dy = m_dy->value();
    g.aperture = comboId(m_aperture);
    g.apertureX = m_apertureX->value();
    g.apertureY = m_apertureY->value();
    g.rings.clear();
    for (const QString &item : m_rings->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList parts = item.trimmed().split(QLatin1Char(':'));
        if (parts.size() < 2)
            continue;
        RingSpec r;
        r.radius = parts[0].toDouble();
        r.count = std::max(1, parts[1].toInt());
        r.rotationDeg = parts.size() > 2 ? parts[2].toDouble() : 0.0;
        if (r.radius > 0)
            g.rings.push_back(r);
    }
    g.centreElement = m_centreElement->isChecked();
    g.cylinderRadius = m_cylRadius->value();
    g.columns = m_columns->value();
    g.rows = m_rows->value();
    g.arcDeg = m_arc->value();
    g.dz = m_dz->value();

    ExcitationSpec &x = d.excitation;
    auto formToTaper = [](TaperSpec &t, QComboBox *kind, QDoubleSpinBox *sll, QSpinBox *nbar, QDoubleSpinBox *param) {
        for (TaperKind k : taperKinds())
            if (taperId(k) == comboId(kind))
                t.kind = k;
        t.sllDb = sll->value();
        t.nbar = nbar->value();
        if (t.kind == TaperKind::CosinePedestal)
            t.pedestalDb = std::min(0.0, param->value());
        else if (t.kind == TaperKind::Cosine)
            t.exponent = std::max(0.0, param->value());
    };
    formToTaper(x.x, m_taperX, m_sllX, m_nbarX, m_paramX);
    formToTaper(x.y, m_taperY, m_sllY, m_nbarY, m_paramY);
    x.radial = m_radial->isChecked();
    x.steerThetaDeg = m_steerTheta->value();
    x.steerPhiDeg = m_steerPhi->value();
    for (SteeringMode m : {SteeringMode::Phase, SteeringMode::TimeDelay, SteeringMode::SubarrayTimeDelay, SteeringMode::None})
        if (steeringModeId(m) == comboId(m_steering))
            x.steering = m;
    x.subarrayX = m_subX->value();
    x.subarrayY = m_subY->value();
    x.subarrayTaper = m_subTaper->isChecked();
    for (BeamPattern b : {BeamPattern::Sum, BeamPattern::DeltaAzimuth, BeamPattern::DeltaElevation})
        if (beamPatternId(b) == comboId(m_pattern))
            x.pattern = b;
    x.extraBeams.clear();
    for (int i = 0; i < m_beams->rowCount(); ++i)
        x.extraBeams.push_back({cell(m_beams, i, 0), cell(m_beams, i, 1), cell(m_beams, i, 2), cell(m_beams, i, 3)});
    if (x.x.kind == TaperKind::Custom && m_weights->rowCount() > 0) {
        x.weights.clear();
        for (int i = 0; i < m_weights->rowCount(); ++i)
            x.weights.emplace_back(cell(m_weights, i, 1), cell(m_weights, i, 2));
    }
    x.nulls.clear();
    for (int i = 0; i < m_nulls->rowCount(); ++i)
        x.nulls.push_back({cell(m_nulls, i, 0), cell(m_nulls, i, 1)});

    AesaSpec &a = d.aesa;
    a.phaseBits = m_phaseBits->value();
    a.attenuatorBits = m_attBits->value();
    a.attenuatorStepDb = m_attStep->value();
    a.delayLsbPs = m_delayLsb->value();
    a.amplitudeErrorDb = m_ampError->value();
    a.phaseErrorDeg = m_phaseError->value();
    a.failedFraction = m_failedFraction->value() / 100.0;
    a.seed = unsigned(m_seed->value());
    a.failed.clear();
    for (const QString &item : m_failedList->text().split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int i = item.toInt(&ok);
        if (ok && i >= 1)
            a.failed.push_back(i - 1);
    }
    std::sort(a.failed.begin(), a.failed.end());
    a.failed.erase(std::unique(a.failed.begin(), a.failed.end()), a.failed.end());

    TrModuleSpec &t = d.trModule;
    auto formToSide = [](TrSideSpec &s, QComboBox *source, QComboBox *link) {
        s.source = comboId(source);
        const QString l = comboId(link);
        if (l.startsWith(QLatin1String("preset:")))
            s.preset = l.mid(7);
        else if (l.startsWith(QLatin1String("linked:")))
            s.document = l.mid(7);
    };
    formToSide(t.tx, m_txSource, m_txLink);
    formToSide(t.rx, m_rxSource, m_rxLink);
    t.tx.powerDbm = m_txPower->value();
    t.tx.gainDb = m_txGain->value();
    t.rx.gainDb = m_rxGain->value();
    t.rx.nfDb = m_rxNf->value();
    t.frontLossDb = m_frontLoss->value();
    t.beamformerLossDb = m_bfLoss->value();
    t.antennaTemperatureK = m_antennaTemp->value();
    t.ambientTemperatureK = m_ambientTemp->value();
    t.txTaper = m_txTaper->isChecked();

    SynthesisSpec &s = d.synthesis;
    s.mainBeamDeg = m_synMainBeam->value();
    s.sllDb = m_synSll->value();
    s.segments.clear();
    for (const QString &item : m_synSegments->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList parts = item.trimmed().split(QLatin1Char(':'));
        if (parts.size() == 2)
            s.segments.push_back({parts[0].toDouble(), std::min(0.0, parts[1].toDouble())});
    }
    s.phaseOnly = m_synPhaseOnly->isChecked();
    s.iterations = m_synIterations->value();

    SweepSpec &w = d.sweeps;
    w.scanPlanePhiDeg = m_scanPlane->value();
    w.scanMaxDeg = m_scanMax->value();
    w.scanStepDeg = m_scanStep->value();
    w.bandStart = m_bandStart->value() * GHz;
    w.bandStop = m_bandStop->value() * GHz;
    w.bandPoints = m_bandPoints->value();
}

void ArrayWorkspace::updateVisibility()
{
    auto formOf = [](QWidget *w) { return qobject_cast<QFormLayout *>(w->parentWidget()->layout()); };
    const QString lattice = comboId(m_lattice);
    const bool linear = lattice == QLatin1String("linear");
    const bool planar = lattice == QLatin1String("rectangular") || lattice == QLatin1String("triangular");
    const bool circular = lattice == QLatin1String("circular");
    const bool cylinder = lattice == QLatin1String("cylinder");
    const bool custom = lattice == QLatin1String("custom");
    QFormLayout *g = formOf(m_lattice);
    setRowVisible(g, m_nx, linear || planar);
    setRowVisible(g, m_dx, linear || planar);
    setRowVisible(g, m_ny, planar);
    setRowVisible(g, m_dy, planar);
    setRowVisible(g, m_aperture, planar);
    const bool truncated = planar && comboId(m_aperture) != QLatin1String("none");
    setRowVisible(g, m_apertureX, truncated);
    setRowVisible(g, m_apertureY, truncated && comboId(m_aperture) == QLatin1String("ellipse"));
    setRowVisible(g, m_rings, circular);
    setRowVisible(g, m_centreElement, circular);
    for (QWidget *w : {static_cast<QWidget *>(m_cylRadius), static_cast<QWidget *>(m_columns), static_cast<QWidget *>(m_rows),
                       static_cast<QWidget *>(m_arc), static_cast<QWidget *>(m_dz)})
        setRowVisible(g, w, cylinder);
    setRowVisible(g, m_importElements, true);
    m_geometryNote->setText(custom ? tr("%n elements in the table.", nullptr, int(m_design.geometry.custom.size()))
                                   : cylinder ? tr("Elements face outward; the array's broadside is +x (steer theta 90).")
                                              : QString());

    QFormLayout *e = formOf(m_elementSource);
    const bool inlineElement = comboId(m_elementSource).isEmpty();
    setRowVisible(e, m_elementModel, inlineElement);
    setRowVisible(e, m_elementPolarisation, inlineElement && comboId(m_elementModel) != QLatin1String("monopole"));
    setRowVisible(e, m_elementExponent, inlineElement && comboId(m_elementModel) == QLatin1String("cosn"));
    setRowVisible(e, m_elementEfficiency, inlineElement);

    QFormLayout *x = formOf(m_taperX);
    auto taperRows = [&](QComboBox *kind, QDoubleSpinBox *sll, QSpinBox *nbar, QDoubleSpinBox *param, bool visible) {
        const QString k = comboId(kind);
        setRowVisible(x, kind, visible);
        setRowVisible(x, sll, visible && (k == QLatin1String("taylor") || k == QLatin1String("chebyshev")));
        setRowVisible(x, nbar, visible && k == QLatin1String("taylor"));
        setRowVisible(x, param, visible && (k == QLatin1String("cosine") || k == QLatin1String("cosine-pedestal")));
        x->labelForField(param) ? static_cast<QLabel *>(x->labelForField(param))
                                      ->setText(k == QLatin1String("cosine") ? tr("  Exponent") : tr("  Edge level dB"))
                                : void();
    };
    const bool radial = m_radial->isChecked() && (planar || circular || custom);
    const bool customTaper = comboId(m_taperX) == QLatin1String("custom");
    taperRows(m_taperX, m_sllX, m_nbarX, m_paramX, true);
    taperRows(m_taperY, m_sllY, m_nbarY, m_paramY, !linear && !radial && !customTaper);
    setRowVisible(x, m_radial, planar || circular || custom);
    setRowVisible(x, m_subY, !linear);
    const bool sub = m_subX->value() > 0 || m_subY->value() > 0;
    setRowVisible(x, m_subTaper, sub);
    setRowVisible(x, m_weights, customTaper);

    // Typed values only matter for the typed source; presets and linked
    // schematics show the values used in the budget note below.
    QFormLayout *t = formOf(m_txSource);
    const bool txTyped = comboId(m_txSource) == QLatin1String("typed");
    const bool rxTyped = comboId(m_rxSource) == QLatin1String("typed");
    setRowVisible(t, m_txLink, !txTyped);
    setRowVisible(t, m_rxLink, !rxTyped);
    setRowVisible(t, m_txPower, txTyped);
    setRowVisible(t, m_txGain, txTyped);
    setRowVisible(t, m_rxGain, rxTyped);
    setRowVisible(t, m_rxNf, rxTyped);
}

// ---- Edits

void ArrayWorkspace::commit(const QString &text, int mergeKey)
{
    if (!m_document)
        return;
    const QJsonObject before = m_content;
    formToDesign();
    // Custom weights belong to one element count: after a geometry change
    // the element table (or 0 dB) takes over and the table is rebuilt.
    if (m_design.excitation.x.kind == TaperKind::Custom) {
        const std::size_t n = buildLayout(m_design.geometry, m_design.designFrequency).elements.size();
        if (m_design.excitation.weights.size() != n) {
            m_design.excitation.weights.clear();
            designToForm();
        }
    }
    const QJsonObject after = m_design.toJson();
    updateVisibility();
    if (after == before)
        return;
    m_content = after;
    m_writing = true;
    m_document->setContent(after);
    m_writing = false;
    m_undo->push(new ContentCommand(text, before, after, mergeKey, [this](const QJsonObject &c) { applyContent(c); }));
    scheduleCompute();
}

void ArrayWorkspace::applyContent(const QJsonObject &content)
{
    m_content = content;
    m_design = ArrayDesign::fromJson(content);
    if (m_document && m_document->content() != content) {
        m_writing = true;
        m_document->setContent(content);
        m_writing = false;
    }
    designToForm();
    scheduleCompute();
}

void ArrayWorkspace::toggleFailure(int index)
{
    auto &failed = m_design.aesa.failed;
    const auto it = std::find(failed.begin(), failed.end(), index);
    if (it != failed.end())
        failed.erase(it);
    else
        failed.push_back(index);
    std::sort(failed.begin(), failed.end());
    const QJsonObject json = m_design.toJson();
    m_design = ArrayDesign::fromJson(json);
    designToForm();
    commit(tr("Toggle element failure"));
}

void ArrayWorkspace::editPositions()
{
    formToDesign();
    PositionEditor editor(m_design.geometry, m_design.designFrequency, this);
    if (editor.exec() != QDialog::Accepted)
        return;
    m_design.geometry.kind = LatticeKind::Custom;
    m_design.geometry.custom = editor.elements();
    designToForm();
    commit(tr("Edit element positions"));
}

void ArrayWorkspace::applyLibraryEntry(const QString &key, const AntennaLibraryEntry &entry)
{
    switch (entry.kind) {
    case AntennaEntryKind::Element:
    case AntennaEntryKind::Pattern:
        m_design.elementRef = QStringLiteral("library:") + key;
        refreshElementChoices();
        designToForm();
        commit(tr("Use library element"));
        break;
    case AntennaEntryKind::TrModule:
        applyTrPreset(entry.data, &m_design.trModule, &m_design.aesa);
        m_design.trModule.tx.source = m_design.trModule.rx.source = QStringLiteral("preset");
        m_design.trModule.tx.preset = m_design.trModule.rx.preset = key;
        designToForm();
        commit(tr("Use T/R module preset"));
        break;
    case AntennaEntryKind::Array: {
        QJsonObject json = m_design.toJson();
        for (auto it = entry.data.begin(); it != entry.data.end(); ++it)
            json.insert(it.key(), it.value());
        m_design = ArrayDesign::fromJson(json);
        refreshElementChoices();
        designToForm();
        commit(tr("Apply array preset"));
        break;
    }
    }
}

void ArrayWorkspace::synthesise()
{
    formToDesign();
    const ArrayLayout layout = buildLayout(m_design.geometry, m_design.designFrequency);
    const SynthesisSpec spec = m_design.synthesis;
    const double frequency = m_design.designFrequency;
    std::atomic_bool cancel = false;
    std::atomic_int progress = 0;
    SynthesisResult result;
    QThread *thread = QThread::create([&] {
        result = ant::synthesise(layout, {}, frequency, spec, &cancel, [&](int p) { progress = p; });
    });
    std::unique_ptr<QProgressDialog> dialog;
    if (isVisible()) {
        dialog = std::make_unique<QProgressDialog>(tr("Synthesising weights..."), tr("Cancel"), 0, 100, this);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setMinimumDuration(300);
        connect(dialog.get(), &QProgressDialog::canceled, this, [&cancel] { cancel = true; });
    }
    QEventLoop loop;
    connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    QTimer poll;
    connect(&poll, &QTimer::timeout, this, [&] {
        if (dialog)
            dialog->setValue(progress);
    });
    poll.start(100);
    thread->start();
    loop.exec();
    thread->wait();
    delete thread;
    dialog.reset();
    if (result.cancelled || result.weights.empty()) {
        m_synResult->setText(tr("Synthesis cancelled."));
        return;
    }
    m_design.excitation.x.kind = TaperKind::Custom;
    m_design.excitation.weights.clear();
    for (const cplx &w : result.weights)
        m_design.excitation.weights.emplace_back(20 * std::log10(std::max(std::abs(w), 1e-6)), toDegrees(std::arg(w)));
    designToForm();
    commit(tr("Synthesise weights"));
    m_synResult->setText(tr("%1 after %2 iterations: highest side lobe %3 dB (worst mask excess %4 dB).")
                             .arg(result.converged ? tr("Mask met") : tr("Mask not met"))
                             .arg(result.iterations + 1)
                             .arg(result.achievedSllDb, 0, 'f', 2)
                             .arg(std::max(0.0, result.worstExcessDb), 0, 'f', 2));
    Q_EMIT statusMessage(m_synResult->text());
}

// ---- Computation

void ArrayWorkspace::scheduleCompute()
{
    m_statusLabel->setText(tr("Computing..."));
    m_debounce->start();
}

void ArrayWorkspace::startCompute()
{
    if (m_running) {
        m_pending = true;
        if (m_cancel)
            *m_cancel = true;
        return;
    }
    auto results = std::make_shared<ArrayResults>();
    results->design = m_design;
    const ElementResolution element = resolveElement(m_design, m_project, m_library);
    results->warnings << element.warnings;
    results->tx = resolveTrSide(m_design.trModule.tx, true, m_project, m_library);
    results->rx = resolveTrSide(m_design.trModule.rx, false, m_project, m_library);
    for (const TrValues *v : {&results->tx, &results->rx}) {
        if (!v->error.isEmpty())
            results->warnings << v->error;
        results->warnings << v->warnings;
    }
    m_elementNote->setText(element.pattern ? element.pattern->spec().summary() : element.error);
    if (!element.pattern) {
        results->error = element.error;
        onComputed(results);
        return;
    }
    const CoPolarisation co = comboId(m_copol) == QLatin1String("y") ? CoPolarisation::Y : CoPolarisation::X;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_cancel;
    auto pattern = element.pattern;
    m_running = true;
    QThread *thread = QThread::create([results, pattern, co, cancel] { compute(results, pattern, co, cancel.get()); });
    thread->setParent(this);
    connect(thread, &QThread::finished, this, [this, thread, results] {
        thread->deleteLater();
        m_running = false;
        if (m_pending || results->cancelled) {
            m_pending = false;
            startCompute();
            return;
        }
        onComputed(results);
    });
    thread->start();
}

void ArrayWorkspace::onComputed(std::shared_ptr<ArrayResults> results)
{
    m_results = std::move(results);
    if (!m_results->error.isEmpty())
        m_statusLabel->setText(tr("Error: %1").arg(m_results->error));
    else
        m_statusLabel->setText(tr("%n elements, %1 s", nullptr, int(m_results->evaluation.layout.elements.size()))
                                   .arg(m_results->seconds, 0, 'f', 2));
    refreshViews();
    const BudgetValues &b = m_results->budget;
    m_budgetLabel->setText(tr("EIRP %1, G/T %2, Tsys %3 K, PA total %4 W\nTransmit: %5; receive: %6")
                               .arg(formatDb(b.eirpDbw, QStringLiteral("dBW")), formatDb(b.gOverTDbK, QStringLiteral("dB/K")))
                               .arg(b.systemTemperatureK, 0, 'f', 0)
                               .arg(b.totalPowerW, 0, 'g', 4)
                               .arg(m_results->tx.origin, m_results->rx.origin));
    Q_EMIT resultsReady();
}

bool ArrayWorkspace::waitForResults(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    if (m_debounce->isActive()) {
        m_debounce->stop();
        startCompute();
    }
    while ((m_running || m_pending || !m_results) && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return !m_running && m_results;
}

void ArrayWorkspace::showView(const QString &name)
{
    const QStringList names = {QStringLiteral("cuts"), QStringLiteral("uv"), QStringLiteral("3d"),
                               QStringLiteral("layout"), QStringLiteral("scan"), QStringLiteral("summary")};
    const int i = int(names.indexOf(name));
    if (i >= 0)
        m_views->setCurrentIndex(i);
}

void ArrayWorkspace::showSettingsPage(int index)
{
    m_toolBox->setCurrentIndex(index);
}

void ArrayWorkspace::setViewOptions(const QString &component, bool polar, double rangeDb)
{
    setComboId(m_component, component);
    m_polar->setChecked(polar);
    m_range->setValue(rangeDb);
    refreshViews();
}

// ---- Views

void ArrayWorkspace::refreshViews()
{
    if (!m_results)
        return;
    refreshCuts();
    refreshUv();
    refresh3d();
    refreshLayout();
    refreshSweeps();
    refreshSummary();
}

namespace {

PatternComponent componentFrom(const QComboBox *c)
{
    for (PatternComponent p : patternComponents())
        if (patternComponentId(p) == comboId(c))
            return p;
    return PatternComponent::Total;
}

} // namespace

void ArrayWorkspace::refreshCuts()
{
    const ArrayResults &r = *m_results;
    const PatternComponent component = componentFrom(m_component);
    const CoPolarisation co = comboId(m_copol) == QLatin1String("y") ? CoPolarisation::Y : CoPolarisation::X;
    std::vector<CutTrace> traces;
    const bool cylinder = r.design.geometry.kind == LatticeKind::Cylinder;
    const QString mode = comboId(m_cutMode);
    if (mode == QLatin1String("horizon")) {
        traces.push_back({&r.horizon, tr("theta = 90 (horizon)")});
        if (m_showIdeal->isChecked() && !r.idealHorizon.fields.empty())
            traces.push_back({&r.idealHorizon, tr("Ideal, theta = 90"), true});
        m_cutPlot->setTitle(tr("Azimuth cut at the horizon (phi from +x towards +y)"));
        plotCuts(m_cutPlot, traces, component, co, m_polar->isChecked(), m_range->value(), QStringLiteral("dBi"), false);
        return;
    }
    const bool beamCuts = mode == QLatin1String("beam")
                          || (mode != QLatin1String("planes") && (cylinder || r.design.excitation.steerThetaDeg > 0.01));
    if (beamCuts) {
        traces.push_back({&r.metrics.cut1, tr("Through the beam, plane 1")});
        traces.push_back({&r.metrics.cut2, tr("Through the beam, plane 2")});
        if (m_showIdeal->isChecked() && !r.idealSteer.fields.empty()) {
            traces.push_back({&r.idealSteer, tr("Ideal, plane 1"), true});
            traces.push_back({&r.cutSteer, tr("Ideal, plane 2"), true});
        }
        m_cutPlot->setTitle(tr("Cuts through the beam (angle from the peak)"));
    } else {
        traces.push_back({&r.cutX, tr("phi = 0 (xz-plane)")});
        if (r.design.geometry.kind != LatticeKind::Linear)
            traces.push_back({&r.cutY, tr("phi = 90 (yz-plane)")});
        if (m_showIdeal->isChecked() && !r.idealX.fields.empty()) {
            traces.push_back({&r.idealX, tr("Ideal, phi = 0"), true});
            if (r.design.geometry.kind != LatticeKind::Linear)
                traces.push_back({&r.idealY, tr("Ideal, phi = 90"), true});
        }
        m_cutPlot->setTitle(tr("Principal planes (signed theta)"));
    }
    plotCuts(m_cutPlot, traces, component, co, m_polar->isChecked(), m_range->value(), QStringLiteral("dBi"));
}

void ArrayWorkspace::refreshUv()
{
    const ArrayResults &r = *m_results;
    const UvGrid &uv = r.uv;
    m_uvPlot->clearOverlays();
    if (uv.size == 0) {
        m_uvPlot->clearImage();
        return;
    }
    const PatternComponent component = componentFrom(m_component);
    const CoPolarisation co = comboId(m_copol) == QLatin1String("y") ? CoPolarisation::Y : CoPolarisation::X;
    std::vector<double> values(uv.fields.size(), NAN);
    double peak = -400;
    for (std::size_t k = 0; k < values.size(); ++k)
        if (uv.valid[k]) {
            values[k] = powerDb(componentPower(uv.fields[k], uv.phi[k], component, co));
            peak = std::max(peak, values[k]);
        }
    for (double &v : values)
        if (std::isfinite(v))
            v = std::max(v, peak - m_range->value());
    m_uvPlot->setImage(uv.size, uv.size, -1, 1, -1, 1, values);
    m_uvPlot->setColorBar(patternComponentName(component), QStringLiteral("dBi"));
    m_uvPlot->setColorRange(peak - m_range->value(), peak);
    const bool cylinder = r.design.geometry.kind == LatticeKind::Cylinder;
    m_uvPlot->setTitle(cylinder ? tr("UV plane (u along y, v along z, boresight +x)") : tr("UV plane (u = sin θ cos φ, v = sin θ sin φ)"));
    ColorMapOverlayStyle edge;
    m_uvPlot->addCircle({0, 0}, 1.0, edge);
    const Vec3 steer = radialUnit(toRadians(r.design.excitation.steerThetaDeg), toRadians(r.design.excitation.steerPhiDeg));
    double u0 = steer.x(), v0 = steer.y();
    if (cylinder) {
        u0 = steer.y();
        v0 = steer.z();
    }
    m_uvPlot->addMarker({u0, v0}, tr("beam"));
    if (r.lobes) {
        ColorMapOverlayStyle dashed;
        dashed.style = Qt::DashLine;
        dashed.width = 1.0;
        for (const Eigen::Vector2d &c : r.lobes->centres) {
            const QPointF p(u0 + c.x(), v0 + c.y());
            if (r.lobes->line) {
                if (std::abs(p.x()) < 2.0)
                    m_uvPlot->addPolyline({QPointF(p.x(), -1.2), QPointF(p.x(), 1.2)}, dashed);
            } else if (std::hypot(p.x(), p.y()) < 2.0) {
                m_uvPlot->addCircle(p, 1.0, dashed);
            }
        }
        for (const Eigen::Vector2d &g : visibleGratingLobes(*r.lobes, u0, v0))
            m_uvPlot->addMarker({g.x(), g.y()}, tr("grating lobe"));
    }
    m_uvPlot->setRange(-1.05, 1.05, -1.05, 1.05);
}

void ArrayWorkspace::refresh3d()
{
    const ArrayResults &r = *m_results;
    if (r.sphere.fields.empty()) {
        m_view3d->clearPattern();
        return;
    }
    const PatternComponent component = componentFrom(m_component);
    const CoPolarisation co = comboId(m_copol) == QLatin1String("y") ? CoPolarisation::Y : CoPolarisation::X;
    show3d(m_view3d, r.sphere, component, co, m_range->value(), patternComponentName(component), QStringLiteral("dBi"));
    std::vector<Scene3DPoint> points;
    const auto &elements = r.evaluation.layout.elements;
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const Vec3 &p = elements[i].position;
        Scene3DPoint point;
        point.position = QVector3D(float(p.x()), float(p.y()), float(p.z()));
        if (r.evaluation.excitation.failed[i])
            point.color = QColor(220, 60, 60);
        point.size = elements.size() > 400 ? 4.0f : 7.0f;
        points.push_back(point);
    }
    m_view3d->setPoints(points);
}

void ArrayWorkspace::refreshLayout()
{
    if (!m_results)
        return;
    const ArrayResults &r = *m_results;
    const auto &elements = r.evaluation.layout.elements;
    const auto &w = r.evaluation.excitation.weights;
    const bool cylinder = r.design.geometry.kind == LatticeKind::Cylinder;
    const bool phase = comboId(m_layoutValue) == QLatin1String("phase");
    std::vector<QPointF> positions;
    std::vector<double> values;
    std::vector<int> crossed;
    double peak = 0;
    for (const cplx &x : w)
        peak = std::max(peak, std::abs(x));
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const Vec3 &p = elements[i].position;
        positions.emplace_back(cylinder ? r.evaluation.layout.cylinderRadius * std::atan2(p.y(), p.x()) : p.x(),
                               cylinder ? p.z() : p.y());
        if (r.evaluation.excitation.failed[i]) {
            crossed.push_back(int(i));
            values.push_back(NAN);
        } else if (phase) {
            values.push_back(toDegrees(std::arg(w[i])));
        } else {
            values.push_back(peak > 0 && std::abs(w[i]) > 0 ? 20 * std::log10(std::abs(w[i]) / peak) : NAN);
        }
    }
    const double pitch = std::max(r.evaluation.layout.pitchX, 1e-6);
    m_layoutPlot->setScatter(positions, values, pitch * 0.7);
    m_layoutPlot->setScatterCrossed(crossed);
    m_layoutPlot->setColorMap(phase ? ColorMap::Phase : ColorMap::Viridis);
    m_layoutPlot->setColorBar(phase ? tr("Phase") : tr("Amplitude"), phase ? QStringLiteral("deg") : QStringLiteral("dB"));
    if (phase)
        m_layoutPlot->setColorRange(-180, 180);
    else
        m_layoutPlot->setColorAutoRange();
    m_layoutPlot->setXAxis(cylinder ? tr("arc") : QStringLiteral("x"), QStringLiteral("m"));
    m_layoutPlot->setYAxis(cylinder ? QStringLiteral("z") : QStringLiteral("y"), QStringLiteral("m"));
    m_layoutPlot->setTitle(tr("Element excitation (click an element to fail or restore it)"));
    m_layoutPlot->clearOverlays();
    // Sub-array outlines: their centres.
    for (const Vec3 &c : r.evaluation.excitation.subarrayCentres) {
        ColorMapOverlayStyle s;
        s.width = 1.0;
        m_layoutPlot->addMarker(QPointF(cylinder ? r.evaluation.layout.cylinderRadius * std::atan2(c.y(), c.x()) : c.x(),
                                        cylinder ? c.z() : c.y()),
                                QString(), s);
    }
    m_layoutPlot->autoscale();
}

void ArrayWorkspace::refreshSweeps()
{
    const ArrayResults &r = *m_results;
    m_scanPlot->clear();
    if (!r.scan.empty()) {
        std::vector<double> x, gain, peak, eirp, gt;
        for (const ScanPoint &p : r.scan) {
            x.push_back(p.scanDeg);
            gain.push_back(p.gainAtSteerDbi);
            peak.push_back(p.peakGainDbi);
            eirp.push_back(p.eirpDbw);
            gt.push_back(p.gOverTDbK);
        }
        m_scanPlot->setXAxis(tr("Scan angle in the phi = %1 plane").arg(r.design.sweeps.scanPlanePhiDeg), QStringLiteral("deg"));
        m_scanPlot->setYAxis(tr("Gain and G/T"), QStringLiteral("dB"));
        m_scanPlot->setYAxis(tr("EIRP"), QStringLiteral("dBW"), false, PlotYAxis::Right);
        m_scanPlot->setTraceData(m_scanPlot->addTrace(tr("Gain at steering (dBi)"), QStringLiteral("dB")), x, gain);
        TraceStyle dashed;
        dashed.lineStyle = Qt::DashLine;
        m_scanPlot->setTraceData(m_scanPlot->addTrace(tr("Peak gain (dBi)"), QStringLiteral("dB"), dashed), x, peak);
        m_scanPlot->setTraceData(m_scanPlot->addTrace(tr("G/T (dB/K)"), QStringLiteral("dB")), x, gt);
        TraceStyle right;
        right.yAxis = PlotYAxis::Right;
        m_scanPlot->setTraceData(m_scanPlot->addTrace(tr("EIRP (dBW)"), QStringLiteral("dBW"), right), x, eirp);
        m_scanPlot->autoscale();
    }
    m_bandPlot->clear();
    if (!r.band.empty()) {
        std::vector<double> f, gain, squint;
        for (const BandPoint &p : r.band) {
            f.push_back(p.frequency);
            gain.push_back(p.gainAtSteerDbi);
            squint.push_back(p.squintDeg);
        }
        m_bandPlot->setXAxis(tr("Frequency"), QStringLiteral("Hz"));
        m_bandPlot->setYAxis(tr("Gain at steering"), QStringLiteral("dBi"));
        m_bandPlot->setYAxis(tr("Beam squint"), QStringLiteral("deg"), false, PlotYAxis::Right);
        m_bandPlot->setTraceData(m_bandPlot->addTrace(tr("Gain at steering"), QStringLiteral("dBi")), f, gain);
        TraceStyle right;
        right.yAxis = PlotYAxis::Right;
        m_bandPlot->setTraceData(m_bandPlot->addTrace(tr("Squint"), QStringLiteral("deg"), right), f, squint);
        m_bandPlot->autoscale();
    }
}

void ArrayWorkspace::refreshSummary()
{
    const ArrayResults &r = *m_results;
    const BeamMetrics &m = r.metrics;
    QList<QPair<QString, QString>> rows;
    const auto &failed = r.evaluation.excitation.failed;
    const int total = int(r.evaluation.layout.elements.size());
    const int active = total - int(std::count(failed.begin(), failed.end(), true));
    auto deg = [](double v) { return std::isfinite(v) ? QStringLiteral("%1 deg").arg(v, 0, 'f', 2) : QStringLiteral("-"); };
    rows << qMakePair(tr("Elements"), tr("%1 active of %2").arg(active).arg(total));
    rows << qMakePair(tr("Frequency"), formatFrequency(r.design.frequency));
    if (!r.error.isEmpty())
        rows << qMakePair(tr("Error"), r.error);
    rows << qMakePair(tr("Peak gain"), formatDb(m.peakGainDbi, QStringLiteral("dBi")));
    rows << qMakePair(tr("Peak direction"), tr("theta %1, phi %2").arg(deg(m.peakThetaDeg), deg(m.peakPhiDeg)));
    rows << qMakePair(tr("Gain at the steering direction"), formatDb(m.gainAtSteerDbi, QStringLiteral("dBi")));
    rows << qMakePair(tr("Pointing error"), deg(m.pointingErrorDeg));
    rows << qMakePair(tr("Integrated directivity"), formatDb(m.directivityDbi, QStringLiteral("dBi")));
    rows << qMakePair(tr("Half-power beamwidth"), tr("%1 (plane 1), %2 (plane 2)").arg(deg(m.beamwidth1Deg), deg(m.beamwidth2Deg)));
    rows << qMakePair(tr("First-null beamwidth"), tr("%1, %2").arg(deg(m.nullWidth1Deg), deg(m.nullWidth2Deg)));
    rows << qMakePair(tr("Side-lobe level in the cuts"),
                      tr("%1, %2").arg(formatDb(m.sideLobe1Db, QStringLiteral("dB")), formatDb(m.sideLobe2Db, QStringLiteral("dB"))));
    rows << qMakePair(tr("Side-lobe level over UV"), formatDb(m.sideLobeDb, QStringLiteral("dB")));
    rows << qMakePair(tr("Cross-polar level at the peak"), formatDb(m.crossPolDb, QStringLiteral("dB")));
    rows << qMakePair(tr("Axial ratio at the peak"), formatDb(m.axialRatioDb, QStringLiteral("dB")));
    if (std::isfinite(r.gratingFreeScan0))
        rows << qMakePair(tr("Scan without grating lobes"),
                          tr("%1 (phi = 0), %2 (phi = 90)").arg(deg(r.gratingFreeScan0), deg(r.gratingFreeScan90)));
    if (!r.scan.empty()) {
        const ScanPoint *broadside = nullptr, *edge = &r.scan.back();
        for (const ScanPoint &p : r.scan)
            if (!broadside || std::abs(p.scanDeg) < std::abs(broadside->scanDeg))
                broadside = &p;
        rows << qMakePair(tr("Scan loss at %1").arg(deg(edge->scanDeg)),
                          formatDb(broadside->gainAtSteerDbi - edge->gainAtSteerDbi, QStringLiteral("dB")));
    }
    rows << qMakePair(tr("EIRP at the steering direction"), formatDb(r.budget.eirpDbw, QStringLiteral("dBW")));
    rows << qMakePair(tr("G/T at the steering direction"), formatDb(r.budget.gOverTDbK, QStringLiteral("dB/K")));
    rows << qMakePair(tr("System temperature"), QStringLiteral("%1 K").arg(r.budget.systemTemperatureK, 0, 'f', 1));
    rows << qMakePair(tr("Total PA output"), QStringLiteral("%1 W").arg(r.budget.totalPowerW, 0, 'g', 5));
    rows << qMakePair(tr("Transmit values"), tr("%1: %2 dBm, %3 dB").arg(r.tx.origin).arg(r.tx.powerDbm, 0, 'f', 2).arg(r.tx.gainDb, 0, 'f', 2));
    rows << qMakePair(tr("Receive values"), tr("%1: NF %2 dB, gain %3 dB").arg(r.rx.origin).arg(r.rx.nfDb, 0, 'f', 2).arg(r.rx.gainDb, 0, 'f', 2));
    if (r.evaluation.excitation.rmsPhaseQuantisationDeg > 0)
        rows << qMakePair(tr("RMS phase quantisation error"), deg(r.evaluation.excitation.rmsPhaseQuantisationDeg));
    QStringList warnings = r.warnings;
    warnings.removeDuplicates();
    for (const QString &w : std::as_const(warnings))
        rows << qMakePair(tr("Warning"), w);
    m_summary->setRowCount(int(rows.size()));
    for (int i = 0; i < int(rows.size()); ++i) {
        m_summary->setItem(i, 0, new QTableWidgetItem(rows[i].first));
        m_summary->setItem(i, 1, new QTableWidgetItem(rows[i].second));
    }
}

} // namespace lvd::antui

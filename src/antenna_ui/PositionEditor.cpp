#include "antenna_ui/PositionEditor.h"

#include "plot/PlotAxis.h"
#include "plot/PlotTheme.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSplitter>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lvd::antui {

using namespace ant;

namespace {

enum Column { ColNumber, ColX, ColY, ColZ, ColNx, ColNy, ColNz, ColRotation, ColAmplitude, ColPhase, ColOn, ColumnCount };

Vec3 normalOf(const CustomElement &e)
{
    const Vec3 n(e.nx, e.ny, e.nz);
    return n.norm() > 1e-12 ? Vec3(n.normalized()) : Vec3(0, 0, 1);
}

// Sets the normal and the rotation from a new frame (normal n, x axis x).
void setFrame(CustomElement &e, const Vec3 &n, const Vec3 &x)
{
    const Vec3 nn = n.normalized();
    auto clean = [](double v) { return std::abs(v) < 1e-12 ? 0.0 : v; };
    e.nx = clean(nn.x());
    e.ny = clean(nn.y());
    e.nz = clean(nn.z());
    const Mat3 reference = orientationFromNormal(nn, 0);
    const double r = toDegrees(std::atan2(x.dot(reference.col(1)), x.dot(reference.col(0))));
    e.rotationDeg = std::abs(r) < 1e-9 ? 0.0 : r;
}

// Index of the axis normal to a view plane, and of its two in-plane axes.
void planeAxes(LayoutPlane plane, int *h, int *v, int *n)
{
    switch (plane) {
    case LayoutPlane::XY:
        *h = 0, *v = 1, *n = 2;
        break;
    case LayoutPlane::XZ:
        *h = 0, *v = 2, *n = 1;
        break;
    case LayoutPlane::YZ:
        *h = 1, *v = 2, *n = 0;
        break;
    }
}

double &coordinate(CustomElement &e, int axis)
{
    return axis == 0 ? e.x : axis == 1 ? e.y : e.z;
}

double coordinate(const CustomElement &e, int axis)
{
    return axis == 0 ? e.x : axis == 1 ? e.y : e.z;
}

QString number(double v)
{
    return QString::number(std::abs(v) < 1e-12 ? 0.0 : v, 'g', 7);
}

} // namespace

// ---- LayoutCanvas

LayoutCanvas::LayoutCanvas(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(320, 260);
}

void LayoutCanvas::setElements(std::vector<CustomElement> *elements, double displayScale, const QString &unit)
{
    m_elements = elements;
    m_scale = displayScale;
    m_unit = unit;
    m_fitted = false;
    update();
}

void LayoutCanvas::setPlane(LayoutPlane plane)
{
    m_plane = plane;
    fitView();
}

void LayoutCanvas::setGrid(double step, bool snap)
{
    m_grid = std::max(step, 1e-9);
    m_snap = snap;
    update();
}

double LayoutCanvas::snapValue(double v) const
{
    return m_snap ? std::round(v / m_grid) * m_grid : v;
}

void LayoutCanvas::setSelection(const std::set<int> &selection)
{
    if (selection == m_selection)
        return;
    m_selection = selection;
    update();
    Q_EMIT selectionChanged();
}

QPointF LayoutCanvas::planePoint(const CustomElement &e) const
{
    int h, v, n;
    planeAxes(m_plane, &h, &v, &n);
    return {coordinate(e, h) * m_scale, coordinate(e, v) * m_scale};
}

void LayoutCanvas::setPlanePoint(CustomElement &e, QPointF p) const
{
    int h, v, n;
    planeAxes(m_plane, &h, &v, &n);
    coordinate(e, h) = p.x() / m_scale;
    coordinate(e, v) = p.y() / m_scale;
}

QPointF LayoutCanvas::toPixel(QPointF p) const
{
    return {width() / 2.0 + (p.x() - m_centre.x()) * m_pixels, height() / 2.0 - (p.y() - m_centre.y()) * m_pixels};
}

QPointF LayoutCanvas::toPlane(QPointF pixel) const
{
    return {m_centre.x() + (pixel.x() - width() / 2.0) / m_pixels, m_centre.y() - (pixel.y() - height() / 2.0) / m_pixels};
}

QPointF LayoutCanvas::elementPixel(int index) const
{
    return toPixel(planePoint((*m_elements)[std::size_t(index)]));
}

double LayoutCanvas::markerRadius() const
{
    // About a third of the typical spacing, within limits.
    double pitch = m_grid;
    if (m_elements && m_elements->size() > 1) {
        std::vector<double> nearest;
        const std::size_t n = std::min<std::size_t>(m_elements->size(), 200);
        for (std::size_t i = 0; i < n; ++i) {
            double best = INFINITY;
            const QPointF a = planePoint((*m_elements)[i]);
            for (std::size_t j = 0; j < m_elements->size(); ++j)
                if (j != i) {
                    const QPointF b = planePoint((*m_elements)[j]);
                    const double d = std::hypot(a.x() - b.x(), a.y() - b.y());
                    if (d > 1e-9)
                        best = std::min(best, d);
                }
            if (std::isfinite(best))
                nearest.push_back(best);
        }
        if (!nearest.empty()) {
            std::nth_element(nearest.begin(), nearest.begin() + long(nearest.size() / 2), nearest.end());
            pitch = nearest[nearest.size() / 2];
        }
    }
    return std::clamp(pitch * m_pixels * 0.32, 3.0, 14.0);
}

int LayoutCanvas::elementAt(QPointF pixel) const
{
    if (!m_elements)
        return -1;
    const double r = markerRadius() + 3;
    int best = -1;
    double bestDistance = r;
    for (std::size_t i = 0; i < m_elements->size(); ++i) {
        const QPointF p = toPixel(planePoint((*m_elements)[i]));
        const double d = std::hypot(p.x() - pixel.x(), p.y() - pixel.y());
        if (d <= bestDistance) {
            bestDistance = d;
            best = int(i);
        }
    }
    return best;
}

void LayoutCanvas::fitView()
{
    if (!m_elements || m_elements->empty() || width() <= 0) {
        m_centre = {};
        m_pixels = std::min(width(), height()) / (8 * m_grid + 1e-9);
        update();
        return;
    }
    double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
    for (const CustomElement &e : *m_elements) {
        const QPointF p = planePoint(e);
        x0 = std::min(x0, p.x());
        x1 = std::max(x1, p.x());
        y0 = std::min(y0, p.y());
        y1 = std::max(y1, p.y());
    }
    const double margin = std::max({x1 - x0, y1 - y0, 2 * m_grid}) * 0.12 + m_grid;
    m_centre = {(x0 + x1) / 2, (y0 + y1) / 2};
    const double w = x1 - x0 + 2 * margin, h = y1 - y0 + 2 * margin;
    m_pixels = std::min((width() - 60) / w, (height() - 40) / h);
    m_fitted = true;
    update();
}

void LayoutCanvas::refresh()
{
    if (m_elements)
        for (auto it = m_selection.begin(); it != m_selection.end();)
            it = *it >= int(m_elements->size()) ? m_selection.erase(it) : std::next(it);
    update();
}

void LayoutCanvas::resizeEvent(QResizeEvent *)
{
    if (!m_fitted)
        fitView();
}

void LayoutCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const PlotTheme theme = PlotTheme::fromPalette(palette());
    p.fillRect(rect(), theme.background);

    // Grid: the snap grid, or a coarser multiple when it is too dense.
    const QPointF lo = toPlane(QPointF(0, height())), hi = toPlane(QPointF(width(), 0));
    double step = m_grid;
    while (step * m_pixels < 8)
        step *= 5;
    p.setPen(QPen(theme.minorGrid, 1));
    for (double x = std::floor(lo.x() / step) * step; x <= hi.x(); x += step)
        p.drawLine(toPixel({x, lo.y()}), toPixel({x, hi.y()}));
    for (double y = std::floor(lo.y() / step) * step; y <= hi.y(); y += step)
        p.drawLine(toPixel({lo.x(), y}), toPixel({hi.x(), y}));
    p.setPen(QPen(theme.axis, 1));
    p.drawLine(toPixel({lo.x(), 0}), toPixel({hi.x(), 0}));
    p.drawLine(toPixel({0, lo.y()}), toPixel({0, hi.y()}));

    // Tick labels on the major grid.
    const AxisTicks ticksX = linearTicks(lo.x(), hi.x(), std::max(2, width() / 90));
    const AxisTicks ticksY = linearTicks(lo.y(), hi.y(), std::max(2, height() / 60));
    p.setPen(theme.mutedText);
    for (double x : ticksX.major)
        p.drawText(QRectF(toPixel({x, 0}).x() - 40, height() - 18, 80, 16), Qt::AlignCenter, QString::number(x, 'g', 4));
    for (double y : ticksY.major)
        if (toPixel({0, y}).y() > 26) // keep clear of the axis label
            p.drawText(QRectF(2, toPixel({0, y}).y() - 8, 60, 16), Qt::AlignLeft | Qt::AlignVCenter, QString::number(y, 'g', 4));
    static const char *names[] = {"x", "y", "z"};
    int h, v, n;
    planeAxes(m_plane, &h, &v, &n);
    p.setPen(theme.text);
    p.drawText(QRectF(width() - 70, height() - 34, 66, 16), Qt::AlignRight,
               QStringLiteral("%1 (%2) →").arg(QLatin1String(names[h]), m_unit));
    p.drawText(QRectF(4, 4, 120, 16), Qt::AlignLeft, QStringLiteral("↑ %1 (%2)").arg(QLatin1String(names[v]), m_unit));

    if (!m_elements)
        return;
    const double r = markerRadius();
    QFont small = font();
    small.setPointSizeF(std::max(6.0, r * 0.75));
    p.setFont(small);
    const QColor on = theme.seriesColor(0);
    for (std::size_t i = 0; i < m_elements->size(); ++i) {
        const CustomElement &e = (*m_elements)[i];
        const QPointF c = toPixel(planePoint(e));
        const bool selected = m_selection.count(int(i)) > 0;
        // Boresight direction projected on the plane.
        const Vec3 nrm = normalOf(e);
        const double nh = h == 0 ? nrm.x() : h == 1 ? nrm.y() : nrm.z();
        const double nv = v == 0 ? nrm.x() : v == 1 ? nrm.y() : nrm.z();
        if (std::hypot(nh, nv) > 0.05) {
            p.setPen(QPen(theme.mutedText, 1.2));
            p.drawLine(c, c + QPointF(nh, -nv) * (r * 2.2));
        }
        p.setPen(QPen(selected ? theme.seriesColor(1) : theme.text, selected ? 2.5 : 1));
        p.setBrush(e.enabled ? QBrush(on) : Qt::NoBrush);
        p.drawEllipse(c, r, r);
        if (r >= 9) {
            p.setPen(e.enabled ? Qt::white : theme.text);
            p.drawText(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), Qt::AlignCenter, QString::number(i + 1));
        }
    }
    if (m_drag == Drag::Band) {
        p.setPen(QPen(theme.text, 1, Qt::DashLine));
        p.setBrush(theme.rubberBand);
        p.drawRect(QRectF(m_press, m_last).normalized());
    }
}

void LayoutCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    m_press = m_last = event->position();
    m_moved = false;
    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        m_drag = Drag::Pan;
        return;
    }
    if (event->button() != Qt::LeftButton || !m_elements)
        return;
    const int hit = elementAt(event->position());
    const bool add = event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    if (hit >= 0) {
        std::set<int> s = m_selection;
        if (add) {
            if (!s.erase(hit))
                s.insert(hit);
        } else if (!s.count(hit)) {
            s = {hit};
        }
        setSelection(s);
        if (s.count(hit)) {
            m_drag = Drag::Move;
            m_anchor = hit;
            m_anchorStart = planePoint((*m_elements)[std::size_t(hit)]);
            m_startPoints.clear();
            for (std::size_t i = 0; i < m_elements->size(); ++i)
                m_startPoints.push_back(planePoint((*m_elements)[i]));
        }
    } else {
        if (!add)
            setSelection({});
        m_drag = Drag::Band;
    }
}

void LayoutCanvas::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    switch (m_drag) {
    case Drag::Pan:
        m_centre -= QPointF((pos.x() - m_last.x()) / m_pixels, -(pos.y() - m_last.y()) / m_pixels);
        m_fitted = true;
        update();
        break;
    case Drag::Move: {
        if (!m_moved && QLineF(pos, m_press).length() < 3)
            break;
        m_moved = true;
        // The grabbed element snaps to the grid; the others keep their offsets.
        const QPointF delta = toPlane(pos) - toPlane(m_press);
        const QPointF target(snapValue(m_anchorStart.x() + delta.x()), snapValue(m_anchorStart.y() + delta.y()));
        const QPointF shift = target - m_anchorStart;
        for (int i : m_selection)
            setPlanePoint((*m_elements)[std::size_t(i)], m_startPoints[std::size_t(i)] + shift);
        update();
        break;
    }
    case Drag::Band:
        update();
        break;
    case Drag::None:
        break;
    }
    m_last = pos;
}

void LayoutCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_drag == Drag::Move && m_moved)
        Q_EMIT elementsMoved();
    if (m_drag == Drag::Band && m_elements) {
        const QRectF band = QRectF(m_press, event->position()).normalized();
        std::set<int> s = (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) ? m_selection : std::set<int>{};
        for (std::size_t i = 0; i < m_elements->size(); ++i)
            if (band.contains(toPixel(planePoint((*m_elements)[i]))))
                s.insert(int(i));
        setSelection(s);
    }
    m_drag = Drag::None;
    update();
}

void LayoutCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && elementAt(event->position()) < 0) {
        const QPointF p = toPlane(event->position());
        Q_EMIT addRequested(QPointF(snapValue(p.x()), snapValue(p.y())));
    }
}

void LayoutCanvas::wheelEvent(QWheelEvent *event)
{
    const QPointF before = toPlane(event->position());
    m_pixels *= std::pow(1.2, event->angleDelta().y() / 120.0);
    m_pixels = std::clamp(m_pixels, 1e-3, 1e7);
    const QPointF after = toPlane(event->position());
    m_centre += before - after;
    m_fitted = true;
    update();
}

void LayoutCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        Q_EMIT deleteRequested();
    else if (event->key() == Qt::Key_Home)
        fitView();
    else if (event->matches(QKeySequence::SelectAll) && m_elements) {
        std::set<int> all;
        for (std::size_t i = 0; i < m_elements->size(); ++i)
            all.insert(int(i));
        setSelection(all);
    } else
        QWidget::keyPressEvent(event);
}

// ---- PositionEditor

PositionEditor::PositionEditor(const GeometrySpec &geometry, double designFrequency, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Edit Element Positions"));
    resize(1150, 720);
    m_specUnit = geometry.unit;
    m_scale = geometry.unit == QLatin1String("m") ? 1000.0 : 1.0;
    m_elements = toCustomElements(geometry, designFrequency);

    auto *layout = new QVBoxLayout(this);
    if (geometry.kind != LatticeKind::Custom) {
        auto *note = new QLabel(tr("The %1 geometry has been converted to an element table. With OK the array becomes a "
                                   "custom table; its taper and steering settings still apply.")
                                    .arg(latticeName(geometry.kind).toLower()));
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    auto *splitter = new QSplitter(Qt::Horizontal);
    // Left: canvas and its options.
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    auto *options = new QHBoxLayout;
    m_plane = new QComboBox;
    m_plane->addItem(tr("Top view (x, y)"));
    m_plane->addItem(tr("Side view (x, z)"));
    m_plane->addItem(tr("Side view (y, z)"));
    m_grid = new QDoubleSpinBox;
    m_grid->setDecimals(4);
    m_grid->setRange(1e-4, 1e5);
    m_grid->setSuffix(QLatin1Char(' ') + unitLabel());
    m_grid->setValue(geometry.unit == QLatin1String("m") ? 5.0 : 0.25);
    m_snap = new QCheckBox(tr("Snap"));
    m_snap->setChecked(true);
    auto *fit = new QPushButton(tr("Fit"));
    options->addWidget(m_plane);
    options->addWidget(new QLabel(tr("Grid")));
    options->addWidget(m_grid);
    options->addWidget(m_snap);
    options->addStretch();
    options->addWidget(fit);
    lv->addLayout(options);
    m_canvas = new LayoutCanvas;
    m_canvas->setElements(&m_elements, m_scale, unitLabel());
    m_canvas->setGrid(m_grid->value(), true);
    lv->addWidget(m_canvas, 1);
    auto *hint = new QLabel(tr("Drag to move, drag on empty space to select, Ctrl+click to add to the selection, "
                               "double-click to add an element, Delete to delete; wheel zooms, right-drag pans, Home fits."));
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);
    lv->addWidget(hint);
    splitter->addWidget(left);

    // Right: the table.
    m_table = new QTableWidget(0, ColumnCount);
    m_table->setHorizontalHeaderLabels({tr("#"), QStringLiteral("x (%1)").arg(unitLabel()), QStringLiteral("y (%1)").arg(unitLabel()),
                                        QStringLiteral("z (%1)").arg(unitLabel()), tr("nx"), tr("ny"), tr("nz"), tr("Rotation °"),
                                        tr("Amplitude dB"), tr("Phase °"), tr("On")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    splitter->addWidget(m_table);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    // Tools.
    auto *tools = new QHBoxLayout;
    auto button = [&](const QString &text, auto slot) {
        auto *b = new QPushButton(text);
        tools->addWidget(b);
        connect(b, &QPushButton::clicked, this, slot);
        return b;
    };
    button(tr("Add"), [this] {
        const QPointF centre = m_canvas->toPlane(QPointF(m_canvas->width() / 2.0, m_canvas->height() / 2.0));
        addElement(QPointF(m_canvas->snapValue(centre.x()), m_canvas->snapValue(centre.y())));
    });
    button(tr("Delete"), [this] { deleteSelected(); });
    button(tr("Duplicate"), [this] { duplicateSelected(); });
    button(tr("Move..."), [this] {
        QDialog d(this);
        d.setWindowTitle(tr("Move Selection"));
        auto *form = new QFormLayout(&d);
        QDoubleSpinBox *dx[3];
        for (int i = 0; i < 3; ++i) {
            dx[i] = new QDoubleSpinBox;
            dx[i]->setDecimals(4);
            dx[i]->setRange(-1e6, 1e6);
            dx[i]->setSuffix(QLatin1Char(' ') + unitLabel());
            form->addRow(QStringLiteral("d%1").arg(QLatin1Char("xyz"[i])), dx[i]);
        }
        auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(box);
        connect(box, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(box, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() == QDialog::Accepted)
            moveSelected(dx[0]->value(), dx[1]->value(), dx[2]->value());
    });
    button(tr("Rotate..."), [this] {
        QDialog d(this);
        d.setWindowTitle(tr("Rotate Selection"));
        auto *form = new QFormLayout(&d);
        auto *angle = new QDoubleSpinBox;
        angle->setRange(-360, 360);
        angle->setSuffix(QStringLiteral(" °"));
        form->addRow(tr("About the axis normal to the view, through the selection centre:"), angle);
        auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(box);
        connect(box, &QDialogButtonBox::accepted, &d, &QDialog::accept);
        connect(box, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() == QDialog::Accepted)
            rotateSelected(angle->value());
    });
    button(tr("Mirror ↔"), [this] { mirrorSelected(true); });
    button(tr("Mirror ↕"), [this] { mirrorSelected(false); });
    button(tr("On/Off"), [this] { toggleSelected(); });
    tools->addSpacing(12);
    button(tr("Import CSV..."), [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Element Table"), QString(),
                                                          tr("CSV or text (*.csv *.txt);;All files (*)"));
        QString error;
        if (!path.isEmpty() && !importCsv(path, &error))
            QMessageBox::warning(this, tr("Import Element Table"), error);
    });
    button(tr("Export CSV..."), [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Export Element Table"), QStringLiteral("elements.csv"),
                                                          tr("CSV (*.csv)"));
        QString error;
        if (!path.isEmpty() && !exportCsv(path, &error))
            QMessageBox::warning(this, tr("Export Element Table"), error);
    });
    tools->addStretch();
    layout->addLayout(tools);

    auto *bottom = new QHBoxLayout;
    m_status = new QLabel;
    bottom->addWidget(m_status, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bottom->addWidget(box);
    layout->addLayout(bottom);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *undoAction = new QAction(tr("Undo"), this);
    undoAction->setShortcut(QKeySequence::Undo);
    connect(undoAction, &QAction::triggered, this, &PositionEditor::undo);
    auto *redoAction = new QAction(tr("Redo"), this);
    redoAction->setShortcut(QKeySequence::Redo);
    connect(redoAction, &QAction::triggered, this, &PositionEditor::redo);
    addAction(undoAction);
    addAction(redoAction);

    connect(m_plane, &QComboBox::currentIndexChanged, this, [this](int i) { m_canvas->setPlane(LayoutPlane(i)); });
    connect(m_grid, &QDoubleSpinBox::valueChanged, this, [this](double v) { m_canvas->setGrid(v, m_snap->isChecked()); });
    connect(m_snap, &QCheckBox::toggled, this, [this](bool on) { m_canvas->setGrid(m_grid->value(), on); });
    connect(fit, &QPushButton::clicked, m_canvas, &LayoutCanvas::fitView);
    connect(m_canvas, &LayoutCanvas::selectionChanged, this, [this] {
        syncSelectionToTable();
        refreshStatus();
    });
    connect(m_canvas, &LayoutCanvas::elementsMoved, this, [this] {
        refreshTable();
        pushState();
    });
    connect(m_canvas, &LayoutCanvas::addRequested, this, &PositionEditor::addElement);
    connect(m_canvas, &LayoutCanvas::deleteRequested, this, &PositionEditor::deleteSelected);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_updating)
            return;
        readRow(item->row());
        m_canvas->refresh();
        pushState();
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &PositionEditor::syncSelectionFromTable);

    m_history = {m_elements};
    m_historyIndex = 0;
    refreshTable();
    refreshStatus();
}

QString PositionEditor::unitLabel() const
{
    return m_specUnit == QLatin1String("m") ? QStringLiteral("mm") : QStringLiteral("λ");
}

void PositionEditor::refreshTable()
{
    m_updating = true;
    m_table->setRowCount(int(m_elements.size()));
    for (int i = 0; i < int(m_elements.size()); ++i) {
        const CustomElement &e = m_elements[std::size_t(i)];
        auto set = [&](int col, const QString &text) {
            QTableWidgetItem *item = m_table->item(i, col);
            if (!item) {
                item = new QTableWidgetItem;
                m_table->setItem(i, col, item);
            }
            item->setText(text);
        };
        set(ColNumber, QString::number(i + 1));
        m_table->item(i, ColNumber)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        set(ColX, number(e.x * m_scale));
        set(ColY, number(e.y * m_scale));
        set(ColZ, number(e.z * m_scale));
        set(ColNx, number(e.nx));
        set(ColNy, number(e.ny));
        set(ColNz, number(e.nz));
        set(ColRotation, number(e.rotationDeg));
        set(ColAmplitude, number(e.amplitudeDb));
        set(ColPhase, number(e.phaseDeg));
        QTableWidgetItem *on = m_table->item(i, ColOn);
        if (!on) {
            on = new QTableWidgetItem;
            on->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
            m_table->setItem(i, ColOn, on);
        }
        on->setCheckState(e.enabled ? Qt::Checked : Qt::Unchecked);
    }
    m_updating = false;
    syncSelectionToTable();
}

void PositionEditor::readRow(int row)
{
    if (row < 0 || row >= int(m_elements.size()))
        return;
    CustomElement &e = m_elements[std::size_t(row)];
    auto value = [&](int col, double fallback) {
        bool ok = false;
        const double v = m_table->item(row, col) ? m_table->item(row, col)->text().toDouble(&ok) : 0.0;
        return ok ? v : fallback;
    };
    e.x = value(ColX, e.x * m_scale) / m_scale;
    e.y = value(ColY, e.y * m_scale) / m_scale;
    e.z = value(ColZ, e.z * m_scale) / m_scale;
    e.nx = value(ColNx, e.nx);
    e.ny = value(ColNy, e.ny);
    e.nz = value(ColNz, e.nz);
    if (std::hypot(e.nx, e.ny, e.nz) < 1e-12) {
        e.nx = e.ny = 0;
        e.nz = 1;
    }
    e.rotationDeg = value(ColRotation, e.rotationDeg);
    e.amplitudeDb = value(ColAmplitude, e.amplitudeDb);
    e.phaseDeg = value(ColPhase, e.phaseDeg);
    if (QTableWidgetItem *on = m_table->item(row, ColOn))
        e.enabled = on->checkState() == Qt::Checked;
}

void PositionEditor::syncSelectionToTable()
{
    m_updating = true;
    m_table->clearSelection();
    const std::set<int> &s = m_canvas->selection();
    for (int i : s)
        if (i < m_table->rowCount())
            m_table->selectionModel()->select(m_table->model()->index(i, 0),
                                              QItemSelectionModel::Select | QItemSelectionModel::Rows);
    if (!s.empty() && *s.begin() < m_table->rowCount())
        m_table->scrollToItem(m_table->item(*s.begin(), ColNumber));
    m_updating = false;
}

void PositionEditor::syncSelectionFromTable()
{
    if (m_updating)
        return;
    std::set<int> s;
    for (const QModelIndex &index : m_table->selectionModel()->selectedRows())
        s.insert(index.row());
    m_updating = true;
    m_canvas->setSelection(s);
    m_updating = false;
    refreshStatus();
}

void PositionEditor::refreshStatus()
{
    const auto on = std::count_if(m_elements.begin(), m_elements.end(), [](const CustomElement &e) { return e.enabled; });
    m_status->setText(tr("%1 elements (%2 on), %3 selected").arg(m_elements.size()).arg(on).arg(m_canvas->selection().size()));
}

void PositionEditor::pushState()
{
    m_history.resize(m_historyIndex + 1);
    if (m_history.back().size() == m_elements.size()
        && std::equal(m_elements.begin(), m_elements.end(), m_history.back().begin(), [](const CustomElement &a, const CustomElement &b) {
               return a.x == b.x && a.y == b.y && a.z == b.z && a.nx == b.nx && a.ny == b.ny && a.nz == b.nz
                      && a.rotationDeg == b.rotationDeg && a.amplitudeDb == b.amplitudeDb && a.phaseDeg == b.phaseDeg
                      && a.enabled == b.enabled;
           }))
        return;
    m_history.push_back(m_elements);
    m_historyIndex = m_history.size() - 1;
    refreshStatus();
}

void PositionEditor::undo()
{
    if (m_historyIndex == 0)
        return;
    m_elements = m_history[--m_historyIndex];
    m_canvas->refresh();
    refreshTable();
    refreshStatus();
}

void PositionEditor::redo()
{
    if (m_historyIndex + 1 >= m_history.size())
        return;
    m_elements = m_history[++m_historyIndex];
    m_canvas->refresh();
    refreshTable();
    refreshStatus();
}

void PositionEditor::addElement(QPointF planePoint)
{
    // The new element copies the last selected (or last) element's depth,
    // orientation and excitation.
    CustomElement e;
    if (!m_canvas->selection().empty())
        e = m_elements[std::size_t(*m_canvas->selection().rbegin())];
    else if (!m_elements.empty())
        e = m_elements.back();
    e.enabled = true;
    m_canvas->setPlanePoint(e, planePoint);
    m_elements.push_back(e);
    refreshTable();
    m_canvas->setSelection({int(m_elements.size()) - 1});
    m_canvas->refresh();
    pushState();
}

void PositionEditor::deleteSelected()
{
    const std::set<int> s = m_canvas->selection();
    if (s.empty())
        return;
    for (auto it = s.rbegin(); it != s.rend(); ++it)
        m_elements.erase(m_elements.begin() + *it);
    m_canvas->setSelection({});
    m_canvas->refresh();
    refreshTable();
    pushState();
}

void PositionEditor::duplicateSelected()
{
    const std::set<int> s = m_canvas->selection();
    if (s.empty())
        return;
    std::set<int> copies;
    const double step = m_grid->value();
    for (int i : s) {
        CustomElement e = m_elements[std::size_t(i)];
        QPointF p = m_canvas->planePoint(e);
        m_canvas->setPlanePoint(e, p + QPointF(step, 0));
        m_elements.push_back(e);
        copies.insert(int(m_elements.size()) - 1);
    }
    refreshTable();
    m_canvas->setSelection(copies);
    m_canvas->refresh();
    pushState();
}

void PositionEditor::moveSelected(double dx, double dy, double dz)
{
    for (int i : m_canvas->selection()) {
        CustomElement &e = m_elements[std::size_t(i)];
        e.x += dx / m_scale;
        e.y += dy / m_scale;
        e.z += dz / m_scale;
    }
    m_canvas->refresh();
    refreshTable();
    pushState();
}

void PositionEditor::rotateSelected(double degrees)
{
    const std::set<int> s = m_canvas->selection();
    if (s.empty())
        return;
    int h, v, n;
    planeAxes(m_canvas->plane(), &h, &v, &n);
    Vec3 centre = Vec3::Zero();
    for (int i : s)
        centre += Vec3(m_elements[std::size_t(i)].x, m_elements[std::size_t(i)].y, m_elements[std::size_t(i)].z);
    centre /= double(s.size());
    Vec3 axis = Vec3::Zero();
    axis[n] = 1.0;
    // Counter-clockwise as seen in the view (right-handed about h x v).
    const Vec3 hv = Vec3::Unit(h).cross(Vec3::Unit(v));
    const Mat3 rot = Eigen::AngleAxisd(toRadians(degrees), hv).toRotationMatrix();
    for (int i : s) {
        CustomElement &e = m_elements[std::size_t(i)];
        const Mat3 frame = orientationFromNormal(normalOf(e), e.rotationDeg);
        const Vec3 p = centre + rot * (Vec3(e.x, e.y, e.z) - centre);
        e.x = p.x();
        e.y = p.y();
        e.z = p.z();
        setFrame(e, rot * frame.col(2), rot * frame.col(0));
    }
    m_canvas->refresh();
    refreshTable();
    pushState();
}

void PositionEditor::mirrorSelected(bool horizontal)
{
    const std::set<int> s = m_canvas->selection();
    if (s.empty())
        return;
    int h, v, n;
    planeAxes(m_canvas->plane(), &h, &v, &n);
    const int axis = horizontal ? h : v;
    double centre = 0;
    for (int i : s)
        centre += coordinate(m_elements[std::size_t(i)], axis);
    centre /= double(s.size());
    Mat3 mirror = Mat3::Identity();
    mirror(axis, axis) = -1;
    for (int i : s) {
        CustomElement &e = m_elements[std::size_t(i)];
        const Mat3 frame = orientationFromNormal(normalOf(e), e.rotationDeg);
        coordinate(e, axis) = 2 * centre - coordinate(e, axis);
        setFrame(e, mirror * frame.col(2), mirror * frame.col(0));
    }
    m_canvas->refresh();
    refreshTable();
    pushState();
}

void PositionEditor::toggleSelected()
{
    const std::set<int> s = m_canvas->selection();
    if (s.empty())
        return;
    // All on when any is off, else all off.
    bool anyOff = false;
    for (int i : s)
        anyOff = anyOff || !m_elements[std::size_t(i)].enabled;
    for (int i : s)
        m_elements[std::size_t(i)].enabled = anyOff;
    m_canvas->refresh();
    refreshTable();
    pushState();
}

bool PositionEditor::importCsv(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    std::vector<CustomElement> list;
    const QRegularExpression split(QStringLiteral("[,;\\s]+"));
    for (const QString &raw : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList v = line.split(split, Qt::SkipEmptyParts);
        bool ok = false;
        v.value(0).toDouble(&ok);
        if (!ok || v.size() < 2)
            continue; // header
        CustomElement c;
        c.x = v.value(0).toDouble() / m_scale;
        c.y = v.value(1).toDouble() / m_scale;
        c.z = v.value(2).toDouble() / m_scale;
        if (v.size() >= 6) {
            c.nx = v.value(3).toDouble();
            c.ny = v.value(4).toDouble();
            c.nz = v.value(5).toDouble();
            if (std::hypot(c.nx, c.ny, c.nz) < 1e-12)
                c.nz = 1;
        }
        c.rotationDeg = v.value(6).toDouble();
        c.amplitudeDb = v.value(7).toDouble();
        c.phaseDeg = v.value(8).toDouble();
        if (v.size() >= 10)
            c.enabled = v.value(9).toDouble() != 0;
        list.push_back(c);
    }
    if (list.empty()) {
        if (error)
            *error = tr("No element rows found. Expected columns: x, y, z[, nx, ny, nz, rotation, amplitude dB, phase deg, on] "
                        "with positions in %1.")
                         .arg(unitLabel());
        return false;
    }
    m_elements = list;
    m_canvas->setSelection({});
    m_canvas->refresh();
    m_canvas->fitView();
    refreshTable();
    pushState();
    return true;
}

bool PositionEditor::exportCsv(const QString &path, QString *error) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QTextStream out(&file);
    out << "# element positions in " << unitLabel() << "\n";
    out << "x,y,z,nx,ny,nz,rotation_deg,amplitude_db,phase_deg,on\n";
    for (const CustomElement &e : m_elements)
        out << number(e.x * m_scale) << ',' << number(e.y * m_scale) << ',' << number(e.z * m_scale) << ',' << number(e.nx) << ','
            << number(e.ny) << ',' << number(e.nz) << ',' << number(e.rotationDeg) << ',' << number(e.amplitudeDb) << ','
            << number(e.phaseDeg) << ',' << (e.enabled ? 1 : 0) << '\n';
    out.flush();
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace lvd::antui

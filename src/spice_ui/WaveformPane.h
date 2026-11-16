#pragma once

#include "plot/PlotWidget.h"
#include "spice/Fourier.h"
#include "spice/SpiceData.h"
#include "spice/VectorExpression.h"

#include <QFrame>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QComboBox;
class QLabel;

namespace lvd::spiceui {

// One pane of the waveform viewer as stored in the document.
struct WaveformPaneDefinition {
    QString id;         // "p1", "p2", ...
    QString name;       // shown in the pane header; empty: "Pane n"
    QStringList traces; // trace expressions: "v(out)", "db(v(out)/v(in))", "i(vv1)*1000"
    bool logX = false;  // AC and noise panes default to true
    // Non-empty: the pane shows the amplitude spectrum of this expression,
    // evaluated on a transient plot; traces are then not used.
    QString fftSource;
    QString fftWindow = QStringLiteral("hann"); // fftWindowKey() of a spice::WindowType
    bool fftLinear = false;                     // spectrum in V instead of dBV
    // What complex traces show, by expression: "db" (default), "mag",
    // "phase", "real", "imag".
    QMap<QString, QString> complexParts;

    bool isFft() const { return !fftSource.isEmpty(); }
    QJsonObject toJson() const;
    static WaveformPaneDefinition fromJson(const QJsonObject &object);
    bool operator==(const WaveformPaneDefinition &other) const = default;
};

// "hann", "blackmanharris", "flattop", ... (lower case, no spaces or dashes).
QString fftWindowKey(spice::WindowType type);
spice::WindowType fftWindowFromKey(const QString &key); // Hann when unknown

QString complexPartKey(ComplexPart part); // "db", "mag", "phase", "real", "imag"
ComplexPart complexPartFromKey(const QString &key); // MagnitudeDb when unknown

// Resolves a name the plot does not have (schematic net labels, part
// references); see spice/SpiceWaveforms.h plotResolver().
using WaveformNameResolver = std::function<std::optional<spice::Waveform>(const spice::SpicePlot &, const QString &)>;

// Waveform of a vector over its plot's scale (sample index when the plot has none).
spice::Waveform plotVectorWaveform(const spice::SpicePlot &plot, const spice::SpiceVector &vector);

// Cursor A and B of a pane, on its selected trace.
struct WaveformCursorReadout {
    bool hasA = false;
    bool hasB = false;
    double xA = 0, yA = 0;
    double xB = 0, yB = 0;
    bool validA = false, validB = false; // y known (x inside the trace)
    QString xUnit;
    QString yUnit;
    // When both are set: B - A, and 1 / |dx|.
    double dx = 0;
    double dy = 0;
    double inverseDx = 0;
    bool hasDelta() const { return hasA && hasB && validA && validB; }
};

class CursorOverlay;

// One plot of the waveform viewer: evaluates its trace expressions (or the
// spectrum of its FFT source) on the shown SPICE plot and draws them in a
// PlotWidget, with cursors A and B on the selected trace.
class WaveformPane : public QFrame
{
    Q_OBJECT
public:
    explicit WaveformPane(const WaveformPaneDefinition &definition, QWidget *parent = nullptr);
    ~WaveformPane() override;

    const WaveformPaneDefinition &definition() const { return m_definition; }
    QString id() const { return m_definition.id; }
    QString displayName() const;
    // Replaces the definition (as the owner or the viewer does; no signal).
    void setDefinition(const WaveformPaneDefinition &definition);

    // Shows the traces on this plot (null: no data). keepView keeps the zoom
    // (when still inside the data) and the cursors, for a new run of the same
    // analysis. The plot must outlive the pane or the next setPlot call.
    void setPlot(const spice::SpicePlot *plot, bool keepView);
    void setNameResolver(WaveformNameResolver resolver);

    bool isFft() const { return m_definition.isFft(); }
    // Transient waveforms (not a spectrum): THD and Fourier apply.
    bool isTimeDomain() const;
    QString xUnit() const { return m_xUnit; }

    // ---- Traces (in definition order; an FFT pane has one: the spectrum)
    int traceCount() const { return int(m_entries.size()); }
    QString traceExpression(int index) const;
    QString traceError(int index) const; // empty when it evaluated
    const spice::Waveform *traceWaveform(int index) const; // evaluated result, null on error
    int plotTraceId(int index) const;                      // -1 on error
    PlotYAxis traceAxis(int index) const;
    ComplexPart traceComplexPart(int index) const;
    // The trace as shown: real values (complex part applied) with its unit.
    std::optional<spice::Waveform> shownWaveform(int index) const;
    // Errors and notes shown under the plot, one per line.
    QString noteText() const;

    // User edits (emit definitionEdited).
    void setTraceComplexPart(int index, ComplexPart part);
    void removeTraceAt(int index);
    void setLogX(bool log);
    void setFftWindow(spice::WindowType window);
    void setFftLinear(bool linear);
    void rename(const QString &name);

    int selectedTrace() const { return m_selected; }
    void setSelectedTrace(int index);

    void setCurrent(bool current);
    bool isCurrent() const { return m_current; }

    // ---- Cursors (0: A, 1: B) on the selected trace
    void setCursorX(int which, std::optional<double> x);
    std::optional<double> cursorX(int which) const;
    bool hasCursor(int which) const { return cursorX(which).has_value(); }
    // Places the cursor at a third (A) or two thirds (B) of the visible range.
    void showCursor(int which, bool on);
    WaveformCursorReadout cursorReadout() const;
    QString cursorText() const;

    // ---- View
    PlotWidget *plot() const { return m_plot; }
    std::pair<double, double> xRange() const;
    void setXRange(double min, double max);
    void autoscale();
    // Scale of the data shown (all traces), for checks against a zoom.
    std::optional<std::pair<double, double>> dataXRange() const;

Q_SIGNALS:
    void activated(); // clicked
    void definitionEdited();
    void xRangeChanged(double min, double max);
    void selectionChanged(); // selected trace, its data or the view changed
    void cursorsChanged();
    void fftRequested(const QString &expression);
    void addPaneRequested();
    void removeRequested();
    void statusMessage(const QString &message);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    struct Entry {
        QString expression;
        int plotId = -1;
        QString plotUnit; // unit the plot trace was created with
        std::optional<spice::Waveform> waveform;
        QString error;
        ComplexPart part = ComplexPart::MagnitudeDb;
        PlotYAxis axis = PlotYAxis::Left;
    };

    void rebuild(bool keepView);
    void evaluate(Entry &entry) const;
    void evaluateSpectrum(Entry &entry) const;
    void assignAxes();
    void updateAxes();
    void updateHeader();
    void updateNotes();
    void restoreCursors(const std::array<std::optional<double>, 2> &xs);
    int markerTrace() const;
    void showContextMenu(const QPoint &globalPos, const QPointF &plotPos);
    void addTraceMenu(class QMenu *menu, int index);
    void emitEdited();
    QString shownName(const Entry &entry) const;
    QColor errorColor() const;

    WaveformPaneDefinition m_definition;
    const spice::SpicePlot *m_data = nullptr; // the shown SPICE plot
    WaveformNameResolver m_nameResolver;
    std::vector<Entry> m_entries;
    QStringList m_notes;
    QString m_xUnit;
    int m_selected = 0;
    bool m_current = false;
    std::array<int, 2> m_cursorMarkers{-1, -1};
    std::pair<double, double> m_lastXRange{0, 0};
    bool m_updatingTraces = false;

    PlotWidget *m_plot = nullptr;
    CursorOverlay *m_overlay = nullptr;
    QLabel *m_title = nullptr;
    QComboBox *m_traceCombo = nullptr;
    QLabel *m_cursorLabel = nullptr;
    QLabel *m_noteLabel = nullptr;
};

} // namespace lvd::spiceui

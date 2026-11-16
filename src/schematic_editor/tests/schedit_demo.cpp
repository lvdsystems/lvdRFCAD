// Renders demo schematics with the editor to PNG files and quits; used for
// visual checks on the native platform (the offscreen platform has no fonts
// on Windows). Usage: schedit_demo <output folder>

#include "core/Connectivity.h"
#include "core/Document.h"
#include "core/Schematic.h"
#include "core/Session.h"
#include "schematic_editor/EditorSupport.h"
#include "schematic_editor/SchematicEditor.h"
#include "schematic_editor/SchematicView.h"

#include <QApplication>
#include <QDir>
#include <QPalette>
#include <QSplitter>
#include <QStyle>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

using namespace lvd;

namespace {

QPalette darkPalette()
{
    QPalette p;
    p.setColor(QPalette::Window, QColor(0x2B, 0x2D, 0x30));
    p.setColor(QPalette::WindowText, QColor(0xDD, 0xDD, 0xDD));
    p.setColor(QPalette::Base, QColor(0x1E, 0x1F, 0x22));
    p.setColor(QPalette::AlternateBase, QColor(0x2B, 0x2D, 0x30));
    p.setColor(QPalette::Text, QColor(0xDD, 0xDD, 0xDD));
    p.setColor(QPalette::Button, QColor(0x3C, 0x3F, 0x41));
    p.setColor(QPalette::ButtonText, QColor(0xDD, 0xDD, 0xDD));
    p.setColor(QPalette::Highlight, QColor(0x2F, 0x65, 0xCA));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(0x88, 0x88, 0x88));
    return p;
}

struct Builder {
    Project *project;
    Schematic *schematic;

    QString place(const QString &key, QPoint at, int rotation = 0, bool mirror = false)
    {
        const DefinitionPtr definition = project->resolve(*schematic, key).definition;
        return schematic->addComponent(key, definition.get(), at, rotation, mirror);
    }
    QPoint pin(const QString &id, const QString &name)
    {
        const PlacedComponent *component = schematic->component(id);
        const DefinitionPtr definition = project->resolve(*schematic, component->definition).definition;
        if (!definition || !definition->symbol.pin(name))
            return component->at;
        return pinPosition(*component, *definition->symbol.pin(name));
    }
    QString wire(QPoint a, QPoint b, bool horizontalFirst = true)
    {
        QList<QPoint> points{a};
        points += orthogonalRoute(a, b, horizontalFirst);
        return schematic->addWire(points);
    }
    QString connect(const QString &a, const QString &pa, const QString &b, const QString &pb, bool hFirst = true)
    {
        return wire(pin(a, pa), pin(b, pb), hFirst);
    }
};

// A receiver chain: source, filter, LNA, mixer with LO, IF filter, ADC.
struct Receiver {
    QString amplifier, ifWire;
};

Receiver buildReceiver(Builder &b)
{
    Schematic &s = *b.schematic;
    s.setVariables({{QStringLiteral("f_rf"), QStringLiteral("2.45GHz")},
                    {QStringLiteral("f_if"), QStringLiteral("70MHz")},
                    {QStringLiteral("f_lo"), QStringLiteral("f_rf - f_if")},
                    {QStringLiteral("g_lna"), QStringLiteral("18")}});
    const QString src = b.place(QStringLiteral("rf-system:source"), QPoint(0, 100));
    const QString ant = b.place(QStringLiteral("rf-system:filter"), QPoint(110, 100));
    const QString lna = b.place(QStringLiteral("rf-system:amplifier"), QPoint(230, 100));
    const QString mix = b.place(QStringLiteral("rf-system:mixer"), QPoint(350, 100));
    const QString lo = b.place(QStringLiteral("rf-system:lo"), QPoint(350, 230), 90);
    const QString iff = b.place(QStringLiteral("rf-system:filter"), QPoint(480, 100));
    const QString adc = b.place(QStringLiteral("rf-system:adc"), QPoint(620, 100));
    const QString res = b.place(QStringLiteral("basic:resistor"), QPoint(170, 170), 90);
    const QString gnd = b.place(QStringLiteral("basic:ground"), QPoint(170, 220));

    s.setParameter(src, QStringLiteral("freq"), QStringLiteral("f_rf"));
    s.setParameter(ant, QStringLiteral("fc"), QStringLiteral("f_rf"));
    s.setParameter(lna, QStringLiteral("gain"), QStringLiteral("g_lna"));
    s.setParameter(lo, QStringLiteral("freq"), QStringLiteral("f_lo"));
    s.setParameter(iff, QStringLiteral("fc"), QStringLiteral("f_if"));
    s.setParameter(iff, QStringLiteral("type"), QStringLiteral("lowpass"));
    s.setParameter(res, QStringLiteral("r"), QStringLiteral("1k"));

    b.connect(src, QStringLiteral("out"), ant, QStringLiteral("in"));
    b.connect(ant, QStringLiteral("out"), lna, QStringLiteral("in"));
    b.connect(lna, QStringLiteral("out"), mix, QStringLiteral("rf"));
    b.connect(lo, QStringLiteral("out"), mix, QStringLiteral("lo"));
    const QString ifWire = b.connect(mix, QStringLiteral("if"), iff, QStringLiteral("in"));
    b.connect(iff, QStringLiteral("out"), adc, QStringLiteral("in"));
    // Bias resistor from the filter-LNA wire to ground.
    QPoint top = b.pin(res, QStringLiteral("p"));
    QPoint bottom = b.pin(res, QStringLiteral("n"));
    if (top.y() > bottom.y())
        std::swap(top, bottom);
    b.wire(QPoint(top.x(), 100), top);
    b.wire(bottom, b.pin(gnd, QStringLiteral("gnd")));

    s.addLabel(QStringLiteral("RF_IN"), QPoint(50, 100));
    s.addLabel(QStringLiteral("LO"), QPoint(350, 160));
    s.addLabel(QStringLiteral("IF"), QPoint(530, 100));
    s.addText(QStringLiteral("2.45 GHz receiver front end"), QPoint(-20, 20), 14);
    s.addText(QStringLiteral("LNA bias network"), QPoint(190, 230), 9);
    return Receiver{lna, ifWire};
}

void buildOrientations(Builder &b)
{
    const QStringList keys = {QStringLiteral("rf-system:amplifier"), QStringLiteral("rf-system:mixer"),
                              QStringLiteral("rf-system:source"), QStringLiteral("basic:capacitor"),
                              QStringLiteral("rf-system:antenna")};
    int y = 0;
    for (const QString &key : keys) {
        int x = 0;
        for (const bool mirror : {false, true}) {
            for (const int rotation : {0, 90, 180, 270}) {
                b.place(key, QPoint(x, y), rotation, mirror);
                x += 130;
            }
        }
        y += 130;
    }
    // A part only in the cache, and one whose definition is missing.
    const DefinitionPtr amplifier = b.project->resolve(*b.schematic, QStringLiteral("rf-system:amplifier")).definition;
    b.schematic->setCachedDefinition(QStringLiteral("oldlib:amplifier"), amplifier);
    b.place(QStringLiteral("oldlib:amplifier"), QPoint(0, y));
    b.place(QStringLiteral("nolib:widget"), QPoint(130, y));
    b.schematic->addText(QStringLiteral("cache part (C badge), missing part"), QPoint(220, y), 10);
}

bool grab(QWidget *widget, const QString &path)
{
    widget->show();
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
    const bool ok = widget->grab().save(path);
    QTextStream(stdout) << (ok ? "wrote " : "FAILED ") << QDir::toNativeSeparators(path) << Qt::endl;
    return ok;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath();
    QDir().mkpath(output);

    QTemporaryDir dir;
    Session session;
    QString error;
    Project *project = session.newProject(dir.filePath(QStringLiteral("demo")), QStringLiteral("demo"), &error);
    if (!project) {
        QTextStream(stderr) << error << Qt::endl;
        return 1;
    }
    Document *receiver = project->createDocument(DocumentType::RfSchematic, QStringLiteral("receiver"), &error);
    Document *orientations = project->createDocument(DocumentType::RfSchematic, QStringLiteral("orient"), &error);
    Builder rb{project, receiver->schematic()};
    const Receiver parts = buildReceiver(rb);
    fixJunctions(*receiver->schematic(), project->lookupFor(*receiver->schematic()));
    Builder ob{project, orientations->schematic()};
    buildOrientations(ob);

    const QPalette light = QApplication::style()->standardPalette();
    int failures = 0;
    for (const bool dark : {false, true}) {
        QApplication::setPalette(dark ? darkPalette() : light);
        const QString suffix = dark ? QStringLiteral("-dark.png") : QStringLiteral("-light.png");

        QSplitter window;
        window.resize(1500, 620);
        auto *editor = new SchematicEditor(project, receiver, &window);
        window.addWidget(editor);
        QSplitter *side = new QSplitter(Qt::Vertical, &window);
        side->addWidget(editor->propertiesWidget());
        side->addWidget(editor->variablesWidget());
        window.addWidget(side);
        window.setSizes({1040, 460});
        window.show();
        QApplication::processEvents();
        editor->zoomToFit();
        editor->selectItems({parts.amplifier});
        editor->highlightNet(parts.ifWire);
        failures += grab(&window, QDir(output).filePath(QStringLiteral("receiver") + suffix)) ? 0 : 1;

        // Wire tool preview and a rule check.
        editor->runRuleCheck();
        editor->selectItems({});
        editor->view()->setTool(SchematicView::Tool::Wire);
        failures += grab(&window, QDir(output).filePath(QStringLiteral("receiver-check") + suffix)) ? 0 : 1;
        window.hide();
        delete editor;

        SchematicEditor other(project, orientations);
        other.resize(1200, 820);
        other.show();
        QApplication::processEvents();
        other.zoomToFit();
        failures += grab(&other, QDir(output).filePath(QStringLiteral("orientations") + suffix)) ? 0 : 1;
    }
    return failures;
}

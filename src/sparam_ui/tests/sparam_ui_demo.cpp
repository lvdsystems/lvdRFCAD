// Opens the S-parameter editor on the native platform with the two-port
// example that has noise data, saves screenshots and quits. Not a test: used
// to check the look in the light and dark themes.
//
// Usage: sparam_ui_demo <output folder> [light|dark]

#include "plot/PlotWidget.h"
#include "sparam_ui/SParamDialogs.h"
#include "sparam_ui/TouchstoneEditor.h"

#include <QApplication>
#include <QDir>
#include <QStyleHints>
#include <QThread>

#include <cstdio>

using namespace lvd;

namespace {

void settle()
{
    for (int i = 0; i < 10; ++i) {
        QApplication::processEvents();
        QThread::msleep(20);
    }
}

bool save(QWidget &widget, const QString &path)
{
    settle();
    const bool ok = widget.grab().save(path);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", qPrintable(QDir::toNativeSeparators(path)));
    return ok;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("sparam_ui_demo"));
    if (argc < 2) {
        std::fprintf(stderr, "usage: sparam_ui_demo <output folder> [light|dark]\n");
        return 2;
    }
    const QString output = QString::fromLocal8Bit(argv[1]);
    const QString theme = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("light");
    QGuiApplication::styleHints()->setColorScheme(theme == QLatin1String("dark") ? Qt::ColorScheme::Dark
                                                                                  : Qt::ColorScheme::Light);
    QDir().mkpath(output);
    const auto file = [&](const QString &name) {
        return QDir(output).filePath(QStringLiteral("%1-%2.png").arg(name, theme));
    };
    const QString data = QDir(QStringLiteral(LVD_RFDATA_TEST_DATA)).filePath(QStringLiteral("amp_noise.s2p"));

    bool ok = true;
    {
        TouchstoneEditor editor;
        QString error;
        if (!editor.openFile(data, &error)) {
            std::fprintf(stderr, "cannot open %s: %s\n", qPrintable(data), qPrintable(error));
            return 1;
        }
        editor.setWindowTitle(editor.title());
        editor.resize(1440, 900);
        editor.show();
        settle();
        editor.setSelectedParameters({{0, 0}, {1, 0}});
        editor.setPlotQuantity(SParamQuantity::MagnitudeDb);
        editor.rectangularPlot()->addMarker(editor.rectangularPlot()->traceIds().back(), 3e9);
        ok = save(editor, file(QStringLiteral("editor"))) && ok;

        editor.setSelectedParameters({{0, 0}, {0, 1}, {1, 0}, {1, 1}});
        editor.setTableRepresentation(SParamRepresentation::Z);
        editor.setTableForm(SParamTableForm::RealImaginary);
        editor.checkPassivityAndReciprocity();
        ok = save(editor, file(QStringLiteral("editor-checks"))) && ok;
        editor.clearHighlights();

        editor.setPlotQuantity(SParamQuantity::StabilityK);
        ok = save(editor, file(QStringLiteral("editor-stability"))) && ok;
        editor.setPlotQuantity(SParamQuantity::NoiseFigureMin);
        ok = save(editor, file(QStringLiteral("editor-noise"))) && ok;
    }
    {
        ResampleDialog dialog(1e9, 6e9, 11);
        dialog.show();
        ok = save(dialog, file(QStringLiteral("resample-dialog"))) && ok;
    }
    {
        RenumberPortsDialog dialog(4);
        dialog.show();
        ok = save(dialog, file(QStringLiteral("renumber-dialog"))) && ok;
    }
    return ok ? 0 : 1;
}

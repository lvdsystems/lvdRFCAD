// Opens the library browser, the symbol editor and the library manager on the
// native platform, saves screenshots and quits. Not a test: used to check the
// look in the light and dark themes.
//
// Usage: library_ui_demo <output folder> [light|dark]

#include "core/Document.h"
#include "core/Library.h"
#include "core/Project.h"
#include "library_ui/LibraryBrowser.h"
#include "library_ui/LibraryManagerDialog.h"
#include "library_ui/SymbolCanvas.h"
#include "library_ui/SymbolEditor.h"

#include <QApplication>
#include <QDir>
#include <QStyleHints>
#include <QTabWidget>
#include <QTemporaryDir>
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
    QApplication::setApplicationName(QStringLiteral("library_ui_demo"));
    if (argc < 2) {
        std::fprintf(stderr, "usage: library_ui_demo <output folder> [light|dark]\n");
        return 2;
    }
    const QString output = QString::fromLocal8Bit(argv[1]);
    const QString theme = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("light");
    QGuiApplication::styleHints()->setColorScheme(theme == QLatin1String("dark") ? Qt::ColorScheme::Dark
                                                                                  : Qt::ColorScheme::Light);
    QDir().mkpath(output);
    const auto file = [&](const QString &name) { return QDir(output).filePath(QStringLiteral("%1-%2.png").arg(name, theme)); };

    QTemporaryDir userDir;
    QTemporaryDir projectDir;
    LibraryManager libraries;
    libraries.setSearchPath(LibraryScope::BuiltIn, LibraryManager::defaultBuiltInPath());
    libraries.setSearchPath(LibraryScope::User, userDir.path());

    QString error;
    auto project = Project::create(projectDir.filePath(QStringLiteral("demo")), QStringLiteral("demo"), &error);
    if (project) {
        project->setLibraryManager(&libraries);
        project->createDocument(DocumentType::RfSchematic, QStringLiteral("receiver"), &error);
        project->createDocument(DocumentType::RfSchematic, QStringLiteral("lna_stage"), &error);
    }

    // A user library, so the tree shows all scopes.
    SymbolEditor creator(&libraries);
    creator.newComponent(QStringLiteral("my-parts"), LibraryScope::User);
    ComponentDefinition custom = creator.definition();
    custom.id = QStringLiteral("bias_tee");
    custom.name = QStringLiteral("Bias tee");
    custom.category = QStringLiteral("RF/Passive");
    creator.setDefinition(custom);
    creator.save(&error);

    bool ok = true;
    {
        LibraryBrowser browser(&libraries);
        browser.setProject(project.get());
        browser.resize(380, 820);
        browser.show();
        browser.selectComponent(QStringLiteral("rf-system:mixer"));
        ok = save(browser, file(QStringLiteral("browser"))) && ok;
        browser.setFilterText(QStringLiteral("line"));
        browser.selectComponent(QStringLiteral("rf-circuit:microstrip"));
        ok = save(browser, file(QStringLiteral("browser-search"))) && ok;
    }
    {
        SymbolEditor editor(&libraries);
        editor.openComponent(QStringLiteral("rf-system:mixer"));
        editor.resize(1440, 900);
        editor.show();
        settle();
        editor.canvas()->zoomToFit();
        editor.canvas()->setSelection({ElementRef{ElementRef::Kind::Pin, 1}});
        ok = save(editor, file(QStringLiteral("editor"))) && ok;
        if (auto *tabs = editor.findChild<QTabWidget *>(QStringLiteral("componentTabs"))) {
            tabs->setCurrentIndex(2);
            ok = save(editor, file(QStringLiteral("editor-parameters"))) && ok;
            tabs->setCurrentIndex(3);
            ok = save(editor, file(QStringLiteral("editor-models"))) && ok;
        }
    }
    {
        SymbolEditor editor(&libraries);
        editor.newComponent(QStringLiteral("my-parts"), LibraryScope::User);
        ComponentDefinition definition = editor.definition();
        definition.id = QStringLiteral("Bad Id");
        definition.symbol.pins[1].at = QPoint(35, 0);
        SymbolGraphic arc;
        arc.type = SymbolGraphic::Type::Arc;
        arc.center = QPoint(0, 0);
        arc.radius = 12;
        arc.startAngle = 30;
        arc.spanAngle = 120;
        definition.symbol.graphics.append(arc);
        SymbolGraphic text;
        text.type = SymbolGraphic::Type::Text;
        text.at = QPoint(0, 0);
        text.text = QStringLiteral("LNA");
        text.anchor = TextAnchor::Center;
        text.size = 8;
        definition.symbol.graphics.append(text);
        editor.setDefinition(definition);
        editor.resize(1440, 900);
        editor.show();
        settle();
        editor.canvas()->setSelection({ElementRef{ElementRef::Kind::Graphic, 1}});
        ok = save(editor, file(QStringLiteral("editor-new"))) && ok;
    }
    {
        LibraryManagerDialog dialog(&libraries);
        dialog.show();
        ok = save(dialog, file(QStringLiteral("manager"))) && ok;
    }
    return ok ? 0 : 1;
}

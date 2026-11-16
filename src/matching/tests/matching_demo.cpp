// Opens the matching window on the native platform for Zs = 50 ohm,
// ZL = 12 - j30 ohm at 2.4 GHz, selects solutions, saves screenshots and
// quits. Not a test: used to check the look in the light and dark themes.
//
// Usage: matching_demo <output folder> [light|dark]

#include "core/Session.h"
#include "matching/MatchingWindow.h"

#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

using namespace lvd;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad-tests"));
    QApplication::setApplicationName(QStringLiteral("matching_demo"));
    if (argc < 2) {
        std::fprintf(stderr, "usage: matching_demo <output folder> [light|dark]\n");
        return 2;
    }
    const QString output = QString::fromLocal8Bit(argv[1]);
    const QString theme = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("light");
    QGuiApplication::styleHints()->setColorScheme(theme == QLatin1String("dark") ? Qt::ColorScheme::Dark
                                                                                  : Qt::ColorScheme::Light);
    QDir().mkpath(output);
    const auto file = [&](const QString &name) { return QDir(output).filePath(QStringLiteral("%1-%2.png").arg(name, theme)); };

    QTemporaryDir dir;
    Session session;
    Project *project = session.newProject(dir.filePath(QStringLiteral("demo")), QStringLiteral("demo"));
    Document *document = project ? project->createDocument(DocumentType::RfSchematic, QStringLiteral("dut")) : nullptr;

    MatchingWindow window;
    window.resize(1360, 900);
    window.setTarget(project, document);
    window.setImpedances({50, 0}, {12, -30}, 2.4e9);
    window.show();

    bool ok = true;
    const auto settle = [] {
        for (int i = 0; i < 10; ++i)
            QApplication::processEvents();
    };
    const auto shoot = [&](const QString &name) {
        settle();
        if (!window.grab().save(file(name))) {
            std::fprintf(stderr, "cannot save %s\n", qPrintable(file(name)));
            ok = false;
        }
    };

    QTimer::singleShot(300, &app, [&] {
        window.setTopology(rf::MatchTopology::LNetwork);
        window.selectSolution(0);
        shoot(QStringLiteral("matching-l"));

        window.setStandardSeries(ESeries::E12);
        window.selectSolution(1);
        shoot(QStringLiteral("matching-l-e12"));
        window.setStandardSeries(std::nullopt);

        window.setTopology(rf::MatchTopology::PiNetwork);
        window.setLoadedQ(5);
        window.selectSolution(0);
        shoot(QStringLiteral("matching-pi"));

        window.setTopology(rf::MatchTopology::SingleStub);
        window.selectSolution(0);
        shoot(QStringLiteral("matching-stub"));

        window.setTopology(rf::MatchTopology::QuarterWave);
        window.selectSolution(0);
        shoot(QStringLiteral("matching-qw"));

        window.setImpedances({50, 0}, {500, 0}, 2.4e9);
        window.setTopology(rf::MatchTopology::MultiSection);
        window.setSections(3);
        window.selectSolution(0);
        shoot(QStringLiteral("matching-ladder"));
        app.exit(ok ? 0 : 1);
    });
    return app.exec();
}

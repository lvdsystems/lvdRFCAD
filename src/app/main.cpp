#include "core/Session.h"
#include "gui/MainWindow.h"
#include "python/PythonInterpreter.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>

#include <exception>
#include <memory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("lvdRFcad"));
    QApplication::setApplicationName(QStringLiteral("lvdRFcad"));
    QApplication::setApplicationDisplayName(QStringLiteral("lvd RF cad"));
    QApplication::setApplicationVersion(QStringLiteral(LVD_VERSION));
    QApplication::setWindowIcon(lvd::MainWindow::applicationIcon());

    lvd::Session session;
    // User libraries live in the per-user data folder unless the user chose another one.
    const QString defaultUserLibraries =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("libraries"));
    session.libraries()->setSearchPath(
        lvd::LibraryScope::User, QSettings().value(QStringLiteral("userLibraryPath"), defaultUserLibraries).toString());

    std::unique_ptr<lvd::PythonInterpreter> python;
    try {
        python = std::make_unique<lvd::PythonInterpreter>(&session);
    } catch (const std::exception &error) {
        QMessageBox::critical(nullptr, QStringLiteral("lvd RF cad"),
                              QStringLiteral("Python could not be started:\n%1").arg(QString::fromLocal8Bit(error.what())));
        return 1;
    }

    lvd::MainWindow window(&session, python.get());
    window.show();

    // A project file on the command line wins over the last session's project.
    const QStringList arguments = QApplication::arguments();
    const QString project =
        arguments.size() > 1 ? arguments.at(1) : QSettings().value(QStringLiteral("lastProject")).toString();
    if (!project.isEmpty() && QFileInfo::exists(project))
        window.openProjectFile(project);
    // Unsaved work of another project left by a crash.
    window.offerRecoveryOfOtherProjects();

    return app.exec();
}

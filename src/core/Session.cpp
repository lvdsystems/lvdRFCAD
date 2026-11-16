#include "core/Session.h"

#include <QDir>

namespace lvd {

Session::Session(QObject *parent) : QObject(parent), m_libraries(new LibraryManager(this))
{
    m_libraries->setSearchPath(LibraryScope::BuiltIn, LibraryManager::defaultBuiltInPath());
}

Session::~Session() = default;

Project *Session::newProject(const QString &directory, const QString &name, QString *error)
{
    auto project = Project::create(directory, name, error);
    if (!project)
        return nullptr;
    return replaceProject(std::move(project));
}

Project *Session::openProject(const QString &projectFile, QString *error)
{
    auto project = Project::load(projectFile, error);
    if (!project)
        return nullptr;
    return replaceProject(std::move(project));
}

void Session::closeProject()
{
    if (!m_project)
        return;
    Q_EMIT projectAboutToClose(m_project.get());
    m_project.reset();
    m_libraries->setSearchPath(LibraryScope::Project, QString());
    Q_EMIT projectChanged(nullptr);
}

Project *Session::replaceProject(std::unique_ptr<Project> project)
{
    if (m_project) {
        Q_EMIT projectAboutToClose(m_project.get());
        m_project.reset();
    }
    m_project = std::move(project);
    m_project->setLibraryManager(m_libraries);
    m_libraries->setSearchPath(LibraryScope::Project, QDir(m_project->directory()).filePath(QStringLiteral("libraries")));
    Q_EMIT projectChanged(m_project.get());
    return m_project.get();
}

} // namespace lvd

#pragma once

#include "core/Library.h"
#include "core/Project.h"

#include <QObject>

#include <memory>

namespace lvd {

// Application state shared by the GUI, the Python API and the command-line
// runner: the component libraries and at most one open project.
class Session : public QObject
{
    Q_OBJECT
public:
    explicit Session(QObject *parent = nullptr);
    ~Session() override;

    Project *project() const { return m_project.get(); }
    LibraryManager *libraries() const { return m_libraries; }

    // On failure the currently open project stays open.
    Project *newProject(const QString &directory, const QString &name, QString *error = nullptr);
    Project *openProject(const QString &projectFile, QString *error = nullptr);
    void closeProject();

Q_SIGNALS:
    void projectAboutToClose(lvd::Project *project);
    void projectChanged(lvd::Project *project); // nullptr when no project is open

private:
    Project *replaceProject(std::unique_ptr<Project> project);

    LibraryManager *m_libraries;
    std::unique_ptr<Project> m_project;
};

} // namespace lvd

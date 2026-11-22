#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <optional>

namespace lvd {

class Project;
class Session;

// Crash safety (requirement NF-7, decision of 2026-11-16): while documents
// have unsaved changes, copies of them are written every few minutes to a
// per-user folder, outside the project:
//
//   <base>/<hash of the project file path>/
//       session.json            project path, time, application version, documents
//       <document file name>    copy of each modified document
//
// A clean close of the project removes the folder. A folder that is still
// there when the project opens again holds work lost in a crash; the
// application offers to restore it.
class Autosave : public QObject
{
    Q_OBJECT
public:
    explicit Autosave(Session *session, QObject *parent = nullptr);
    ~Autosave() override;

    // Seconds between saves; 0 disables autosave. Default 120.
    int intervalSeconds() const { return m_interval; }
    void setIntervalSeconds(int seconds);

    // Writes the copies now (normally done by the timer). Returns false and
    // sets the error when a copy cannot be written.
    bool saveNow(QString *error = nullptr);

    // The folder of all autosave copies (QStandardPaths application data
    // folder, "autosave"); tests may redirect it.
    static QString baseDirectory();
    static void setBaseDirectory(const QString &directory);
    // The folder of one project.
    static QString directoryFor(const QString &projectFile);

    struct RecoveredDocument {
        QString name;      // document name
        QString copyPath;  // the autosaved copy
        QDateTime savedAt; // when the copy was written
    };
    struct Recovery {
        QString projectFile;
        QString projectName;
        QDateTime savedAt;
        QString applicationVersion;
        QList<RecoveredDocument> documents;
    };
    // Unsaved work left from an earlier session, if any.
    static std::optional<Recovery> pendingRecovery(const QString &projectFile);
    // All projects with unsaved work left (newest first); projects whose file
    // no longer exists are skipped.
    static QList<Recovery> allPendingRecoveries();
    // Puts the copies into the open project's documents (marked modified);
    // call before any editor shows them. Removes the copies afterwards.
    // Returns the names of the restored documents.
    static QStringList restore(Project &project, const Recovery &recovery, QStringList *errors = nullptr);
    // Removes the copies of a project.
    static void discard(const QString &projectFile);

Q_SIGNALS:
    void saved(int documents);
    void failed(const QString &error);

private:
    void onProjectChanged(Project *project);
    void onProjectAboutToClose(Project *project);
    void onTimer();

    Session *m_session;
    QPointer<Project> m_project;
    QTimer m_timer;
    int m_interval = 120;
};

} // namespace lvd

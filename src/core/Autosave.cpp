#include "core/Autosave.h"

#include "core/Document.h"
#include "core/JsonFile.h"
#include "core/Project.h"
#include "core/Session.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QStandardPaths>

#include <algorithm>

namespace lvd {

namespace {

QString &baseOverride()
{
    static QString directory;
    return directory;
}

QString sessionFile(const QString &directory)
{
    return QDir(directory).filePath(QStringLiteral("session.json"));
}

QString canonicalProjectPath(const QString &projectFile)
{
    const QFileInfo info(projectFile);
    const QString canonical = info.canonicalFilePath();
    return QDir::fromNativeSeparators(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

std::optional<Autosave::Recovery> readRecovery(const QString &directory)
{
    const auto object = readJsonFile(sessionFile(directory), nullptr);
    if (!object || object->value(QStringLiteral("format")).toString() != QLatin1String("lvdrfcad-autosave"))
        return std::nullopt;
    Autosave::Recovery r;
    r.projectFile = object->value(QStringLiteral("project")).toString();
    r.projectName = object->value(QStringLiteral("projectName")).toString();
    r.savedAt = QDateTime::fromString(object->value(QStringLiteral("savedAt")).toString(), Qt::ISODate);
    r.applicationVersion = object->value(QStringLiteral("applicationVersion")).toString();
    for (const QJsonValue &v : object->value(QStringLiteral("documents")).toArray()) {
        const QJsonObject d = v.toObject();
        Autosave::RecoveredDocument doc;
        doc.name = d.value(QStringLiteral("name")).toString();
        doc.copyPath = QDir(directory).filePath(d.value(QStringLiteral("copy")).toString());
        doc.savedAt = QDateTime::fromString(d.value(QStringLiteral("savedAt")).toString(), Qt::ISODate);
        if (QFileInfo::exists(doc.copyPath))
            r.documents << doc;
    }
    if (r.documents.isEmpty())
        return std::nullopt;
    return r;
}

} // namespace

Autosave::Autosave(Session *session, QObject *parent)
    : QObject(parent), m_session(session)
{
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, &Autosave::onTimer);
    connect(session, &Session::projectChanged, this, &Autosave::onProjectChanged);
    connect(session, &Session::projectAboutToClose, this, &Autosave::onProjectAboutToClose);
    setIntervalSeconds(m_interval);
    onProjectChanged(session->project());
}

Autosave::~Autosave() = default;

void Autosave::setIntervalSeconds(int seconds)
{
    m_interval = std::max(0, seconds);
    if (m_interval > 0)
        m_timer.start(m_interval * 1000);
    else
        m_timer.stop();
}

QString Autosave::baseDirectory()
{
    if (!baseOverride().isEmpty())
        return baseOverride();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("autosave"));
}

void Autosave::setBaseDirectory(const QString &directory)
{
    baseOverride() = directory;
}

QString Autosave::directoryFor(const QString &projectFile)
{
    const QByteArray key = canonicalProjectPath(projectFile).toLower().toUtf8();
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
    return QDir(baseDirectory()).filePath(hash);
}

void Autosave::onProjectChanged(Project *project)
{
    m_project = project;
}

void Autosave::onProjectAboutToClose(Project *project)
{
    // A clean close: the user saved or chose not to; the copies are not needed.
    if (project && !project->isSnapshot())
        discard(project->filePath());
    m_project = nullptr;
}

void Autosave::onTimer()
{
    QString error;
    if (!saveNow(&error))
        Q_EMIT failed(error);
}

bool Autosave::saveNow(QString *error)
{
    Project *project = m_project.data();
    if (!project || project->isSnapshot())
        return true;
    const QString directory = directoryFor(project->filePath());
    QList<Document *> modified;
    for (Document *d : project->documents())
        if (d->isModified())
            modified << d;
    if (modified.isEmpty()) {
        // Everything saved: nothing to recover.
        QDir(directory).removeRecursively();
        return true;
    }
    if (!QDir().mkpath(directory)) {
        setError(error, QStringLiteral("cannot create the autosave folder %1").arg(QDir::toNativeSeparators(directory)));
        return false;
    }
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    QJsonArray documents;
    QStringList keep{QStringLiteral("session.json")};
    for (Document *d : std::as_const(modified)) {
        const QString copy = QFileInfo(d->filePath()).fileName();
        if (!writeJsonFile(QDir(directory).filePath(copy), d->toJson(), error))
            return false;
        keep << copy;
        documents.append(QJsonObject{{QStringLiteral("name"), d->name()},
                                     {QStringLiteral("copy"), copy},
                                     {QStringLiteral("savedAt"), now}});
    }
    // Copies of documents saved since the last autosave go.
    for (const QFileInfo &f : QDir(directory).entryInfoList(QDir::Files))
        if (!keep.contains(f.fileName()))
            QFile::remove(f.absoluteFilePath());
    const QJsonObject session{{QStringLiteral("format"), QStringLiteral("lvdrfcad-autosave")},
                              {QStringLiteral("version"), 1},
                              {QStringLiteral("project"), canonicalProjectPath(project->filePath())},
                              {QStringLiteral("projectName"), project->name()},
                              {QStringLiteral("savedAt"), now},
                              {QStringLiteral("applicationVersion"), QStringLiteral(LVD_VERSION)},
                              {QStringLiteral("documents"), documents}};
    if (!writeJsonFile(sessionFile(directory), session, error))
        return false;
    Q_EMIT saved(int(modified.size()));
    return true;
}

std::optional<Autosave::Recovery> Autosave::pendingRecovery(const QString &projectFile)
{
    return readRecovery(directoryFor(projectFile));
}

QList<Autosave::Recovery> Autosave::allPendingRecoveries()
{
    QList<Recovery> out;
    for (const QFileInfo &dir : QDir(baseDirectory()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        auto r = readRecovery(dir.absoluteFilePath());
        if (r && QFileInfo::exists(r->projectFile))
            out << *r;
    }
    std::sort(out.begin(), out.end(), [](const Recovery &a, const Recovery &b) { return a.savedAt > b.savedAt; });
    return out;
}

QStringList Autosave::restore(Project &project, const Recovery &recovery, QStringList *errors)
{
    QStringList restored;
    for (const RecoveredDocument &r : recovery.documents) {
        Document *document = project.document(r.name);
        if (!document) {
            if (errors)
                *errors << QStringLiteral("%1 is no longer in the project").arg(r.name);
            continue;
        }
        QString error;
        const auto object = readJsonFile(r.copyPath, &error);
        if (!object || !document->restoreFromJson(*object, &error)) {
            if (errors)
                *errors << error;
            continue;
        }
        restored << r.name;
    }
    discard(project.filePath());
    return restored;
}

void Autosave::discard(const QString &projectFile)
{
    QDir(directoryFor(projectFile)).removeRecursively();
}

} // namespace lvd

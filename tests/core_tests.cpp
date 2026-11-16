#include <catch2/catch_test_macros.hpp>

#include "core/JsonFile.h"
#include "core/Session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using namespace lvd;

TEST_CASE("document types have unique identifiers and extensions")
{
    const auto &types = documentTypes();
    REQUIRE(types.size() == 4);
    for (const auto &info : types) {
        REQUIRE(documentTypeFromId(info.id) == info.type);
        for (const auto &other : types) {
            if (other.type != info.type) {
                REQUIRE(other.id != info.id);
                REQUIRE(other.fileExtension != info.fileExtension);
            }
        }
    }
    REQUIRE_FALSE(documentTypeFromId(QStringLiteral("pcb")).has_value());
}

TEST_CASE("a document round-trips through its file")
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("array.lvdant"));

    Document document(DocumentType::AntennaArray, path);
    REQUIRE(document.name() == QStringLiteral("array"));
    REQUIRE(document.schematic() == nullptr);
    QJsonObject content;
    content.insert(QStringLiteral("answer"), 42);
    REQUIRE(document.setContent(content));
    REQUIRE(document.isModified());

    QString error;
    REQUIRE(document.save(&error));
    REQUIRE_FALSE(document.isModified());

    auto loaded = Document::load(path, &error);
    REQUIRE(loaded);
    REQUIRE(loaded->type() == DocumentType::AntennaArray);
    REQUIRE(loaded->name() == QStringLiteral("array"));
    REQUIRE(loaded->content() == content);
    REQUIRE_FALSE(loaded->isModified());
}

TEST_CASE("files from a newer format version are rejected")
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("future.lvdrf"));
    QJsonObject object;
    object.insert(QStringLiteral("format"), Document::formatId());
    object.insert(QStringLiteral("version"), Document::FormatVersion + 1);
    object.insert(QStringLiteral("type"), QStringLiteral("rf"));
    REQUIRE(writeJsonFile(path, object, nullptr));

    QString error;
    REQUIRE_FALSE(Document::load(path, &error));
    REQUIRE(error.contains(QStringLiteral("version")));
}

TEST_CASE("invalid files are reported, not loaded")
{
    QTemporaryDir dir;
    QString error;

    REQUIRE_FALSE(Document::load(dir.filePath(QStringLiteral("missing.lvdrf")), &error));
    REQUIRE_FALSE(error.isEmpty());

    const QString garbage = dir.filePath(QStringLiteral("garbage.lvdrf"));
    QFile file(garbage);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("not json");
    file.close();
    REQUIRE_FALSE(Document::load(garbage, &error));
    REQUIRE_FALSE(Project::load(garbage, &error));
}

TEST_CASE("names that cannot be file names are refused")
{
    REQUIRE(Project::validateName(QStringLiteral("rx front end")));
    REQUIRE_FALSE(Project::validateName(QString()));
    REQUIRE_FALSE(Project::validateName(QStringLiteral("  ")));
    REQUIRE_FALSE(Project::validateName(QStringLiteral("a/b")));
    REQUIRE_FALSE(Project::validateName(QStringLiteral("a:b")));
    REQUIRE_FALSE(Project::validateName(QStringLiteral("name.")));
    REQUIRE_FALSE(Project::validateName(QStringLiteral(" name")));
}

TEST_CASE("a project keeps its documents across save and reopen")
{
    QTemporaryDir dir;
    const QString projectDir = dir.filePath(QStringLiteral("radar"));
    QString error;

    QString projectFile;
    {
        auto project = Project::create(projectDir, QStringLiteral("radar"), &error);
        REQUIRE(project);
        REQUIRE(project->name() == QStringLiteral("radar"));
        REQUIRE(project->documents().isEmpty());
        projectFile = project->filePath();

        int added = 0;
        QObject::connect(project.get(), &Project::documentAdded, [&added] { ++added; });

        REQUIRE(project->createDocument(DocumentType::RfSchematic, QStringLiteral("receiver"), &error));
        REQUIRE(project->createDocument(DocumentType::SpiceSchematic, QStringLiteral("bias"), &error));
        REQUIRE(project->createDocument(DocumentType::AntennaArray, QStringLiteral("array"), &error));
        REQUIRE(added == 3);
        REQUIRE(QFileInfo::exists(QDir(projectDir).filePath(QStringLiteral("receiver.lvdrf"))));

        // Names are unique within a project, whatever the type or letter case.
        REQUIRE_FALSE(project->createDocument(DocumentType::AntennaArray, QStringLiteral("Receiver"), &error));
        REQUIRE_FALSE(project->createDocument(DocumentType::RfSchematic, QStringLiteral("a?b"), &error));
        REQUIRE(project->documents().size() == 3);

        // A second project cannot be created over the first.
        REQUIRE_FALSE(Project::create(projectDir, QStringLiteral("radar"), &error));
    }

    auto reopened = Project::load(projectFile, &error);
    REQUIRE(reopened);
    REQUIRE(reopened->loadWarnings().isEmpty());
    REQUIRE(reopened->documents().size() == 3);
    REQUIRE(reopened->document(QStringLiteral("bias"))->type() == DocumentType::SpiceSchematic);
    REQUIRE(reopened->document(QStringLiteral("array"))->type() == DocumentType::AntennaArray);

    SECTION("removing a document keeps or deletes its file as asked")
    {
        int removed = 0;
        QObject::connect(reopened.get(), &Project::documentAboutToBeRemoved, [&removed] { ++removed; });
        const QString biasPath = reopened->document(QStringLiteral("bias"))->filePath();
        const QString arrayPath = reopened->document(QStringLiteral("array"))->filePath();

        REQUIRE(reopened->removeDocument(reopened->document(QStringLiteral("bias")), false, &error));
        REQUIRE(reopened->removeDocument(reopened->document(QStringLiteral("array")), true, &error));
        REQUIRE(removed == 2);
        REQUIRE(QFileInfo::exists(biasPath));
        REQUIRE_FALSE(QFileInfo::exists(arrayPath));

        auto again = Project::load(projectFile, &error);
        REQUIRE(again);
        REQUIRE(again->documents().size() == 1);
    }

    SECTION("a missing document file is a warning and survives a save")
    {
        const QString biasPath = reopened->document(QStringLiteral("bias"))->filePath();
        reopened.reset();
        REQUIRE(QFile::rename(biasPath, biasPath + QStringLiteral(".moved")));

        auto partial = Project::load(projectFile, &error);
        REQUIRE(partial);
        REQUIRE(partial->documents().size() == 2);
        REQUIRE(partial->loadWarnings().size() == 1);
        REQUIRE(partial->save(&error));
        partial.reset();

        REQUIRE(QFile::rename(biasPath + QStringLiteral(".moved"), biasPath));
        auto complete = Project::load(projectFile, &error);
        REQUIRE(complete);
        REQUIRE(complete->documents().size() == 3);
    }
}

TEST_CASE("the session holds one project and announces changes")
{
    QTemporaryDir dir;
    Session session;
    QList<Project *> changes;
    int closing = 0;
    QObject::connect(&session, &Session::projectChanged, [&changes](Project *project) { changes.append(project); });
    QObject::connect(&session, &Session::projectAboutToClose, [&closing] { ++closing; });

    QString error;
    Project *first = session.newProject(dir.filePath(QStringLiteral("one")), QStringLiteral("one"), &error);
    REQUIRE(first);
    REQUIRE(session.project() == first);
    const QString firstFile = first->filePath();

    // A failed open leaves the current project in place.
    REQUIRE_FALSE(session.openProject(dir.filePath(QStringLiteral("nothing.lvdproj")), &error));
    REQUIRE(session.project() == first);
    REQUIRE(closing == 0);

    Project *second = session.newProject(dir.filePath(QStringLiteral("two")), QStringLiteral("two"), &error);
    REQUIRE(second);
    REQUIRE(closing == 1);
    REQUIRE(session.project()->name() == QStringLiteral("two"));

    session.closeProject();
    REQUIRE(session.project() == nullptr);
    REQUIRE(closing == 2);
    REQUIRE(changes.size() == 3);
    REQUIRE(changes.last() == nullptr);

    REQUIRE(session.openProject(firstFile, &error));
    REQUIRE(session.project()->name() == QStringLiteral("one"));
}

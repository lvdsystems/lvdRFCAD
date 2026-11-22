#pragma once

#include <QDialog>

#include <functional>

namespace lvd {

// Help > About: the LVD Systems logo and website, version, the bundled components with their versions and
// licences, and the licence texts (LICENSE, THIRD-PARTY-NOTICES.md).
class AboutDialog : public QDialog
{
    Q_OBJECT
public:
    // findFile maps a path relative to the installation (or source tree) to
    // an existing file, or returns an empty string.
    AboutDialog(const QString &pythonVersion, std::function<QString(const QString &)> findFile, QWidget *parent = nullptr);

    // The rows of the component table: name, version, licence.
    struct Component {
        QString name, version, licence;
    };
    static QList<Component> components(const QString &pythonVersion);
    static QString website();

private:
    void showText(const QString &title, const QString &relativePath, bool markdown);

    std::function<QString(const QString &)> m_findFile;
};

} // namespace lvd

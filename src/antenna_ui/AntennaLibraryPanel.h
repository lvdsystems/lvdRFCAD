#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QTreeWidget;

namespace lvd::ant {
class AntennaLibraryManager;
struct AntennaLibraryEntry;
} // namespace lvd::ant

namespace lvd::antui {

// The antenna library in the library dock: libraries, grouped by kind
// (element models, patterns, T/R modules, array presets). Activating an
// entry applies it to the current antenna document.
class AntennaLibraryPanel : public QWidget
{
    Q_OBJECT
public:
    explicit AntennaLibraryPanel(ant::AntennaLibraryManager *library, QWidget *parent = nullptr);

    void reload();
    int entryCount() const;
    QTreeWidget *tree() const { return m_tree; }

Q_SIGNALS:
    void entryActivated(const QString &key);

private:
    ant::AntennaLibraryManager *m_library;
    QLineEdit *m_filter;
    QTreeWidget *m_tree;
    QLabel *m_description;
};

} // namespace lvd::antui

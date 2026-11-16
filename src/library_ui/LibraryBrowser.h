#pragma once

#include "core/ComponentDefinition.h"

#include <QSet>
#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QMimeData;
class QModelIndex;
class QSortFilterProxyModel;
class QSplitter;
class QStandardItem;
class QStandardItemModel;
class QTextBrowser;
class QToolButton;
class QTreeView;

namespace lvd {

class Document;
class LibraryManager;
class Project;
class SymbolPreview;
class ComponentTreeView;

// Tree of the available components: library -> category path -> component,
// plus the schematic sheets of the current project. Components can be
// searched, previewed, dragged onto a schematic and placed.
class LibraryBrowser : public QWidget
{
    Q_OBJECT
public:
    explicit LibraryBrowser(LibraryManager *libraries, QWidget *parent = nullptr);

    // MIME type of a dragged component; the payload is the UTF-8 definition key.
    static QString mimeType();
    static QMimeData *createMimeData(const QString &definitionKey);

    // The project whose schematics are offered as sheets; nullptr for none.
    void setProject(Project *project);
    Project *project() const { return m_project; }

    // Filter text as typed in the search box.
    void setFilterText(const QString &text);
    QString filterText() const;
    // Shows only components with a model of one of these kinds (components
    // without models always show); empty shows everything. An RF schematic
    // and a SPICE schematic each see the parts they can simulate.
    void setModelKinds(const QStringList &kinds);
    QStringList modelKinds() const { return m_modelKinds; }

    // Selects and reveals a component; returns false if it is not shown.
    bool selectComponent(const QString &definitionKey);
    // Key of the selected component or sheet; empty when a library or category is selected.
    QString selectedKey() const;

    // Keys of all components and sheets in the tree, and of those that pass the filter.
    QStringList componentKeys() const;
    QStringList visibleComponentKeys() const;
    // Display path of a component: "library/category/.../name"; empty if not found.
    QString componentPath(const QString &definitionKey) const;

    QTreeView *treeView() const;

Q_SIGNALS:
    // Double-click, Enter or "Place" on a component or sheet.
    void componentActivated(const QString &definitionKey);
    void editComponentRequested(const QString &definitionKey);
    // "New component in this library..."; libraryName is empty when no library applies.
    void newComponentRequested(const QString &libraryName);
    void manageLibrariesRequested();

protected:
    void changeEvent(QEvent *event) override;

private:
    void rebuild();
    void addSheets();
    void onFilterChanged();
    void onCurrentChanged();
    void showContextMenu(const QPoint &position);
    void activateIndex(const QModelIndex &index);
    void deleteComponent(const QString &definitionKey);
    void updateWarnings();
    void showWarnings();

    QStandardItem *findItem(const QString &id) const;
    QStandardItem *findItem(QStandardItem *parent, const QString &id) const;
    QModelIndex proxyIndexOfKey(const QString &key) const;
    void collectExpanded(const QModelIndex &parent, QSet<QString> &expanded) const;
    void restoreExpanded(const QModelIndex &parent, const QSet<QString> &expanded);
    void collectKeys(const QModelIndex &parent, QStringList &keys, bool proxy) const;

    LibraryManager *m_libraries;
    Project *m_project = nullptr;
    QStandardItemModel *m_model;
    QSortFilterProxyModel *m_proxy;
    ComponentTreeView *m_tree;
    QLineEdit *m_search;
    QToolButton *m_warningButton;
    SymbolPreview *m_preview;
    QTextBrowser *m_details;
    QSplitter *m_splitter;
    QSet<QString> m_expandedBeforeFilter;
    bool m_filterActive = false;
    QStringList m_modelKinds;
    bool acceptsModels(const ComponentDefinition &definition) const;
    bool m_rebuilding = false;
    bool m_builtOnce = false;
    QSet<QString> m_knownTopLevel; // top-level rows seen before, to expand new ones
    Document *m_removingDocument = nullptr;
};

} // namespace lvd

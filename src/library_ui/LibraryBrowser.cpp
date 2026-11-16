#include "library_ui/LibraryBrowser.h"

#include "core/Document.h"
#include "core/Library.h"
#include "core/Project.h"
#include "library_ui/LibraryUiCommon.h"
#include "library_ui/SymbolPreview.h"
#include "widgets/SymbolPainter.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyle>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <map>

namespace lvd {

namespace {

enum Role {
    IdRole = Qt::UserRole + 1, // stable identity, used to keep expansion and selection
    KeyRole,                   // definition key of components and sheets
    TypeRole,
    SearchRole,                // lower-case text the search box matches
    SortRole,
    PlaceableRole,
};

enum ItemType { LibraryItem, CategoryItem, ComponentItem, SheetGroupItem, SheetItem };

QString folded(const QString &text)
{
    return text.toLower();
}

QString searchTextOf(const ComponentDefinition &definition, const QString &key)
{
    QStringList parts{definition.name, definition.id, definition.category, definition.description, key};
    for (const ParameterDefinition &parameter : definition.parameters) {
        parts.append(parameter.name);
        if (!parameter.label.isEmpty())
            parts.append(parameter.label);
    }
    return folded(parts.join(QLatin1Char('\n')));
}

// Matches components whose search text contains every word of the filter;
// libraries and categories show when a descendant matches.
class ComponentFilter : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setWords(const QString &text)
    {
        beginFilterChange();
        m_words = folded(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        if (m_words.isEmpty())
            return true;
        const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
        const int type = index.data(TypeRole).toInt();
        if (type != ComponentItem && type != SheetItem)
            return false; // shown through recursive filtering when a child matches
        const QString text = index.data(SearchRole).toString();
        for (const QString &word : m_words) {
            if (!text.contains(word))
                return false;
        }
        return true;
    }

private:
    QStringList m_words;
};

class ComponentModel : public QStandardItemModel
{
public:
    using QStandardItemModel::QStandardItemModel;

    QStringList mimeTypes() const override { return {LibraryBrowser::mimeType()}; }

    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        for (const QModelIndex &index : indexes) {
            const QString key = index.data(KeyRole).toString();
            if (!key.isEmpty() && index.data(PlaceableRole).toBool())
                return LibraryBrowser::createMimeData(key);
        }
        return nullptr;
    }
};

} // namespace

// Tree view that drags components with their symbol as the drag image and
// activates them with Enter.
class ComponentTreeView : public QTreeView
{
public:
    using QTreeView::QTreeView;

    std::function<void(const QModelIndex &)> onActivate;
    std::function<QPixmap(const QModelIndex &)> dragPixmap;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && currentIndex().isValid()
            && onActivate && !currentIndex().data(KeyRole).toString().isEmpty()) {
            onActivate(currentIndex());
            event->accept();
            return;
        }
        QTreeView::keyPressEvent(event);
    }

    void startDrag(Qt::DropActions supportedActions) override
    {
        const QModelIndex index = currentIndex();
        if (!index.isValid() || !index.data(PlaceableRole).toBool())
            return;
        QMimeData *mime = model()->mimeData({index});
        if (!mime)
            return;
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        if (dragPixmap) {
            const QPixmap pixmap = dragPixmap(index);
            if (!pixmap.isNull()) {
                drag->setPixmap(pixmap);
                drag->setHotSpot(QPoint(qRound(pixmap.width() / pixmap.devicePixelRatio() / 2),
                                        qRound(pixmap.height() / pixmap.devicePixelRatio() / 2)));
            }
        }
        drag->exec(supportedActions & Qt::CopyAction ? Qt::CopyAction : supportedActions, Qt::CopyAction);
    }
};

LibraryBrowser::LibraryBrowser(LibraryManager *libraries, QWidget *parent)
    : QWidget(parent), m_libraries(libraries)
{
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search name, category, parameter..."));
    m_search->setClearButtonEnabled(true);
    m_search->setToolTip(tr("Filters by name, id, category, description and parameter names.\n"
                            "Several words must all match."));

    auto *menuButton = new QToolButton(this);
    menuButton->setText(tr("Library"));
    menuButton->setToolTip(tr("Library actions"));
    menuButton->setPopupMode(QToolButton::InstantPopup);
    menuButton->setAutoRaise(true);
    auto *menu = new QMenu(menuButton);
    menu->addAction(tr("New Component..."), this, [this] {
        const QString key = selectedKey();
        QString libraryName, componentId;
        if (!LibraryManager::splitKey(key, &libraryName, &componentId) || libraryName == QLatin1String("sheet"))
            libraryName.clear();
        if (const QModelIndex current = m_tree->currentIndex(); current.isValid()) {
            QModelIndex index = current;
            while (index.isValid() && index.data(TypeRole).toInt() != LibraryItem)
                index = index.parent();
            if (index.isValid())
                libraryName = index.data(KeyRole).toString();
        }
        Q_EMIT newComponentRequested(libraryName);
    });
    menu->addAction(tr("Manage Libraries..."), this, &LibraryBrowser::manageLibrariesRequested);
    menu->addSeparator();
    menu->addAction(tr("Expand All"), this, [this] { m_tree->expandAll(); });
    menu->addAction(tr("Collapse All"), this, [this] { m_tree->collapseAll(); });
    menu->addAction(tr("Reload Libraries"), this, [this] { m_libraries->reload(); });
    menuButton->setMenu(menu);

    m_warningButton = new QToolButton(this);
    m_warningButton->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
    m_warningButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_warningButton->setAutoRaise(true);
    m_warningButton->hide();
    connect(m_warningButton, &QToolButton::clicked, this, &LibraryBrowser::showWarnings);

    m_model = new ComponentModel(this);
    m_proxy = new ComponentFilter(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setRecursiveFilteringEnabled(true);
    m_proxy->setSortRole(SortRole);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);

    m_tree = new ComponentTreeView(this);
    m_tree->setModel(m_proxy);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setIconSize(QSize(28, 20));
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setSortingEnabled(false);
    m_tree->onActivate = [this](const QModelIndex &index) { activateIndex(index); };
    m_tree->dragPixmap = [this](const QModelIndex &index) -> QPixmap {
        const QString key = index.data(KeyRole).toString();
        DefinitionPtr definition = key.startsWith(QLatin1String("sheet:")) && m_project
                                       ? m_project->sheetDefinition(key.mid(6))
                                       : m_libraries->find(key);
        if (!definition)
            return {};
        // Roughly the size the symbol has on a schematic at normal zoom.
        const QRectF bounds = SymbolPainter::boundingRect(definition->symbol);
        const QSize size(qBound(24, qRound(bounds.width() * 2.0), 400), qBound(24, qRound(bounds.height() * 2.0), 400));
        return symbolPixmap(definition->symbol, size, styleFor(this), false, devicePixelRatioF(), 2.0);
    };

    m_preview = new SymbolPreview(this);
    m_preview->setPlaceholderText(tr("Select a component to see its symbol."));
    m_preview->setMinimumHeight(110);
    m_preview->setMaximumScale(3);

    m_details = new QTextBrowser(this);
    m_details->setOpenLinks(false);
    m_details->setFrameShape(QFrame::NoFrame);

    auto *detailPane = new QWidget(this);
    auto *detailLayout = new QVBoxLayout(detailPane);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->addWidget(m_preview, 2);
    detailLayout->addWidget(m_details, 3);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->addWidget(m_tree);
    m_splitter->addWidget(detailPane);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setChildrenCollapsible(false);

    auto *searchRow = new QHBoxLayout;
    searchRow->setSpacing(2);
    searchRow->addWidget(m_search, 1);
    searchRow->addWidget(menuButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(3);
    layout->addLayout(searchRow);
    layout->addWidget(m_warningButton, 0, Qt::AlignLeft);
    layout->addWidget(m_splitter, 1);

    connect(m_search, &QLineEdit::textChanged, this, &LibraryBrowser::onFilterChanged);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, &LibraryBrowser::onCurrentChanged);
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (!index.data(KeyRole).toString().isEmpty())
            activateIndex(index);
    });
    connect(m_tree, &QWidget::customContextMenuRequested, this, &LibraryBrowser::showContextMenu);
    connect(m_libraries, &LibraryManager::librariesChanged, this, &LibraryBrowser::rebuild);

    rebuild();
}

QString LibraryBrowser::mimeType()
{
    return QStringLiteral("application/x-lvdrfcad-component");
}

QMimeData *LibraryBrowser::createMimeData(const QString &definitionKey)
{
    auto *mime = new QMimeData;
    mime->setData(mimeType(), definitionKey.toUtf8());
    mime->setText(definitionKey);
    return mime;
}

QTreeView *LibraryBrowser::treeView() const
{
    return m_tree;
}

void LibraryBrowser::setProject(Project *project)
{
    if (m_project == project)
        return;
    if (m_project)
        disconnect(m_project, nullptr, this, nullptr);
    m_project = project;
    if (project) {
        connect(project, &Project::documentAdded, this, &LibraryBrowser::rebuild);
        connect(project, &Project::documentAboutToBeRemoved, this, [this](Document *document) {
            // The document is still listed while this signal runs.
            m_removingDocument = document;
            rebuild();
            m_removingDocument = nullptr;
        });
        connect(project, &QObject::destroyed, this, [this] {
            m_project = nullptr;
            rebuild();
        });
    }
    rebuild();
}

void LibraryBrowser::setFilterText(const QString &text)
{
    m_search->setText(text);
}

QString LibraryBrowser::filterText() const
{
    return m_search->text();
}

QStandardItem *LibraryBrowser::findItem(QStandardItem *parent, const QString &id) const
{
    for (int row = 0; row < parent->rowCount(); ++row) {
        QStandardItem *child = parent->child(row);
        if (child->data(IdRole).toString() == id)
            return child;
        if (QStandardItem *found = findItem(child, id))
            return found;
    }
    return nullptr;
}

QStandardItem *LibraryBrowser::findItem(const QString &id) const
{
    return findItem(m_model->invisibleRootItem(), id);
}

QModelIndex LibraryBrowser::proxyIndexOfKey(const QString &key) const
{
    // Prefer the placeable item: a shadowed library may list the same key.
    QModelIndex fallback;
    std::function<QModelIndex(const QModelIndex &)> search = [&](const QModelIndex &parent) -> QModelIndex {
        for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
            const QModelIndex index = m_proxy->index(row, 0, parent);
            if (index.data(KeyRole).toString() == key && index.data(TypeRole).toInt() != LibraryItem) {
                if (index.data(PlaceableRole).toBool())
                    return index;
                if (!fallback.isValid())
                    fallback = index;
            }
            const QModelIndex found = search(index);
            if (found.isValid())
                return found;
        }
        return {};
    };
    const QModelIndex found = search(QModelIndex());
    return found.isValid() ? found : fallback;
}

bool LibraryBrowser::selectComponent(const QString &definitionKey)
{
    const QModelIndex index = proxyIndexOfKey(definitionKey);
    if (!index.isValid())
        return false;
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent())
        m_tree->expand(parent);
    m_tree->setCurrentIndex(index);
    m_tree->scrollTo(index);
    return true;
}

QString LibraryBrowser::selectedKey() const
{
    const QModelIndex index = m_tree->currentIndex();
    if (!index.isValid() || index.data(TypeRole).toInt() == LibraryItem)
        return {};
    return index.data(KeyRole).toString();
}

void LibraryBrowser::collectKeys(const QModelIndex &parent, QStringList &keys, bool proxy) const
{
    const QAbstractItemModel *model = proxy ? static_cast<const QAbstractItemModel *>(m_proxy) : m_model;
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        const int type = index.data(TypeRole).toInt();
        if (type == ComponentItem || type == SheetItem)
            keys.append(index.data(KeyRole).toString());
        collectKeys(index, keys, proxy);
    }
}

QStringList LibraryBrowser::componentKeys() const
{
    QStringList keys;
    collectKeys(QModelIndex(), keys, false);
    return keys;
}

QStringList LibraryBrowser::visibleComponentKeys() const
{
    QStringList keys;
    collectKeys(QModelIndex(), keys, true);
    return keys;
}

QString LibraryBrowser::componentPath(const QString &definitionKey) const
{
    QModelIndex index = proxyIndexOfKey(definitionKey);
    if (!index.isValid())
        return {};
    QStringList parts;
    for (; index.isValid(); index = index.parent()) {
        const int type = index.data(TypeRole).toInt();
        if (type == LibraryItem)
            parts.prepend(index.data(KeyRole).toString()); // the plain library name
        else if (type == SheetGroupItem)
            parts.prepend(QStringLiteral("sheet"));
        else
            parts.prepend(index.data(Qt::DisplayRole).toString());
    }
    return parts.join(QLatin1Char('/'));
}

void LibraryBrowser::collectExpanded(const QModelIndex &parent, QSet<QString> &expanded) const
{
    for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
        const QModelIndex index = m_proxy->index(row, 0, parent);
        if (m_tree->isExpanded(index)) {
            expanded.insert(index.data(IdRole).toString());
            collectExpanded(index, expanded);
        }
    }
}

void LibraryBrowser::restoreExpanded(const QModelIndex &parent, const QSet<QString> &expanded)
{
    for (int row = 0; row < m_proxy->rowCount(parent); ++row) {
        const QModelIndex index = m_proxy->index(row, 0, parent);
        if (expanded.contains(index.data(IdRole).toString())) {
            m_tree->expand(index);
            restoreExpanded(index, expanded);
        }
    }
}

namespace {

QIcon componentIcon(const Symbol &symbol, const SchematicStyle &style, double ratio)
{
    return QIcon(symbolPixmap(symbol, QSize(28, 20), style, false, ratio, 1.2));
}

} // namespace

void LibraryBrowser::setModelKinds(const QStringList &kinds)
{
    if (kinds == m_modelKinds)
        return;
    m_modelKinds = kinds;
    rebuild();
}

bool LibraryBrowser::acceptsModels(const ComponentDefinition &definition) const
{
    if (m_modelKinds.isEmpty() || definition.models.isEmpty())
        return true; // ground, ports and other model-less parts fit every schematic
    for (const ModelDefinition &model : definition.models)
        if (m_modelKinds.contains(model.kind))
            return true;
    return false;
}

void LibraryBrowser::rebuild()
{
    const bool firstBuild = !m_builtOnce;
    QSet<QString> expanded;
    collectExpanded(QModelIndex(), expanded);
    if (m_filterActive)
        expanded = m_expandedBeforeFilter;
    const QModelIndex current = m_tree->currentIndex();
    const QString currentId = current.isValid() ? current.data(IdRole).toString() : QString();
    const int scroll = m_tree->verticalScrollBar()->value();

    m_rebuilding = true;
    m_model->clear();

    const SchematicStyle schematicStyle = styleFor(this);
    const double ratio = devicePixelRatioF();
    const QIcon folderIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QColor greyed = palette().color(QPalette::Disabled, QPalette::Text);

    QStringList initiallyExpanded;
    for (const LibraryManager::Entry &entry : m_libraries->entries()) {
        const Library &library = entry.library;
        const int scopeIndex = static_cast<int>(entry.scope);
        const QString libraryId = QStringLiteral("l/%1/%2").arg(scopeIndex).arg(library.name);

        auto *libraryItem = new QStandardItem(tr("%1  (%2)").arg(library.name, scopeDisplayName(entry.scope)));
        libraryItem->setData(libraryId, IdRole);
        libraryItem->setData(library.name, KeyRole);
        libraryItem->setData(LibraryItem, TypeRole);
        libraryItem->setData(QStringLiteral("1%1%2").arg(scopeIndex).arg(library.name), SortRole);
        libraryItem->setDragEnabled(false);
        libraryItem->setEditable(false);
        QFont font = libraryItem->font();
        font.setBold(true);
        QString tooltip = tr("Library \"%1\" (%2)\n%3\n%n component(s)", nullptr, static_cast<int>(library.components.size()))
                              .arg(library.name, scopeDisplayName(entry.scope), QDir::toNativeSeparators(library.filePath));
        if (!library.description.isEmpty())
            tooltip += QLatin1Char('\n') + library.description;
        if (!isWritableScope(entry.scope))
            tooltip += QLatin1Char('\n') + tr("Read-only: copy components to a user or project library to change them.");
        if (const LibraryManager::Entry *winner = shadowingEntry(*m_libraries, entry)) {
            font.setBold(false);
            font.setItalic(true);
            libraryItem->setForeground(greyed);
            libraryItem->setText(tr("%1  (%2, hidden)").arg(library.name, scopeDisplayName(entry.scope)));
            tooltip += QLatin1Char('\n')
                       + tr("Hidden: the %1 library \"%2\" (%3) has the same name and is used instead.")
                             .arg(scopeDisplayName(winner->scope), winner->library.name,
                                  QDir::toNativeSeparators(winner->library.filePath));
        } else {
            initiallyExpanded.append(libraryId);
        }
        libraryItem->setFont(font);
        libraryItem->setToolTip(tooltip);

        std::map<QString, QStandardItem *> categories;
        const std::function<QStandardItem *(const QString &)> categoryItem = [&](const QString &path) -> QStandardItem * {
            if (path.isEmpty())
                return libraryItem;
            const auto it = categories.find(path);
            if (it != categories.end())
                return it->second;
            const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
            QStandardItem *parent = categoryItem(slash < 0 ? QString() : path.left(slash));
            const QString name = path.mid(slash + 1);
            auto *item = new QStandardItem(folderIcon, name);
            item->setData(QStringLiteral("c/%1/%2/%3").arg(scopeIndex).arg(library.name, path), IdRole);
            item->setData(CategoryItem, TypeRole);
            item->setData(QStringLiteral("0") + name, SortRole);
            item->setDragEnabled(false);
            item->setEditable(false);
            if (entry.shadowed)
                item->setForeground(greyed);
            parent->appendRow(item);
            categories.emplace(path, item);
            return item;
        };

        int shown = 0;
        for (const DefinitionPtr &definition : library.components) {
            if (!acceptsModels(*definition))
                continue;
            ++shown;
            const QString key = LibraryManager::makeKey(library.name, definition->id);
            QStringList path = definition->category.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            for (QString &part : path)
                part = part.trimmed();
            auto *item = new QStandardItem(componentIcon(definition->symbol, schematicStyle, ratio), definition->name);
            item->setData(QStringLiteral("k/%1/%2").arg(scopeIndex).arg(key), IdRole);
            item->setData(key, KeyRole);
            item->setData(ComponentItem, TypeRole);
            item->setData(searchTextOf(*definition, key), SearchRole);
            item->setData(QStringLiteral("1") + definition->name, SortRole);
            item->setData(!entry.shadowed, PlaceableRole);
            item->setDragEnabled(!entry.shadowed);
            item->setEditable(false);
            QString itemTip = QStringLiteral("<b>%1</b><br>%2").arg(definition->name.toHtmlEscaped(), key.toHtmlEscaped());
            if (!definition->description.isEmpty())
                itemTip += QStringLiteral("<br>") + definition->description.toHtmlEscaped();
            if (entry.shadowed) {
                item->setForeground(greyed);
                itemTip += QStringLiteral("<br><i>%1</i>").arg(tr("Hidden by a library with the same name; not placeable.").toHtmlEscaped());
            } else {
                itemTip += QStringLiteral("<br><i>%1</i>").arg(tr("Drag onto a schematic or double-click to place.").toHtmlEscaped());
            }
            item->setToolTip(itemTip);
            categoryItem(path.join(QLatin1Char('/')))->appendRow(item);
        }
        // A library with nothing for this kind of schematic is left out.
        if (shown == 0 && !library.components.empty()) {
            delete libraryItem;
            continue;
        }
        m_model->appendRow(libraryItem);
    }

    addSheets();
    initiallyExpanded.append(QStringLiteral("sheets"));

    m_proxy->sort(0);
    m_rebuilding = false;

    if (m_filterActive) {
        m_expandedBeforeFilter = expanded;
        m_tree->expandAll();
    } else if (firstBuild) {
        restoreExpanded(QModelIndex(), QSet<QString>(initiallyExpanded.begin(), initiallyExpanded.end()));
    } else {
        // New libraries and the sheets group start expanded.
        for (const QString &id : initiallyExpanded) {
            if (!m_knownTopLevel.contains(id))
                expanded.insert(id);
        }
        restoreExpanded(QModelIndex(), expanded);
    }
    m_knownTopLevel = QSet<QString>(initiallyExpanded.begin(), initiallyExpanded.end());
    m_builtOnce = true;

    if (QStandardItem *item = currentId.isEmpty() ? nullptr : findItem(currentId)) {
        const QModelIndex index = m_proxy->mapFromSource(item->index());
        if (index.isValid())
            m_tree->setCurrentIndex(index);
    }
    m_tree->verticalScrollBar()->setValue(scroll);
    onCurrentChanged();
    updateWarnings();
}

void LibraryBrowser::addSheets()
{
    if (!m_project)
        return;
    QList<Document *> sheets;
    for (Document *document : m_project->documents()) {
        if (document->isSchematic() && document != m_removingDocument)
            sheets.append(document);
    }
    if (sheets.isEmpty())
        return;

    auto *group = new QStandardItem(style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Sheets"));
    group->setData(QStringLiteral("sheets"), IdRole);
    group->setData(SheetGroupItem, TypeRole);
    group->setData(QStringLiteral("0"), SortRole);
    group->setDragEnabled(false);
    group->setEditable(false);
    QFont font = group->font();
    font.setBold(true);
    group->setFont(font);
    group->setToolTip(tr("Schematics of project \"%1\", placeable as hierarchical blocks").arg(m_project->name()));

    const SchematicStyle schematicStyle = styleFor(this);
    for (Document *document : sheets) {
        const QString key = Project::sheetKey(document->name());
        const DefinitionPtr definition = m_project->sheetDefinition(document->name());
        auto *item = definition ? new QStandardItem(componentIcon(definition->symbol, schematicStyle, devicePixelRatioF()),
                                                    document->name())
                                : new QStandardItem(document->name());
        item->setData(QStringLiteral("s/") + document->name(), IdRole);
        item->setData(key, KeyRole);
        item->setData(SheetItem, TypeRole);
        item->setData(folded(document->name() + QLatin1Char('\n') + key + QStringLiteral("\nsheet hierarchy")
                            + (definition ? QLatin1Char('\n') + searchTextOf(*definition, key) : QString())),
                      SearchRole);
        item->setData(QStringLiteral("1") + document->name(), SortRole);
        item->setData(true, PlaceableRole);
        item->setEditable(false);
        item->setToolTip(tr("Schematic \"%1\" as a block (%2)").arg(document->name(), key));
        group->appendRow(item);
    }
    m_model->appendRow(group);
}

void LibraryBrowser::onFilterChanged()
{
    const QString text = m_search->text().trimmed();
    const bool active = !text.isEmpty();
    if (active && !m_filterActive) {
        m_expandedBeforeFilter.clear();
        collectExpanded(QModelIndex(), m_expandedBeforeFilter);
    }
    static_cast<ComponentFilter *>(m_proxy)->setWords(text);
    if (active) {
        m_tree->expandAll();
    } else if (m_filterActive) {
        m_tree->collapseAll();
        restoreExpanded(QModelIndex(), m_expandedBeforeFilter);
        if (m_tree->currentIndex().isValid())
            m_tree->scrollTo(m_tree->currentIndex());
    }
    m_filterActive = active;

    // Select the first match so its preview shows while typing.
    if (active && !m_tree->currentIndex().isValid()) {
        const QStringList keys = visibleComponentKeys();
        if (!keys.isEmpty())
            selectComponent(keys.first());
    }
}

void LibraryBrowser::onCurrentChanged()
{
    if (m_rebuilding)
        return;
    const QModelIndex index = m_tree->currentIndex();
    const QString key = index.isValid() ? index.data(KeyRole).toString() : QString();
    const int type = index.isValid() ? index.data(TypeRole).toInt() : -1;

    DefinitionPtr definition;
    if (type == SheetItem && m_project)
        definition = m_project->sheetDefinition(key.mid(6));
    else if (type == ComponentItem) {
        // A shadowed library's component is shown from that library, not the winner.
        QString libraryName, componentId;
        LibraryManager::splitKey(key, &libraryName, &componentId);
        const QString id = index.data(IdRole).toString();
        for (const LibraryManager::Entry &entry : m_libraries->entries()) {
            if (entry.library.name == libraryName
                && id == QStringLiteral("k/%1/%2").arg(static_cast<int>(entry.scope)).arg(key)) {
                definition = entry.library.component(componentId);
                break;
            }
        }
    }

    if (!definition) {
        m_preview->clear();
        if (type == LibraryItem) {
            m_details->setHtml(QStringLiteral("<p>%1</p>").arg(index.data(Qt::ToolTipRole).toString().toHtmlEscaped().replace(
                QLatin1Char('\n'), QStringLiteral("<br>"))));
        } else {
            m_details->clear();
        }
        return;
    }

    m_preview->setSymbol(definition->symbol);

    QString html;
    html += QStringLiteral("<p style='margin-bottom:2px'><b style='font-size:large'>%1</b></p>").arg(definition->name.toHtmlEscaped());
    html += QStringLiteral("<p style='margin-top:0'><code>%1</code></p>").arg(key.toHtmlEscaped());
    if (!definition->description.isEmpty())
        html += QStringLiteral("<p>%1</p>").arg(definition->description.toHtmlEscaped());
    html += QStringLiteral("<p><b>%1</b> %2</p>").arg(tr("Pins:"), definition->pins.join(QStringLiteral(", ")).toHtmlEscaped());
    if (!definition->category.isEmpty())
        html += QStringLiteral("<p><b>%1</b> %2 &nbsp; <b>%3</b> %4</p>")
                    .arg(tr("Category:"), definition->category.toHtmlEscaped(), tr("Reference:"),
                         definition->referencePrefix.toHtmlEscaped());
    if (!definition->parameters.isEmpty()) {
        html += QStringLiteral("<table cellspacing='0' cellpadding='2' width='100%'><tr><th align='left'>%1</th>"
                               "<th align='left'>%2</th><th align='left'>%3</th></tr>")
                    .arg(tr("Parameter"), tr("Default"), tr("Unit"));
        for (const ParameterDefinition &parameter : definition->parameters) {
            QString name = parameter.name.toHtmlEscaped();
            if (!parameter.label.isEmpty())
                name = QStringLiteral("<span title='%1'>%2</span>").arg(parameter.label.toHtmlEscaped(), name);
            QString value = parameter.defaultValue.toHtmlEscaped();
            if (!parameter.choices.isEmpty())
                value += QStringLiteral(" <i>(%1)</i>").arg(parameter.choices.join(QStringLiteral(", ")).toHtmlEscaped());
            else if (parameter.isFile)
                value += QStringLiteral(" <i>(%1)</i>").arg(tr("file"));
            html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td></tr>")
                        .arg(name, value, parameter.unit.toHtmlEscaped());
        }
        html += QStringLiteral("</table>");
    }
    if (!definition->models.isEmpty()) {
        QStringList models;
        for (const ModelDefinition &model : definition->models) {
            QString detail;
            for (auto it = model.data.begin(); it != model.data.end(); ++it) {
                if (it.key() != QLatin1String("kind") && it.value().isString()) {
                    detail = it.value().toString();
                    break;
                }
            }
            models.append(detail.isEmpty() ? model.kind.toHtmlEscaped()
                                           : QStringLiteral("%1 <i>%2</i>").arg(model.kind.toHtmlEscaped(),
                                                                                detail.left(60).toHtmlEscaped()));
        }
        html += QStringLiteral("<p><b>%1</b> %2</p>").arg(tr("Models:"), models.join(QStringLiteral(", ")));
    } else if (!definition->netName.isEmpty()) {
        html += QStringLiteral("<p><b>%1</b> %2</p>").arg(tr("Global net:"), definition->netName.toHtmlEscaped());
    }
    m_details->setHtml(html);
}

void LibraryBrowser::activateIndex(const QModelIndex &index)
{
    const QString key = index.data(KeyRole).toString();
    if (key.isEmpty() || index.data(TypeRole).toInt() == LibraryItem)
        return;
    if (!index.data(PlaceableRole).toBool())
        return;
    Q_EMIT componentActivated(key);
}

void LibraryBrowser::showContextMenu(const QPoint &position)
{
    const QModelIndex index = m_tree->indexAt(position);
    QMenu menu(this);
    const int type = index.isValid() ? index.data(TypeRole).toInt() : -1;
    const QString key = index.isValid() ? index.data(KeyRole).toString() : QString();

    // The library this row belongs to.
    QModelIndex libraryIndex = index;
    while (libraryIndex.isValid() && libraryIndex.data(TypeRole).toInt() != LibraryItem)
        libraryIndex = libraryIndex.parent();
    const LibraryManager::Entry *entry = nullptr;
    if (libraryIndex.isValid()) {
        const QString id = libraryIndex.data(IdRole).toString();
        for (const LibraryManager::Entry &candidate : m_libraries->entries()) {
            if (id == QStringLiteral("l/%1/%2").arg(static_cast<int>(candidate.scope)).arg(candidate.library.name))
                entry = &candidate;
        }
    }
    const bool writable = entry && !entry->shadowed && isWritableScope(entry->scope);
    const QString libraryName = entry ? entry->library.name : QString();

    if (type == ComponentItem || type == SheetItem) {
        QAction *place = menu.addAction(tr("Place"), this, [this, index] { activateIndex(index); });
        place->setEnabled(index.data(PlaceableRole).toBool());
        if (type == ComponentItem) {
            menu.addAction(tr("Edit Symbol..."), this, [this, key] { Q_EMIT editComponentRequested(key); });
        }
        menu.addAction(tr("Copy Key"), this, [key] { QApplication::clipboard()->setText(key); });
    }
    if (entry) {
        if (!menu.isEmpty())
            menu.addSeparator();
        QAction *create = menu.addAction(tr("New Component in This Library..."), this,
                                         [this, libraryName] { Q_EMIT newComponentRequested(libraryName); });
        create->setEnabled(writable);
        if (!writable)
            create->setToolTip(tr("Built-in and hidden libraries are read-only."));
        if (type == ComponentItem) {
            QAction *remove = menu.addAction(tr("Delete Component"), this, [this, key] { deleteComponent(key); });
            remove->setEnabled(writable);
        }
        const QString folder = QFileInfo(entry->library.filePath).absolutePath();
        menu.addAction(tr("Open Library Folder"), this,
                       [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
    }
    if (!menu.isEmpty())
        menu.addSeparator();
    menu.addAction(tr("New Component..."), this, [this, writable, libraryName] {
        Q_EMIT newComponentRequested(writable ? libraryName : QString());
    });
    menu.addAction(tr("Manage Libraries..."), this, &LibraryBrowser::manageLibrariesRequested);
    menu.setToolTipsVisible(true);
    menu.exec(m_tree->viewport()->mapToGlobal(position));
}

void LibraryBrowser::deleteComponent(const QString &definitionKey)
{
    QString libraryName, componentId;
    if (!LibraryManager::splitKey(definitionKey, &libraryName, &componentId))
        return;
    const LibraryManager::Entry *entry = effectiveEntry(*m_libraries, libraryName);
    if (!entry || !isWritableScope(entry->scope))
        return;
    const DefinitionPtr definition = entry->library.component(componentId);
    if (!definition)
        return;
    const auto answer = QMessageBox::question(
        this, tr("Delete Component"),
        tr("Delete \"%1\" (%2) from library \"%3\"?\n\nSchematics that use it keep working from their cached "
           "copy, but they will be flagged.")
            .arg(definition->name, definitionKey, libraryName),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    Library library = entry->library;
    library.removeComponent(componentId);
    QString error;
    if (!m_libraries->saveLibrary(library, entry->scope, &error))
        QMessageBox::warning(this, tr("Delete Component"), tr("Could not save the library:\n%1").arg(error));
}

void LibraryBrowser::updateWarnings()
{
    const QStringList warnings = m_libraries->warnings();
    m_warningButton->setVisible(!warnings.isEmpty());
    m_warningButton->setText(tr("%n library warning(s)", nullptr, static_cast<int>(warnings.size())));
    QStringList shown = warnings.mid(0, 10);
    if (warnings.size() > 10)
        shown.append(tr("... and %n more", nullptr, static_cast<int>(warnings.size() - 10)));
    m_warningButton->setToolTip(shown.join(QLatin1Char('\n')));
}

void LibraryBrowser::showWarnings()
{
    QMessageBox box(QMessageBox::Warning, tr("Library Warnings"),
                    tr("Some library files or components could not be loaded:"), QMessageBox::Close, this);
    box.setDetailedText(m_libraries->warnings().join(QLatin1Char('\n')));
    box.setInformativeText(m_libraries->warnings().mid(0, 5).join(QLatin1Char('\n')));
    box.exec();
}

void LibraryBrowser::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // Icons are drawn in the theme's colours.
    if (event->type() == QEvent::PaletteChange && m_builtOnce)
        rebuild();
}

} // namespace lvd

#include "gui/AboutDialog.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QTableWidget>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace lvd {

QString AboutDialog::website()
{
    return QStringLiteral("http://www.lvdsystems.eu");
}

QList<AboutDialog::Component> AboutDialog::components(const QString &pythonVersion)
{
    return {
        {QStringLiteral("Qt"), QString::fromLatin1(qVersion()), QStringLiteral("LGPL-3.0")},
        {QStringLiteral("Python"), pythonVersion, QStringLiteral("PSF License 2")},
        {QStringLiteral("pybind11"), QStringLiteral(LVD_PYBIND11_VERSION), QStringLiteral("BSD-3-Clause")},
        {QStringLiteral("Eigen"), QStringLiteral(LVD_EIGEN_VERSION), QStringLiteral("MPL-2.0")},
        {QStringLiteral("ngspice"), QStringLiteral(LVD_NGSPICE_VERSION_TEXT), QStringLiteral("Modified BSD (see COPYING)")},
        {QStringLiteral("openEMS"), QStringLiteral(LVD_OPENEMS_VERSION_TEXT), QStringLiteral("GPL-3.0-or-later")},
        {QStringLiteral("HDF5"), QStringLiteral(LVD_HDF5_VERSION), QStringLiteral("BSD-3-Clause")},
    };
}

AboutDialog::AboutDialog(const QString &pythonVersion, std::function<QString(const QString &)> findFile, QWidget *parent)
    : QDialog(parent)
    , m_findFile(std::move(findFile))
{
    setWindowTitle(tr("About lvd RF cad"));
    auto *layout = new QVBoxLayout(this);
    // The LVD Systems logo has white lettering, so it sits on a dark band in both themes.
    auto *banner = new QLabel;
    banner->setObjectName(QStringLiteral("logo"));
    banner->setPixmap(QPixmap(QStringLiteral(":/lvd/lvd-systems-logo.png")));
    banner->setAlignment(Qt::AlignCenter);
    banner->setMinimumHeight(100);
    banner->setStyleSheet(QStringLiteral("QLabel { background: #262626; border-radius: 6px; padding: 14px; }"));
    layout->addWidget(banner);

    auto *title = new QLabel(tr("<h2>lvd RF cad %1</h2>"
                                "<p>RF system and circuit design, SPICE simulation and antenna design.</p>"
                                "<p>LVD Systems &mdash; <a href=\"%2\">%2</a></p>"
                                "<p>Copyright © 2026 the lvd RF cad authors. Licensed under the GNU General Public "
                                "License, version 3 or later. This program comes with ABSOLUTELY NO WARRANTY.</p>")
                                 .arg(QStringLiteral(LVD_VERSION), website()));
    title->setObjectName(QStringLiteral("title"));
    title->setOpenExternalLinks(true);
    title->setWordWrap(true);
    title->setTextFormat(Qt::RichText);
    layout->addWidget(title);

    layout->addWidget(new QLabel(tr("Bundled components:")));
    const QList<Component> rows = components(pythonVersion);
    auto *table = new QTableWidget(int(rows.size()), 3);
    table->setObjectName(QStringLiteral("components"));
    table->setHorizontalHeaderLabels({tr("Component"), tr("Version"), tr("Licence")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->horizontalHeader()->setStretchLastSection(true);
    for (int i = 0; i < rows.size(); ++i) {
        table->setItem(i, 0, new QTableWidgetItem(rows[i].name));
        table->setItem(i, 1, new QTableWidgetItem(rows[i].version));
        table->setItem(i, 2, new QTableWidgetItem(rows[i].licence));
    }
    table->resizeColumnsToContents();
    table->horizontalHeader()->setStretchLastSection(true);
    table->setMinimumHeight(table->verticalHeader()->length() + table->horizontalHeader()->height() + 4);
    layout->addWidget(table);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *notices = buttons->addButton(tr("Third-Party Notices..."), QDialogButtonBox::ActionRole);
    auto *licence = buttons->addButton(tr("Licence..."), QDialogButtonBox::ActionRole);
    connect(notices, &QPushButton::clicked, this,
            [this] { showText(tr("Third-Party Notices"), QStringLiteral("THIRD-PARTY-NOTICES.md"), true); });
    connect(licence, &QPushButton::clicked, this, [this] { showText(tr("Licence"), QStringLiteral("LICENSE"), false); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(560, 580);
}

void AboutDialog::showText(const QString &title, const QString &relativePath, bool markdown)
{
    const QString path = m_findFile ? m_findFile(relativePath) : QString();
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, title, tr("%1 was not found next to the program.").arg(relativePath));
        return;
    }
    // A window of the main window, so it never keeps the application open.
    auto *view = new QTextBrowser(parentWidget() ? parentWidget() : this);
    view->setWindowFlag(Qt::Window);
    view->setAttribute(Qt::WA_DeleteOnClose);
    view->setWindowTitle(title);
    view->setOpenExternalLinks(true);
    const QString text = QString::fromUtf8(file.readAll());
    if (markdown) {
        view->setMarkdown(text);
    } else {
        view->setFont(QFont(QStringLiteral("Consolas"), 9));
        view->setPlainText(text);
    }
    view->resize(820, 700);
    view->show();
}

} // namespace lvd

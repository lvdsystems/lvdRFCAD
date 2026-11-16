#include "gui/DocumentView.h"

#include "core/Document.h"

#include <QDir>
#include <QLabel>
#include <QVBoxLayout>

namespace lvd {

DocumentView::DocumentView(Document *document, QWidget *parent) : QWidget(parent), m_document(document)
{
    auto *title = new QLabel(document->name(), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.6);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);

    auto *details = new QLabel(
        tr("%1\n%2\n\nThe editor for this document type is not available yet.")
            .arg(documentTypeInfo(document->type()).displayName, QDir::toNativeSeparators(document->filePath())),
        this);
    details->setAlignment(Qt::AlignCenter);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->setForegroundRole(QPalette::PlaceholderText);

    auto *layout = new QVBoxLayout(this);
    layout->addStretch();
    layout->addWidget(title);
    layout->addWidget(details);
    layout->addStretch();
}

} // namespace lvd

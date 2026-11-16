#pragma once

#include <QWidget>

namespace lvd {

class Document;

// The tab content for one open document. Until the editors exist it shows
// the document's identity only.
class DocumentView : public QWidget
{
    Q_OBJECT
public:
    explicit DocumentView(Document *document, QWidget *parent = nullptr);

    Document *document() const { return m_document; }

private:
    Document *m_document;
};

} // namespace lvd

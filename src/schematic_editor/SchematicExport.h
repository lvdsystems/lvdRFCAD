#pragma once

#include <QJsonObject>
#include <QRectF>
#include <QString>
#include <QStringList>

class QPainter;
class QPrinter;

namespace lvd {

class Document;
class Project;
class SchematicScene;

// Printing and export of schematics (requirement PL-12). The drawing is
// rendered from its own scene in the light style, without grid, selection or
// highlights. Printed and PDF pages carry a frame and a title block (project,
// document, the schematic's title-block fields, date, program version); SVG
// and PNG images hold the drawing only.
class SchematicExport
{
public:
    // By the file extension: .pdf (A4, orientation from the drawing), .svg or
    // .png (200 dpi equivalent).
    static bool exportFile(Project *project, Document *document, const QString &path, QString *error = nullptr);
    // On a printer set up by the caller (see landscape()).
    static bool print(Project *project, Document *document, QPrinter *printer, QString *error = nullptr);
    // True when the drawing is wider than tall: the page orientation to offer.
    static bool landscape(Project *project, Document *document);

    // The text of the title block, for tests and previews: one entry per line.
    static QStringList titleBlockLines(Project *project, Document *document);

    // Paints one page: frame, drawing fitted into the area left by the title
    // block, and the title block. `page` is the printable area in device
    // units; `dotsPerMm` converts millimetres to device units.
    static void paintPage(QPainter &painter, const QRectF &page, double dotsPerMm, SchematicScene &scene,
                          const QStringList &titleLines);
};

} // namespace lvd

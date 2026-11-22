#include "schematic_editor/SchematicExport.h"

#include "core/Document.h"
#include "core/JsonFile.h"
#include "core/Project.h"
#include "core/Schematic.h"
#include "schematic_editor/SchematicScene.h"
#include "widgets/SchematicStyle.h"

#include <QDate>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QImage>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPrinter>
#include <QSvgGenerator>

#include <algorithm>
#include <memory>

namespace lvd {

namespace {

constexpr double MarginMm = 10;
constexpr double TitleWidthMm = 95;
constexpr double LineMm = 5.2;

std::unique_ptr<SchematicScene> exportScene(Project *project, Document *document)
{
    auto scene = std::make_unique<SchematicScene>(project, document);
    SchematicStyle style = SchematicStyle::light();
    style.background = Qt::white; // paper
    scene->setStyle(style);
    scene->setGridVisible(false);
    scene->connectivity(); // junctions and unconnected-pin markers up to date
    // Items cached as screen bitmaps would print blurred: draw everything as vectors.
    for (QGraphicsItem *item : scene->items())
        item->setCacheMode(QGraphicsItem::NoCache);
    return scene;
}

QRectF drawingBounds(SchematicScene &scene)
{
    QRectF bounds = scene.contentBounds();
    if (bounds.isEmpty())
        bounds = QRectF(0, 0, 100, 100);
    return bounds.adjusted(-20, -20, 20, 20);
}

} // namespace

QStringList SchematicExport::titleBlockLines(Project *project, Document *document)
{
    const QJsonObject fields = document && document->schematic() ? document->schematic()->titleBlock() : QJsonObject();
    const QString documentName = document ? document->name() : QString();
    const QString title = fields.value(QStringLiteral("title")).toString(documentName);
    QStringList lines;
    lines << title;
    lines << QStringLiteral("Project: %1    Document: %2").arg(project ? project->name() : QString(), documentName);
    QString revision = QStringLiteral("Date: %1").arg(QDate::currentDate().toString(Qt::ISODate));
    if (fields.contains(QStringLiteral("revision")))
        revision = QStringLiteral("Revision: %1    %2").arg(fields.value(QStringLiteral("revision")).toString(), revision);
    lines << revision;
    QStringList who;
    if (fields.contains(QStringLiteral("author")))
        who << QStringLiteral("Drawn by: %1").arg(fields.value(QStringLiteral("author")).toString());
    if (fields.contains(QStringLiteral("company")))
        who << fields.value(QStringLiteral("company")).toString();
    if (!who.isEmpty())
        lines << who.join(QStringLiteral("    "));
    lines << QStringLiteral("lvd RF cad %1    Sheet 1 of 1").arg(QStringLiteral(LVD_VERSION));
    return lines;
}

void SchematicExport::paintPage(QPainter &painter, const QRectF &page, double dotsPerMm, SchematicScene &scene,
                                const QStringList &titleLines)
{
    const double margin = MarginMm * dotsPerMm;
    const QRectF frame = page.adjusted(margin, margin, -margin, -margin);
    const double titleHeight = (titleLines.size() * LineMm + 2) * dotsPerMm;
    const double titleWidth = std::min(TitleWidthMm * dotsPerMm, frame.width());
    const QRectF titleBox(frame.right() - titleWidth, frame.bottom() - titleHeight, titleWidth, titleHeight);

    painter.save();
    painter.fillRect(page, Qt::white);
    QPen pen(Qt::black, 0.35 * dotsPerMm);
    painter.setPen(pen);
    painter.drawRect(frame);

    // The drawing above the title block, fitted and centred.
    const QRectF area = frame.adjusted(3 * dotsPerMm, 3 * dotsPerMm, -3 * dotsPerMm, -(titleHeight + 3 * dotsPerMm));
    const QRectF source = drawingBounds(scene);
    const double scale = std::min(area.width() / source.width(), area.height() / source.height());
    const QSizeF size = source.size() * scale;
    const QRectF target(area.center().x() - size.width() / 2, area.center().y() - size.height() / 2, size.width(),
                        size.height());
    scene.render(&painter, target, source, Qt::KeepAspectRatio);

    // Title block.
    painter.setPen(pen);
    painter.setBrush(Qt::white);
    painter.drawRect(titleBox);
    for (int i = 1; i < titleLines.size(); ++i) {
        const double y = titleBox.top() + (1 + i * LineMm) * dotsPerMm;
        painter.drawLine(QPointF(titleBox.left(), y), QPointF(titleBox.right(), y));
    }
    QFont font(QStringLiteral("Segoe UI"));
    for (int i = 0; i < titleLines.size(); ++i) {
        font.setPixelSize(int((i == 0 ? 3.6 : 2.7) * dotsPerMm));
        font.setBold(i == 0);
        painter.setFont(font);
        const double top = titleBox.top() + (i == 0 ? 0 : 1 + i * LineMm) * dotsPerMm;
        const double height = (i == 0 ? 1 + LineMm : LineMm) * dotsPerMm;
        painter.drawText(QRectF(titleBox.left() + 2 * dotsPerMm, top, titleBox.width() - 4 * dotsPerMm, height),
                         Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, titleLines[i]);
    }
    painter.restore();
}

bool SchematicExport::exportFile(Project *project, Document *document, const QString &path, QString *error)
{
    if (!document || !document->schematic()) {
        setError(error, QStringLiteral("only schematics can be exported"));
        return false;
    }
    auto scene = exportScene(project, document);
    const QRectF bounds = drawingBounds(*scene);
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("pdf")) {
        QPdfWriter writer(path);
        writer.setTitle(document->name());
        writer.setCreator(QStringLiteral("lvd RF cad %1").arg(QStringLiteral(LVD_VERSION)));
        writer.setResolution(600);
        writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4),
                                         bounds.width() >= bounds.height() ? QPageLayout::Landscape : QPageLayout::Portrait,
                                         QMarginsF(0, 0, 0, 0)));
        QPainter painter;
        if (!painter.begin(&writer)) {
            setError(error, QStringLiteral("cannot write %1").arg(path));
            return false;
        }
        const double dotsPerMm = writer.resolution() / 25.4;
        paintPage(painter, QRectF(0, 0, writer.width(), writer.height()), dotsPerMm, *scene, titleBlockLines(project, document));
        painter.end();
        return true;
    }
    if (suffix == QLatin1String("svg")) {
        QSvgGenerator generator;
        generator.setFileName(path);
        generator.setTitle(document->name());
        generator.setDescription(QStringLiteral("lvd RF cad schematic"));
        generator.setSize(bounds.size().toSize());
        generator.setViewBox(QRectF(QPointF(0, 0), bounds.size()));
        QPainter painter;
        if (!painter.begin(&generator)) {
            setError(error, QStringLiteral("cannot write %1").arg(path));
            return false;
        }
        painter.fillRect(QRectF(QPointF(0, 0), bounds.size()), Qt::white);
        scene->render(&painter, QRectF(QPointF(0, 0), bounds.size()), bounds);
        painter.end();
        return true;
    }
    if (suffix == QLatin1String("png")) {
        const double scale = 200.0 / 96.0;
        QImage image((bounds.size() * scale).toSize(), QImage::Format_ARGB32_Premultiplied);
        image.setDotsPerMeterX(int(200 / 0.0254));
        image.setDotsPerMeterY(int(200 / 0.0254));
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        scene->render(&painter, QRectF(QPointF(0, 0), image.size()), bounds);
        painter.end();
        if (!image.save(path)) {
            setError(error, QStringLiteral("cannot write %1").arg(path));
            return false;
        }
        return true;
    }
    setError(error, QStringLiteral("unknown export format \"%1\"; use .pdf, .svg or .png").arg(suffix));
    return false;
}

bool SchematicExport::landscape(Project *project, Document *document)
{
    if (!document || !document->schematic())
        return true;
    auto scene = exportScene(project, document);
    const QRectF bounds = drawingBounds(*scene);
    return bounds.width() >= bounds.height();
}

bool SchematicExport::print(Project *project, Document *document, QPrinter *printer, QString *error)
{
    if (!document || !document->schematic() || !printer) {
        setError(error, QStringLiteral("only schematics can be printed"));
        return false;
    }
    auto scene = exportScene(project, document);
    QPainter painter;
    if (!painter.begin(printer)) {
        setError(error, QStringLiteral("cannot start printing"));
        return false;
    }
    const QRectF page = printer->pageLayout().fullRectPixels(printer->resolution());
    const QRectF paint = printer->pageLayout().paintRectPixels(printer->resolution());
    // The frame keeps its margin from the paper edge; the device origin is at
    // the printable area's corner.
    const QRectF pageInDevice = page.translated(-paint.topLeft());
    paintPage(painter, pageInDevice, printer->resolution() / 25.4, *scene, titleBlockLines(project, document));
    painter.end();
    return true;
}

} // namespace lvd

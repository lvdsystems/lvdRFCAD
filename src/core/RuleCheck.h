#pragma once

#include <QList>
#include <QString>

namespace lvd {

class Document;
class Project;

struct RuleViolation {
    enum class Severity { Error, Warning, Info };

    Severity severity = Severity::Warning;
    QString document;
    QString itemId; // the item to highlight; may be empty
    QString message;
};

QString severityName(RuleViolation::Severity severity);

// Electrical rule check of one schematic document and the sheets below it
// (requirement PL-11). Sorted by severity, then document, then item.
QList<RuleViolation> checkSchematic(const Project &project, const Document &document);

} // namespace lvd

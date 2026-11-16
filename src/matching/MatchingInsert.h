#pragma once

#include "matching/MatchingSynthesis.h"

#include <QPoint>
#include <QString>
#include <QStringList>

#include <optional>

namespace lvd {

class Document;
class Project;

struct MatchInsertOptions {
    QString inputLabel = QStringLiteral("MATCH_IN");   // made unique with a suffix if taken
    QString outputLabel = QStringLiteral("MATCH_OUT");
    int significantDigits = 10; // of the parameter text; snapped values print exactly
};

struct InsertedMatch {
    QString inputLabel;  // the net label names used
    QString outputLabel;
    QPoint input;        // the labelled wire ends: connect the source here
    QPoint output;       // ... and the load here
    QStringList itemIds; // every item added
};

// Places a matching network into a schematic document as one undo step:
// basic:capacitor / basic:inductor, rf-circuit:tline / open_stub /
// short_stub, basic:ground under shunt lumped elements, wired as a ladder
// from left (source) to right (load) starting at `origin`, with a net label
// at each end. Returns nullopt with *error set if a library part cannot be
// resolved or the document is not a schematic; nothing is changed then.
std::optional<InsertedMatch> insertMatchingNetwork(Project &project, Document &document, const rf::MatchSolution &solution,
                                                   QPoint origin, QString *error = nullptr,
                                                   const MatchInsertOptions &options = {});

} // namespace lvd

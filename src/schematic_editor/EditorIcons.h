#pragma once

#include <QIcon>

class QPalette;

namespace lvd {

enum class EditorIcon {
    Undo, Redo, Cut, Copy, Paste, Duplicate, Delete, Rotate, Mirror, SelectAll,
    Select, Wire, Label, Text, Highlight, ZoomFit, RuleCheck, Reset, Browse, Add, Remove, Up, Down
};

// The theme icon when the platform has one, else a simple drawn icon in the
// palette's text colour.
QIcon editorIcon(EditorIcon icon, const QPalette &palette);

} // namespace lvd

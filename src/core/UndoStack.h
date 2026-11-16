#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <vector>

namespace lvd {

// Undo history. QtCore has no undo framework (QUndoStack is in QtGui), and the
// core must stay free of GUI modules.
class UndoStack : public QObject
{
    Q_OBJECT
public:
    struct Command {
        QString text;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    explicit UndoStack(QObject *parent = nullptr);

    // Records a change that has already been applied. Discards the redo history.
    void push(Command command);

    bool canUndo() const { return m_index > 0; }
    bool canRedo() const { return m_index < static_cast<int>(m_commands.size()); }
    QString undoText() const;
    QString redoText() const;
    void undo();
    void redo();

    int index() const { return m_index; }
    int count() const { return static_cast<int>(m_commands.size()); }

    bool isClean() const { return m_index == m_cleanIndex; }
    void setClean();
    void clear(); // also marks the empty stack clean

Q_SIGNALS:
    void changed(); // after push, undo, redo, setClean and clear
    void cleanChanged(bool clean);

private:
    void finish(bool wasClean);

    std::vector<Command> m_commands;
    int m_index = 0;
    int m_cleanIndex = 0; // -1 when the clean state can no longer be reached
    bool m_busy = false;
};

} // namespace lvd

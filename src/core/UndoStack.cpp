#include "core/UndoStack.h"

namespace lvd {

UndoStack::UndoStack(QObject *parent) : QObject(parent)
{
}

void UndoStack::push(Command command)
{
    if (m_busy)
        return; // changes made while undoing or redoing are part of that step
    const bool wasClean = isClean();
    m_commands.resize(static_cast<size_t>(m_index));
    if (m_cleanIndex > m_index)
        m_cleanIndex = -1;
    m_commands.push_back(std::move(command));
    ++m_index;
    finish(wasClean);
}

QString UndoStack::undoText() const
{
    return canUndo() ? m_commands[static_cast<size_t>(m_index - 1)].text : QString();
}

QString UndoStack::redoText() const
{
    return canRedo() ? m_commands[static_cast<size_t>(m_index)].text : QString();
}

void UndoStack::undo()
{
    if (!canUndo() || m_busy)
        return;
    const bool wasClean = isClean();
    m_busy = true;
    --m_index;
    m_commands[static_cast<size_t>(m_index)].undo();
    m_busy = false;
    finish(wasClean);
}

void UndoStack::redo()
{
    if (!canRedo() || m_busy)
        return;
    const bool wasClean = isClean();
    m_busy = true;
    m_commands[static_cast<size_t>(m_index)].redo();
    ++m_index;
    m_busy = false;
    finish(wasClean);
}

void UndoStack::setClean()
{
    const bool wasClean = isClean();
    m_cleanIndex = m_index;
    finish(wasClean);
}

void UndoStack::setCleanUnreachable()
{
    const bool wasClean = isClean();
    m_cleanIndex = -1;
    finish(wasClean);
}

void UndoStack::clear()
{
    const bool wasClean = isClean();
    m_commands.clear();
    m_index = 0;
    m_cleanIndex = 0;
    finish(wasClean);
}

void UndoStack::finish(bool wasClean)
{
    Q_EMIT changed();
    if (wasClean != isClean())
        Q_EMIT cleanChanged(isClean());
}

} // namespace lvd

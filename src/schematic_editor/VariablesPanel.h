#pragma once

#include <QWidget>

class QTableWidget;
class QToolButton;

namespace lvd {

class Schematic;

// The schematic's variables: name, expression and evaluated value or error.
// Edits go through Schematic::setVariables, so they are undoable.
class VariablesPanel : public QWidget
{
    Q_OBJECT
public:
    explicit VariablesPanel(Schematic *schematic, QWidget *parent = nullptr);

    QTableWidget *table() const { return m_table; }

    void addVariable();
    void removeSelected();
    void moveSelected(int offset);

protected:
    void changeEvent(QEvent *event) override;

private:
    void refresh();
    void applyTable();
    void updateButtons();

    Schematic *m_schematic;
    QTableWidget *m_table;
    QToolButton *m_add;
    QToolButton *m_remove;
    QToolButton *m_up;
    QToolButton *m_down;
    bool m_updating = false;
};

} // namespace lvd

#pragma once

#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace lvd {

class PythonInterpreter;

// Interactive Python prompt: output above, one input line below, with
// command history on the Up and Down keys.
class PythonConsole : public QWidget
{
    Q_OBJECT
public:
    explicit PythonConsole(PythonInterpreter *interpreter, QWidget *parent = nullptr);

    void execute(const QString &line);
    QString outputText() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Style { Output, Error, Echo };

    void append(const QString &text, Style style);
    void setContinuation(bool continuation);
    void showHistory(int index);

    PythonInterpreter *m_interpreter;
    QPlainTextEdit *m_output;
    QLabel *m_prompt;
    QLineEdit *m_input;
    QStringList m_history;
    int m_historyIndex = 0; // == m_history.size() when editing a new line
    QString m_pendingLine;
    bool m_continuation = false;
};

} // namespace lvd

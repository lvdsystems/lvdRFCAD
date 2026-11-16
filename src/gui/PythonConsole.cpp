#include "gui/PythonConsole.h"

#include "python/PythonInterpreter.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QVBoxLayout>

namespace lvd {

PythonConsole::PythonConsole(PythonInterpreter *interpreter, QWidget *parent)
    : QWidget(parent), m_interpreter(interpreter)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), font.family()});
    font.setStyleHint(QFont::Monospace);

    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setFont(font);
    m_output->setFrameShape(QFrame::NoFrame);
    m_output->setMaximumBlockCount(10000);

    m_prompt = new QLabel(this);
    m_prompt->setFont(font);

    m_input = new QLineEdit(this);
    m_input->setFont(font);
    m_input->setFrame(false);
    m_input->setPlaceholderText(tr("Python statement"));
    m_input->installEventFilter(this);

    auto *inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(4, 2, 4, 2);
    inputRow->addWidget(m_prompt);
    inputRow->addWidget(m_input);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_output);
    layout->addLayout(inputRow);

    setFocusProxy(m_input);
    setContinuation(false);

    connect(m_interpreter, &PythonInterpreter::output, this,
            [this](const QString &text, bool isError) { append(text, isError ? Style::Error : Style::Output); });
    connect(m_input, &QLineEdit::returnPressed, this, [this] {
        const QString line = m_input->text();
        m_input->clear();
        execute(line);
    });

    append(tr("Python %1. The lvdrfcad module is imported.\n").arg(m_interpreter->pythonVersion()), Style::Echo);
}

void PythonConsole::execute(const QString &line)
{
    append((m_continuation ? QStringLiteral("... ") : QStringLiteral(">>> ")) + line + QLatin1Char('\n'), Style::Echo);
    if (!line.trimmed().isEmpty() && (m_history.isEmpty() || m_history.last() != line))
        m_history.append(line);
    m_historyIndex = static_cast<int>(m_history.size());
    m_pendingLine.clear();
    setContinuation(m_interpreter->push(line));
}

QString PythonConsole::outputText() const
{
    return m_output->toPlainText();
}

bool PythonConsole::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Up) {
            showHistory(m_historyIndex - 1);
            return true;
        }
        if (key == Qt::Key_Down) {
            showHistory(m_historyIndex + 1);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PythonConsole::append(const QString &text, Style style)
{
    QTextCharFormat format;
    switch (style) {
    case Style::Output:
        format.setForeground(palette().color(QPalette::Text));
        break;
    case Style::Error:
        // Readable on both light and dark backgrounds.
        format.setForeground(QColor(0xD0, 0x45, 0x37));
        break;
    case Style::Echo:
        format.setForeground(palette().color(QPalette::PlaceholderText));
        break;
    }
    QTextCursor cursor(m_output->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text, format);
    m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
}

void PythonConsole::setContinuation(bool continuation)
{
    m_continuation = continuation;
    m_prompt->setText(continuation ? QStringLiteral("...") : QStringLiteral(">>>"));
}

void PythonConsole::showHistory(int index)
{
    const int count = static_cast<int>(m_history.size());
    if (index < 0 || index > count)
        return;
    if (m_historyIndex == count)
        m_pendingLine = m_input->text();
    m_historyIndex = index;
    m_input->setText(index == count ? m_pendingLine : m_history.at(index));
}

} // namespace lvd

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace lvd {

class Session;

// The embedded Python interpreter, with the "lvdrfcad" module bound to a
// Session. Only one instance may exist in a process. Python runs on the
// thread that created the interpreter.
class PythonInterpreter : public QObject
{
    Q_OBJECT
public:
    // Throws std::runtime_error if Python cannot be started.
    explicit PythonInterpreter(Session *session, QObject *parent = nullptr);
    ~PythonInterpreter() override;

    QString pythonVersion() const;

    // Feeds one line to the interactive console. Returns true when the
    // statement is incomplete and more lines are expected.
    bool push(const QString &line);

    // Runs a script file as __main__ with sys.argv = [path] + arguments.
    // Returns false if the script raised an exception or exited non-zero.
    bool runFile(const QString &path, const QStringList &arguments = {});

Q_SIGNALS:
    // Text written to sys.stdout (isError false) or sys.stderr (isError true).
    void output(const QString &text, bool isError);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace lvd

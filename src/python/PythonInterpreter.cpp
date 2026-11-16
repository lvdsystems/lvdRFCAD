#include "python/Bindings.h"

#include <pybind11/embed.h>

#include "python/PythonInterpreter.h"

#include "core/Session.h"
#include "spice/SpiceNetlister.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QThread>

#include <stdexcept>

namespace py = pybind11;

namespace lvd {

namespace {
QPointer<Session> g_session;
PythonInterpreter *g_interpreter = nullptr;
} // namespace

Session *pythonSession()
{
    return g_session;
}

void writePythonOutput(const QString &text, bool isError)
{
    if (g_interpreter)
        Q_EMIT g_interpreter->output(text, isError);
}

} // namespace lvd

// Defined in this file so the linker cannot drop the module registration.
PYBIND11_EMBEDDED_MODULE(lvdrfcad, module)
{
    lvd::registerBindings(module);
}

namespace lvd {

namespace {

// A bundled interpreter next to the executable wins over the one the
// application was built against. PYTHONHOME, if set, overrides both.
QString pythonHome()
{
    if (qEnvironmentVariableIsSet("PYTHONHOME"))
        return {};
    if (QCoreApplication::instance()) {
        const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("python"));
        if (QFileInfo(bundled).isDir())
            return bundled;
    }
    return QStringLiteral(LVD_PYTHON_HOME);
}

// Reports a Python exception on sys.stderr. Returns false unless the
// exception is a successful SystemExit.
bool reportException(py::error_already_set &error)
{
    if (error.matches(PyExc_SystemExit)) {
        const py::object code = error.value().attr("code");
        if (code.is_none() || (py::isinstance<py::int_>(code) && code.cast<int>() == 0))
            return true;
        if (!py::isinstance<py::int_>(code))
            writePythonOutput(QString::fromStdString(py::str(code)) + QLatin1Char('\n'), true);
        return false;
    }
    py::module_::import("traceback").attr("print_exception")(error.value());
    return false;
}

// Runs a Python-defined SPICE model (requirement SP-8): the model code
// defines spice(ref, nodes, params) returning netlist lines. The GUI thread
// holds the interpreter, so calls from worker threads (batches) are handed
// to it and waited for.
std::optional<QString> runPythonSpiceModel(const spice::PythonModelRequest &request, QString *error)
{
    const auto call = [&]() -> std::optional<QString> {
        py::gil_scoped_acquire gil;
        try {
            QString source = request.model.value(QStringLiteral("source")).toString();
            const QString file = request.model.value(QStringLiteral("file")).toString();
            if (source.trimmed().isEmpty() && !file.isEmpty()) {
                const QString path = QFileInfo(file).isRelative() ? QDir(request.projectDirectory).filePath(file) : file;
                QFile input(path);
                if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    *error = QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(path), input.errorString());
                    return std::nullopt;
                }
                source = QString::fromUtf8(input.readAll());
            }
            py::dict scope;
            scope["__builtins__"] = py::module_::import("builtins");
            scope["__name__"] = "lvd_spice_model";
            py::exec(source.toStdString(), scope);
            if (!scope.contains("spice")) {
                *error = QStringLiteral("the model code defines no function spice(ref, nodes, params)");
                return std::nullopt;
            }
            py::dict nodes, parameters;
            for (auto it = request.nodes.begin(); it != request.nodes.end(); ++it)
                nodes[py::str(it.key().toStdString())] = it.value().toStdString();
            for (auto it = request.parameters.begin(); it != request.parameters.end(); ++it) {
                if (it.value().typeId() == QMetaType::Double)
                    parameters[py::str(it.key().toStdString())] = it.value().toDouble();
                else
                    parameters[py::str(it.key().toStdString())] = it.value().toString().toStdString();
            }
            const py::object text = scope["spice"](request.reference.toStdString(), nodes, parameters);
            return QString::fromStdString(py::str(text));
        } catch (py::error_already_set &e) {
            *error = QString::fromStdString(e.what());
            return std::nullopt;
        }
    };
    QCoreApplication *application = QCoreApplication::instance();
    if (!application || QThread::currentThread() == application->thread())
        return call();
    std::optional<QString> result;
    QMetaObject::invokeMethod(application, [&] { result = call(); }, Qt::BlockingQueuedConnection);
    return result;
}

} // namespace

struct PythonInterpreter::Impl {
    // Declared first so it is destroyed last, after every Python object.
    std::unique_ptr<py::scoped_interpreter> interpreter;
    py::object console;
};

PythonInterpreter::PythonInterpreter(Session *session, QObject *parent) : QObject(parent), d(new Impl)
{
    if (g_interpreter)
        throw std::runtime_error("a Python interpreter already exists");

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;
    config.install_signal_handlers = 0;
    const QString home = pythonHome();
    if (!home.isEmpty()) {
        const PyStatus status =
            PyConfig_SetString(&config, &config.home, QDir::toNativeSeparators(home).toStdWString().c_str());
        if (PyStatus_Exception(status)) {
            PyConfig_Clear(&config);
            throw std::runtime_error("cannot set the Python home directory");
        }
    }
    // Takes ownership of config. Throws std::runtime_error on failure.
    d->interpreter = std::make_unique<py::scoped_interpreter>(&config, 0, nullptr, false);

    g_session = session;
    g_interpreter = this;
    spice::setPythonModelHandler(&runPythonSpiceModel);

    try {
        py::module_ api = py::module_::import("lvdrfcad");
        py::module_ sys = py::module_::import("sys");
        sys.attr("stdout") = api.attr("_OutputStream")(false);
        sys.attr("stderr") = api.attr("_OutputStream")(true);

        py::dict locals;
        locals["__name__"] = "__console__";
        locals["lvdrfcad"] = api;
        d->console = py::module_::import("code").attr("InteractiveConsole")(locals);
    } catch (const py::error_already_set &error) {
        const std::string message = error.what();
        d->console = py::object();
        d->interpreter.reset();
        g_interpreter = nullptr;
        g_session = nullptr;
        throw std::runtime_error("cannot set up Python: " + message);
    }
}

PythonInterpreter::~PythonInterpreter()
{
    spice::setPythonModelHandler({});
    d->console = py::object();
    d->interpreter.reset();
    g_interpreter = nullptr;
    g_session = nullptr;
}

QString PythonInterpreter::pythonVersion() const
{
    return QString::fromLatin1(Py_GetVersion()).section(QLatin1Char(' '), 0, 0);
}

bool PythonInterpreter::push(const QString &line)
{
    try {
        return d->console.attr("push")(line.toStdString()).cast<bool>();
    } catch (py::error_already_set &error) {
        // InteractiveConsole reports ordinary exceptions itself; only
        // SystemExit propagates. The application decides when to quit.
        if (error.matches(PyExc_SystemExit))
            writePythonOutput(QStringLiteral("exit() is ignored in the embedded console\n"), true);
        else
            reportException(error);
        d->console.attr("resetbuffer")();
        return false;
    }
}

bool PythonInterpreter::runFile(const QString &path, const QStringList &arguments)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        writePythonOutput(QStringLiteral("Cannot open %1: %2\n").arg(path, file.errorString()), true);
        return false;
    }
    const QByteArray source = file.readAll();
    const std::string nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdString();

    try {
        py::list argv;
        argv.append(nativePath);
        for (const QString &argument : arguments)
            argv.append(argument.toStdString());
        py::module_::import("sys").attr("argv") = argv;

        py::dict globals;
        globals["__name__"] = "__main__";
        globals["__file__"] = nativePath;
        globals["__builtins__"] = py::module_::import("builtins");

        const py::object code = py::module_::import("builtins")
                                    .attr("compile")(py::bytes(source.constData(), static_cast<size_t>(source.size())),
                                                     nativePath, "exec");
        py::module_::import("builtins").attr("exec")(code, globals);
        return true;
    } catch (py::error_already_set &error) {
        return reportException(error);
    }
}

} // namespace lvd

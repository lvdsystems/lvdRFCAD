#pragma once

#include <pybind11/pybind11.h>

#include <QString>

namespace lvd {

class Session;

// Fills the "lvdrfcad" Python module.
void registerBindings(pybind11::module_ &module);

// Implemented by PythonInterpreter.
Session *pythonSession();
void writePythonOutput(const QString &text, bool isError);

} // namespace lvd

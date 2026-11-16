#pragma once

#include <pybind11/pybind11.h>

namespace lvd {

class Document;

// Antenna classes of the "lvdrfcad" module (AntennaArray, AntennaElement,
// ArrayPattern) and lvdrfcad.antenna_libraries() / read_pattern().
void registerAntennaBindings(pybind11::module_ &module);

// The Python object of an antenna array or element document; None for
// other document types.
pybind11::object antennaArrayObject(Document *document);
pybind11::object antennaElementObject(Document *document);

} // namespace lvd

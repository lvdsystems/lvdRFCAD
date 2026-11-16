add_library(lvdpy STATIC
    AntennaBindings.cpp
    AntennaBindings.h
    Bindings.cpp
    Bindings.h
    PythonInterpreter.cpp
    PythonInterpreter.h
)
target_link_libraries(lvdpy PUBLIC lvdcore lvdspice PRIVATE lvdrfsim lvdantenna pybind11::embed)
target_compile_definitions(lvdpy PRIVATE LVD_PYTHON_HOME="${LVD_PYTHON_HOME}")

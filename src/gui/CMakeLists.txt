add_library(lvdgui STATIC
    AboutDialog.cpp
    AboutDialog.h
    DocumentView.cpp
    DocumentView.h
    MainWindow.cpp
    MainWindow.h
    NewProjectDialog.cpp
    NewProjectDialog.h
    PythonConsole.cpp
    PythonConsole.h
    SchematicWorkspace.cpp
    SchematicWorkspace.h
    ViolationsPanel.cpp
    ViolationsPanel.h
)
# Application icon and the LVD Systems logo (regenerate with tools/make_icon.py).
qt_add_resources(lvdgui "gui_resources" PREFIX "/lvd" BASE resources FILES
    resources/lvd-systems-logo.png
    resources/lvdrfcad-16.png resources/lvdrfcad-24.png resources/lvdrfcad-32.png resources/lvdrfcad-48.png
    resources/lvdrfcad-64.png resources/lvdrfcad-128.png resources/lvdrfcad-256.png)
target_compile_definitions(lvdgui PRIVATE LVD_SOURCE_TREE="${CMAKE_SOURCE_DIR}")
# Versions shown in Help > About (keep in step with the root CMakeLists.txt and cmake/*.cmake).
if(NOT LVD_NGSPICE_VERSION)
    set(LVD_NGSPICE_VERSION "-")
endif()
if(NOT LVD_OPENEMS_VERSION)
    set(LVD_OPENEMS_VERSION "-")
endif()
target_compile_definitions(lvdgui PRIVATE
    LVD_PYBIND11_VERSION="3.0.1" LVD_EIGEN_VERSION="3.4.0" LVD_HDF5_VERSION="2.2.0"
    LVD_NGSPICE_VERSION_TEXT="${LVD_NGSPICE_VERSION}" LVD_OPENEMS_VERSION_TEXT="${LVD_OPENEMS_VERSION}")
target_link_libraries(lvdgui PUBLIC lvdcore lvdpy lvdwidgets lvdschedit lvdlibui lvdsimui lvdspiceui lvdspui lvdmatchui lvdantui Qt6::Widgets)

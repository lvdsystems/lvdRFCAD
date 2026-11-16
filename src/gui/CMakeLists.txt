add_library(lvdgui STATIC
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
target_compile_definitions(lvdgui PRIVATE LVD_SOURCE_TREE="${CMAKE_SOURCE_DIR}")
target_link_libraries(lvdgui PUBLIC lvdcore lvdpy lvdwidgets lvdschedit lvdlibui lvdsimui lvdspiceui lvdspui lvdmatchui lvdantui Qt6::Widgets)

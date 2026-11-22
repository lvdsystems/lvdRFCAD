add_library(lvdcore STATIC
    Autosave.cpp
    Autosave.h
    ComponentDefinition.cpp
    ComponentDefinition.h
    Connectivity.cpp
    Connectivity.h
    Document.cpp
    Document.h
    DocumentType.cpp
    DocumentType.h
    Expression.cpp
    Expression.h
    JsonFile.cpp
    JsonFile.h
    Library.cpp
    Library.h
    Netlist.cpp
    Netlist.h
    Project.cpp
    Project.h
    RuleCheck.cpp
    RuleCheck.h
    Schematic.cpp
    Schematic.h
    Session.cpp
    Session.h
    StandardValues.cpp
    StandardValues.h
    Substrates.cpp
    Substrates.h
    UndoStack.cpp
    UndoStack.h
)
target_include_directories(lvdcore PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lvdcore PUBLIC Qt6::Core)
target_compile_definitions(lvdcore
    PUBLIC LVD_VERSION="${PROJECT_VERSION}"
    PRIVATE LVD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

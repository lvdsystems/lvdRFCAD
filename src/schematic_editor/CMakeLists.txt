# Schematic editor for RF and SPICE documents (requirement PL-3).
add_library(lvdschedit STATIC
    EditorIcons.cpp
    EditorIcons.h
    EditorSupport.cpp
    EditorSupport.h
    PropertiesPanel.cpp
    PropertiesPanel.h
    SchematicEditor.cpp
    SchematicEditor.h
    SchematicExport.cpp
    SchematicExport.h
    SchematicItems.cpp
    SchematicItems.h
    SchematicScene.cpp
    SchematicScene.h
    SchematicView.cpp
    SchematicView.h
    VariablesPanel.cpp
    VariablesPanel.h
)
find_package(Qt6 REQUIRED COMPONENTS Svg PrintSupport)
target_link_libraries(lvdschedit PUBLIC lvdwidgets Qt6::PrintSupport PRIVATE Qt6::Svg)

if(LVD_BUILD_TESTS)
    add_executable(schematic_editor_tests tests/schematic_editor_tests.cpp)
    target_link_libraries(schematic_editor_tests PRIVATE lvdschedit Qt6::Test Catch2::Catch2)
    target_compile_definitions(schematic_editor_tests PRIVATE LVD_SOURCE_TREE="${CMAKE_SOURCE_DIR}")
    add_test(NAME schematic_editor COMMAND schematic_editor_tests)
    set_tests_properties(schematic_editor PROPERTIES
        ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
        TIMEOUT 300)

    # Renders a demo schematic to PNG files on the native platform, for
    # visual checks: schedit_demo <output folder>.
    add_executable(schedit_demo tests/schedit_demo.cpp)
    target_link_libraries(schedit_demo PRIVATE lvdschedit)
endif()

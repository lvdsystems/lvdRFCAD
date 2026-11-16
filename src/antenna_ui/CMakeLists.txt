# Antenna design user interface: array and element workspaces, the antenna
# library panel (requirements AN-1 to AN-6).
add_library(lvdantui STATIC
    AntennaLibraryPanel.cpp
    AntennaLibraryPanel.h
    AntennaUiCommon.cpp
    AntennaUiCommon.h
    ArrayWorkspace.cpp
    ArrayWorkspace.h
    ElementWorkspace.cpp
    ElementWorkspace.h
    PositionEditor.cpp
    PositionEditor.h
)
target_include_directories(lvdantui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lvdantui PUBLIC lvdantenna lvdplot lvdplot3d Qt6::Widgets)

if(LVD_BUILD_TESTS)
    add_executable(antenna_ui_tests tests/antenna_ui_tests.cpp)
    target_link_libraries(antenna_ui_tests PRIVATE lvdantui Qt6::Test Catch2::Catch2)
    target_compile_definitions(antenna_ui_tests PRIVATE LVD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME antenna_ui COMMAND antenna_ui_tests)
    set_tests_properties(antenna_ui PROPERTIES
        ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
        TIMEOUT 300)
endif()

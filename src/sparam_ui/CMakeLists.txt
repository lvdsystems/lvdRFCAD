# S-parameter file editor (requirement RF-4): summary, plots, table and
# undoable network operations on Touchstone data.
add_library(lvdspui STATIC
    SParamDialogs.cpp
    SParamDialogs.h
    SParamFormat.cpp
    SParamFormat.h
    SParamTableModel.cpp
    SParamTableModel.h
    TouchstoneEditor.cpp
    TouchstoneEditor.h
)
target_include_directories(lvdspui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lvdspui PUBLIC lvdrfdata lvdplot Qt6::Widgets)

if(LVD_BUILD_TESTS)
    add_executable(sparam_ui_tests tests/sparam_ui_tests.cpp)
    target_link_libraries(sparam_ui_tests PRIVATE lvdspui Qt6::Test Catch2::Catch2)
    target_compile_definitions(sparam_ui_tests PRIVATE
        LVD_RFDATA_TEST_DATA="${CMAKE_SOURCE_DIR}/src/rfdata/tests/data")
    add_test(NAME sparam_ui COMMAND sparam_ui_tests)
    set_tests_properties(sparam_ui PROPERTIES
        ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
        TIMEOUT 300)

    # Not a test: opens the editor on the native platform, saves screenshots
    # and quits. Usage: sparam_ui_demo <output folder> [light|dark]
    add_executable(sparam_ui_demo tests/sparam_ui_demo.cpp)
    target_link_libraries(sparam_ui_demo PRIVATE lvdspui)
    target_compile_definitions(sparam_ui_demo PRIVATE
        LVD_RFDATA_TEST_DATA="${CMAKE_SOURCE_DIR}/src/rfdata/tests/data")
endif()

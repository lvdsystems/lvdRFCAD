# Library user interface: browser, symbol editor and library manager dialog.
add_library(lvdlibui STATIC
    LibraryBrowser.cpp
    LibraryBrowser.h
    LibraryManagerDialog.cpp
    LibraryManagerDialog.h
    LibraryTargetDialog.cpp
    LibraryTargetDialog.h
    LibraryUiCommon.cpp
    LibraryUiCommon.h
    SymbolCanvas.cpp
    SymbolCanvas.h
    SymbolEditor.cpp
    SymbolEditor.h
    SymbolPreview.cpp
    SymbolPreview.h
)
target_link_libraries(lvdlibui PUBLIC lvdwidgets Qt6::Widgets)

if(LVD_BUILD_TESTS)
    add_executable(library_ui_tests tests/library_ui_tests.cpp)
    target_link_libraries(library_ui_tests PRIVATE lvdlibui Qt6::Test Catch2::Catch2)
    add_test(NAME library_ui COMMAND library_ui_tests)
    set_tests_properties(library_ui PROPERTIES
        ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
        TIMEOUT 120)

    # Not a test: opens the windows on the native platform, saves screenshots
    # and quits. Usage: library_ui_demo <output folder> [light|dark]
    add_executable(library_ui_demo tests/library_ui_demo.cpp)
    target_link_libraries(library_ui_demo PRIVATE lvdlibui)
endif()

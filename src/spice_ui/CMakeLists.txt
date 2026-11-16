# SPICE user interface (requirements SP-2 to SP-6, SP-10).
#
# lvdwaveui: the waveform viewer (panes of traces given as expressions,
# cursors, measurements, FFT), independent of documents and engines.
# lvdspiceui: the SPICE simulation view of a schematic: analysis setup,
# background runs, result tables, netlist view and schematic annotation.
set(_waveui_sources
    MeasurementPanel.cpp
    MeasurementPanel.h
    WaveformPane.cpp
    WaveformPane.h
    WaveformViewer.cpp
    WaveformViewer.h
)
set(_waveui_existing)
foreach(_file IN LISTS _waveui_sources)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_file})
        list(APPEND _waveui_existing ${_file})
    endif()
endforeach()
if(_waveui_existing)
    add_library(lvdwaveui STATIC ${_waveui_existing})
    target_include_directories(lvdwaveui PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(lvdwaveui PUBLIC lvdspice lvdwaveform lvdplot Qt6::Widgets)
    if(LVD_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/waveform_viewer_tests.cpp)
        add_executable(waveform_viewer_tests tests/waveform_viewer_tests.cpp)
        target_link_libraries(waveform_viewer_tests PRIVATE lvdwaveui Qt6::Test Catch2::Catch2)
        add_test(NAME waveform_viewer COMMAND waveform_viewer_tests)
        set_tests_properties(waveform_viewer PROPERTIES
            ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
            TIMEOUT 300)
    endif()
    if(LVD_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/waveform_viewer_demo.cpp)
        # Not a test: shows the viewer with synthetic results and saves screenshots.
        add_executable(waveform_viewer_demo tests/waveform_viewer_demo.cpp)
        target_link_libraries(waveform_viewer_demo PRIVATE lvdwaveui)
    endif()
endif()

set(_spiceui_sources
    BatchDialog.cpp
    BatchDialog.h
    BatchView.cpp
    BatchView.h
    InstrumentsView.cpp
    InstrumentsView.h
    ModelImportDialog.cpp
    ModelImportDialog.h
    OperatingPointLabels.cpp
    OperatingPointLabels.h
    SpiceAnalysisDialog.cpp
    SpiceAnalysisDialog.h
    SpiceRunner.cpp
    SpiceRunner.h
    SpiceSimulationView.cpp
    SpiceSimulationView.h
)
set(_spiceui_existing)
foreach(_file IN LISTS _spiceui_sources)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_file})
        list(APPEND _spiceui_existing ${_file})
    endif()
endforeach()
if(_spiceui_existing AND TARGET lvdwaveui)
    add_library(lvdspiceui STATIC ${_spiceui_existing})
    target_include_directories(lvdspiceui PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(lvdspiceui PUBLIC lvdwaveui lvdspice lvdwidgets Qt6::Widgets)
    if(LVD_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/spice_ui_tests.cpp)
        add_executable(spice_ui_tests tests/spice_ui_tests.cpp)
        target_link_libraries(spice_ui_tests PRIVATE lvdspiceui Qt6::Test Catch2::Catch2)
        target_compile_definitions(spice_ui_tests PRIVATE LVD_NGSPICE_RUNTIME="${LVD_NGSPICE_RUNTIME_DIR}")
        add_test(NAME spice_ui COMMAND spice_ui_tests)
        set_tests_properties(spice_ui PROPERTIES
            ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}"
            ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
            TIMEOUT 300)
    endif()
endif()

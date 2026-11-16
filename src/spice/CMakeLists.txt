# SPICE simulation (requirements SP-1 to SP-10).
#
# lvdwaveform: waveform mathematics used by the viewer and the instruments
# (trace expressions, measurements, Fourier and FFT). Plain vectors, no engine.
# lvdspice: the engine interface, the ngspice shared-library driver, the
# netlister and analysis setups.
set(_waveform_sources
    Fourier.cpp
    Fourier.h
    VectorExpression.cpp
    VectorExpression.h
    WaveformMath.cpp
    WaveformMath.h
)
set(_waveform_existing)
foreach(_file IN LISTS _waveform_sources)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_file})
        list(APPEND _waveform_existing ${_file})
    endif()
endforeach()
if(_waveform_existing)
    add_library(lvdwaveform STATIC ${_waveform_existing})
    target_link_libraries(lvdwaveform PUBLIC Qt6::Core)
    target_include_directories(lvdwaveform PUBLIC ${CMAKE_SOURCE_DIR}/src)
endif()

add_library(lvdspice STATIC
    NgspiceEngine.cpp
    NgspiceEngine.h
    RawFile.cpp
    RawFile.h
    SpiceAnalysis.cpp
    SpiceAnalysis.h
    SpiceBatch.cpp
    SpiceBatch.h
    SpiceData.cpp
    SpiceData.h
    SpiceEngine.cpp
    SpiceEngine.h
    SpiceImport.cpp
    SpiceImport.h
    SpiceNetlister.cpp
    SpiceNetlister.h
    SpiceProcessPool.cpp
    SpiceProcessPool.h
    SpiceWaveforms.cpp
    SpiceWaveforms.h
)
target_link_libraries(lvdspice PUBLIC lvdcore)
if(TARGET lvdwaveform)
    target_link_libraries(lvdspice PUBLIC lvdwaveform)
endif()

if(LVD_BUILD_TESTS)
    add_executable(spice_tests tests/spice_tests.cpp tests/netlist_tests.cpp tests/reference_tests.cpp tests/import_tests.cpp tests/batch_tests.cpp)
    target_link_libraries(spice_tests PRIVATE lvdspice Catch2::Catch2WithMain)
    target_compile_definitions(spice_tests PRIVATE
        LVD_NGSPICE_CONSOLE="${LVD_NGSPICE_CONSOLE}"
        LVD_NGSPICE_RUNTIME="${LVD_NGSPICE_RUNTIME_DIR}"
        LVD_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        LVD_CLI="$<TARGET_FILE:lvdrfcad-cli>")
    add_dependencies(spice_tests lvdrfcad-cli)
    add_test(NAME spice COMMAND spice_tests)
    set_tests_properties(spice PROPERTIES ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}")

    if(TARGET lvdwaveform AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/waveform_tests.cpp)
        add_executable(waveform_tests tests/waveform_tests.cpp)
        target_link_libraries(waveform_tests PRIVATE lvdwaveform Catch2::Catch2WithMain)
        add_test(NAME waveform COMMAND waveform_tests)
        set_tests_properties(waveform PROPERTIES ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}")
    endif()
endif()

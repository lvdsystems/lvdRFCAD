# Full-wave simulation with openEMS (requirements AN-7 to AN-10, section 10):
# result readers, model and mesher, openEMS XML, process control, templates.
add_library(lvdfullwave STATIC
    Hdf5File.cpp
    Hdf5File.h
    FwElement.cpp
    FwElement.h
    FwModel.cpp
    FwModel.h
    Mesher.cpp
    Mesher.h
    OpenEmsXml.cpp
    OpenEmsXml.h
    Simulation.cpp
    Simulation.h
    SimulationResults.cpp
    SimulationResults.h
    StlFile.cpp
    StlFile.h
    Templates.cpp
    Templates.h
)
target_link_libraries(lvdfullwave PUBLIC lvdantenna Qt6::Gui PRIVATE lvd_hdf5)

if(LVD_BUILD_TESTS)
    add_executable(fullwave_tests tests/fullwave_tests.cpp)
    target_link_libraries(fullwave_tests PRIVATE lvdfullwave Catch2::Catch2WithMain)
    target_compile_definitions(fullwave_tests PRIVATE LVD_FULLWAVE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME fullwave COMMAND fullwave_tests "~[openems]")
    set_tests_properties(fullwave PROPERTIES ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}")
    if(LVD_OPENEMS_RUNTIME_DIR)
        # Runs the real solver (about a minute): the probe-fed patch against the
        # openEMS Simple Patch Antenna tutorial (the phase 8 exit criterion).
        add_test(NAME fullwave_openems COMMAND fullwave_tests "[openems]~[templates]")
        set_tests_properties(fullwave_openems PROPERTIES
            ENVIRONMENT "LVD_OPENEMS_DIR=${LVD_OPENEMS_RUNTIME_DIR};LVD_FULLWAVE_WORK=${CMAKE_BINARY_DIR}/fullwave_test"
            ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
            TIMEOUT 900)
        # Every template at its defaults, the horn and helix against their
        # openEMS tutorials: about 15 minutes and 4 GB of temporary disk.
        # Label "long": the default test preset leaves it out; run it with
        # ctest --preset windows-all.
        add_test(NAME fullwave_templates COMMAND fullwave_tests "[templates]")
        set_tests_properties(fullwave_templates PROPERTIES
            ENVIRONMENT "LVD_OPENEMS_DIR=${LVD_OPENEMS_RUNTIME_DIR};LVD_FULLWAVE_WORK=${CMAKE_BINARY_DIR}/fullwave_templates"
            ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
            LABELS long
            TIMEOUT 3600)
    endif()
endif()

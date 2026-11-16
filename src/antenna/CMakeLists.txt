# Antenna design (requirements AN-1 to AN-6): element models, array
# geometry, excitation and AESA effects, far-field computation, synthesis,
# the T/R budget and antenna libraries. No GUI.
add_library(lvdantenna STATIC
    AntennaLibrary.cpp
    AntennaLibrary.h
    AntennaTypes.h
    ArrayAnalysis.cpp
    ArrayAnalysis.h
    ArrayDesign.cpp
    ArrayDesign.h
    ArrayGeometry.cpp
    ArrayGeometry.h
    ElementModel.cpp
    ElementModel.h
    Excitation.cpp
    Excitation.h
    PatternEngine.cpp
    PatternEngine.h
    PatternFile.cpp
    PatternFile.h
    Synthesis.cpp
    Synthesis.h
)
target_link_libraries(lvdantenna PUBLIC lvdcore lvdrfsim lvd_eigen)

if(LVD_BUILD_TESTS)
    add_executable(antenna_tests tests/antenna_tests.cpp tests/array_tests.cpp)
    target_link_libraries(antenna_tests PRIVATE lvdantenna Catch2::Catch2WithMain)
    target_compile_definitions(antenna_tests PRIVATE LVD_ANTENNA_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME antenna COMMAND antenna_tests)
    set_tests_properties(antenna PROPERTIES ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}")
endif()

# RF simulation: element models, the linear circuit solver with noise,
# analyses and measurements.
add_library(lvdrfsim STATIC
    Analysis.cpp
    Analysis.h
    Budget.cpp
    Budget.h
    ElementModels.cpp
    ElementModels.h
    LinearSolver.cpp
    LinearSolver.h
    Measurement.cpp
    Measurement.h
    Optimizer.cpp
    Optimizer.h
    SignalChain.cpp
    SignalChain.h
    Spurs.cpp
    Spurs.h
)
target_link_libraries(lvdrfsim PUBLIC lvdcore lvdrfdata)

if(LVD_BUILD_TESTS)
    # Spur tests live in their own file, owned by the spur analysis.
    set(_rfsim_test_sources tests/rfsim_tests.cpp tests/budget_tests.cpp tests/optimizer_tests.cpp)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/spur_tests.cpp)
        list(APPEND _rfsim_test_sources tests/spur_tests.cpp)
    endif()
    add_executable(rfsim_tests ${_rfsim_test_sources})
    target_link_libraries(rfsim_tests PRIVATE lvdrfsim Catch2::Catch2WithMain)
    target_compile_definitions(rfsim_tests PRIVATE LVD_RF_TEST_DATA="${CMAKE_SOURCE_DIR}/src/rfdata/tests/data")
    add_test(NAME rfsim COMMAND rfsim_tests)
    set_tests_properties(rfsim PROPERTIES ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}")
endif()

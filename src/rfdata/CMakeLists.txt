# RF data: network types and conversions, Touchstone files, network
# operations and physical line models. Depends on QtCore and Eigen only.
add_library(lvdrfdata STATIC
    LineModels.cpp
    LineModels.h
    NetworkMath.cpp
    NetworkMath.h
    NetworkOps.cpp
    NetworkOps.h
    RfTypes.h
    Touchstone.cpp
    Touchstone.h
)
target_include_directories(lvdrfdata PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lvdrfdata PUBLIC Qt6::Core lvd_eigen)
if(MSVC)
    # Eigen's complex matrix products need large object files.
    target_compile_options(lvdrfdata PUBLIC /bigobj)
endif()

if(LVD_BUILD_TESTS)
    # Reference data in tests/data is generated with scikit-rf by
    # tools/generate_rf_reference.py.
    add_executable(rfdata_tests
        tests/LineModelsTests.cpp
        tests/NetworkMathTests.cpp
        tests/NetworkOpsTests.cpp
        tests/ReferenceData.cpp
        tests/ReferenceData.h
        tests/TouchstoneTests.cpp
    )
    target_link_libraries(rfdata_tests PRIVATE lvdrfdata Catch2::Catch2WithMain)
    target_compile_definitions(rfdata_tests PRIVATE
        LVD_RFDATA_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME rfdata COMMAND rfdata_tests)
    set_tests_properties(rfdata PROPERTIES
        ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}")
endif()

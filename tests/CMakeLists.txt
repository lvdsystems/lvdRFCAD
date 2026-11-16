add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE lvdcore Catch2::Catch2WithMain)
add_test(NAME core COMMAND core_tests)

add_executable(schematic_tests schematic_tests.cpp)
target_link_libraries(schematic_tests PRIVATE lvdcore Qt6::Test Catch2::Catch2WithMain)
add_test(NAME schematic COMMAND schematic_tests)

add_executable(expression_tests expression_tests.cpp)
target_link_libraries(expression_tests PRIVATE lvdcore Catch2::Catch2WithMain)
add_test(NAME expression COMMAND expression_tests)

add_executable(gui_tests gui_tests.cpp)
target_link_libraries(gui_tests PRIVATE lvdgui Qt6::Test Catch2::Catch2)
add_test(NAME gui COMMAND gui_tests)
set_tests_properties(gui PROPERTIES ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}" TIMEOUT 120)

add_test(NAME python_api
    COMMAND lvdrfcad-cli ${CMAKE_CURRENT_SOURCE_DIR}/scripts/python_api.py ${CMAKE_BINARY_DIR}/python_api_test)
add_test(NAME python_schematic
    COMMAND lvdrfcad-cli ${CMAKE_CURRENT_SOURCE_DIR}/scripts/python_schematic.py ${CMAKE_BINARY_DIR}/python_schematic_test)

add_test(NAME python_rf
    COMMAND lvdrfcad-cli ${CMAKE_CURRENT_SOURCE_DIR}/scripts/python_rf.py ${CMAKE_BINARY_DIR}/python_rf_test)

add_test(NAME python_budget
    COMMAND lvdrfcad-cli ${CMAKE_CURRENT_SOURCE_DIR}/scripts/python_budget.py ${CMAKE_BINARY_DIR}/python_budget_test)

add_test(NAME python_spice
    COMMAND lvdrfcad-cli ${CMAKE_CURRENT_SOURCE_DIR}/scripts/python_spice.py ${CMAKE_BINARY_DIR}/python_spice_test)

add_test(NAME python_antenna
    COMMAND lvdrfcad-cli ${CMAKE_CURRENT_SOURCE_DIR}/scripts/python_antenna.py ${CMAKE_BINARY_DIR}/python_antenna_test)

set_tests_properties(core schematic expression gui python_api python_schematic python_rf python_budget python_spice python_antenna
    PROPERTIES ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}")

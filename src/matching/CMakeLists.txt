# Matching-network synthesis (requirements RF-10 and RF-13): the synthesis
# core and schematic insertion (no GUI), and the matching tool window.
add_library(lvdmatching STATIC
    MatchingInsert.cpp
    MatchingInsert.h
    MatchingSynthesis.cpp
    MatchingSynthesis.h
)
target_include_directories(lvdmatching PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lvdmatching PUBLIC lvdrfsim)

add_library(lvdmatchui STATIC
    MatchLadderView.cpp
    MatchLadderView.h
    MatchingWindow.cpp
    MatchingWindow.h
)
target_link_libraries(lvdmatchui PUBLIC lvdmatching lvdplot lvdwidgets Qt6::Widgets)

if(LVD_BUILD_TESTS)
    add_executable(matching_tests tests/matching_tests.cpp)
    target_link_libraries(matching_tests PRIVATE lvdmatching Catch2::Catch2WithMain)
    add_test(NAME matching COMMAND matching_tests)
    set_tests_properties(matching PROPERTIES ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}")

    add_executable(matching_ui_tests tests/matching_ui_tests.cpp)
    target_link_libraries(matching_ui_tests PRIVATE lvdmatchui Qt6::Test Catch2::Catch2)
    add_test(NAME matching_ui COMMAND matching_ui_tests)
    set_tests_properties(matching_ui PROPERTIES
        ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
        TIMEOUT 120)

    # Not a test: opens the matching window on the native platform for
    # Zs = 50, ZL = 12 - j30 ohm at 2.4 GHz, saves screenshots and quits.
    # Usage: matching_demo <output folder> [light|dark]
    add_executable(matching_demo tests/matching_demo.cpp)
    target_link_libraries(matching_demo PRIVATE lvdmatchui)
endif()

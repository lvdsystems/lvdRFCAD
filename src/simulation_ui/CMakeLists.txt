# Simulation user interface: analysis setup, background runs, result plots,
# tables and export (requirements RF-5, PL-7, NF-3), and RF budget analyses
# (RF-6 to RF-9).
add_library(lvdsimui STATIC
    AnalysisDialog.cpp
    AnalysisDialog.h
    BudgetDialog.cpp
    BudgetDialog.h
    BudgetModels.cpp
    BudgetModels.h
    BudgetRunner.cpp
    BudgetRunner.h
    BudgetView.cpp
    BudgetView.h
    DesignVariables.cpp
    DesignVariables.h
    OptimizationDialog.cpp
    OptimizationDialog.h
    OptimizationRunner.cpp
    OptimizationRunner.h
    PlotDefinitions.cpp
    PlotDefinitions.h
    PlotPane.cpp
    PlotPane.h
    ResultsTable.cpp
    ResultsTable.h
    SimulationRunner.cpp
    SimulationRunner.h
    SimulationView.cpp
    SimulationView.h
    TuningPanel.cpp
    TuningPanel.h
    TuningRunner.cpp
    TuningRunner.h
)
target_include_directories(lvdsimui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lvdsimui PUBLIC lvdrfsim lvdplot Qt6::Widgets)

if(LVD_BUILD_TESTS)
    add_executable(simulation_ui_tests tests/simulation_ui_tests.cpp tests/budget_ui_tests.cpp tests/tuning_ui_tests.cpp)
    target_link_libraries(simulation_ui_tests PRIVATE lvdsimui Qt6::Test Catch2::Catch2)
    add_test(NAME simulation_ui COMMAND simulation_ui_tests)
    set_tests_properties(simulation_ui PROPERTIES
        ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
        TIMEOUT 300)

    # Not a test: simulates example circuits, shows the simulation view on the
    # native platform, saves screenshots and quits.
    # Usage: simui_demo <output folder> [light|dark]
    add_executable(simui_demo tests/simui_demo.cpp)
    target_link_libraries(simui_demo PRIVATE lvdsimui)

    # Not a test: builds a superheterodyne receiver, runs its budget on the
    # native platform, saves screenshots of the budget views and quits.
    # Usage: budgetui_demo <output folder> [light|dark]
    add_executable(budgetui_demo tests/budgetui_demo.cpp)
    target_link_libraries(budgetui_demo PRIVATE lvdsimui)

    # Not a test: tunes a low-pass filter and optimises an L-match on the
    # native platform, saves screenshots of the tuning panel and the
    # optimisation dialog, prints the tuning update rate and quits.
    # Usage: tune_demo <output folder> [light|dark]
    add_executable(tune_demo tests/tune_demo.cpp)
    target_link_libraries(tune_demo PRIVATE lvdsimui)
endif()

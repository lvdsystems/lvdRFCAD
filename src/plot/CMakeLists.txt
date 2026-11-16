# Plot widgets: rectangular, Smith chart, polar and colour-map plots drawn
# with QPainter.
add_library(lvdplot STATIC
    ColorMapPlot.cpp
    ColorMapPlot.h
    ColorMaps.cpp
    ColorMaps.h
    PlotAxis.cpp
    PlotAxis.h
    PlotMath.h
    PlotPaint.cpp
    PlotTheme.cpp
    PlotTheme.h
    PlotWidget.cpp
    PlotWidget.h
    PlotWidgetPrivate.h
)
target_include_directories(lvdplot PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lvdplot PUBLIC Qt6::Widgets)

# SVG export when Qt SVG is installed.
find_package(Qt6 QUIET COMPONENTS Svg)
if(TARGET Qt6::Svg)
    target_link_libraries(lvdplot PRIVATE Qt6::Svg)
    target_compile_definitions(lvdplot PRIVATE LVD_PLOT_HAVE_SVG)
endif()

if(LVD_BUILD_TESTS)
    add_executable(plot_tests tests/plot_tests.cpp)
    target_link_libraries(plot_tests PRIVATE lvdplot Qt6::Test Catch2::Catch2)
    add_test(NAME plot COMMAND plot_tests)
    set_tests_properties(plot PROPERTIES
        ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
        TIMEOUT 120)

    # Not a test: renders example RF plots on the native platform, saves PNGs
    # and quits. Usage: plot_demo <output folder> [light|dark]
    add_executable(plot_demo tests/plot_demo.cpp)
    target_link_libraries(plot_demo PRIVATE lvdplot)
endif()

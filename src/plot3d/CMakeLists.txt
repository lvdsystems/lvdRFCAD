# 3D view: antenna radiation patterns and geometry, drawn with OpenGL through
# QOpenGLWidget, with a QPainter fallback when OpenGL is unavailable.
find_package(Qt6 REQUIRED COMPONENTS OpenGL OpenGLWidgets)

add_library(lvdplot3d STATIC
    Pattern3DView.cpp
    Pattern3DView.h
    Scene3D.cpp
    Scene3D.h
    Scene3DGL.cpp
    Scene3DRender.h
    Scene3DSoftware.cpp
)
target_include_directories(lvdplot3d PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lvdplot3d PUBLIC lvdplot Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)

if(LVD_BUILD_TESTS)
    add_executable(plot3d_tests tests/plot3d_tests.cpp)
    target_link_libraries(plot3d_tests PRIVATE lvdplot3d Qt6::Test Catch2::Catch2)
    add_test(NAME plot3d COMMAND plot3d_tests)
    set_tests_properties(plot3d PROPERTIES
        ENVIRONMENT "${LVD_TEST_GUI_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${LVD_TEST_PATH_MODIFICATION}"
        TIMEOUT 120)

    # Not a test: renders an example pattern on the native platform through
    # OpenGL and the fallback, saves PNGs and quits.
    # Usage: plot3d_demo <output folder> [light|dark]
    add_executable(plot3d_demo tests/plot3d_demo.cpp)
    target_link_libraries(plot3d_demo PRIVATE lvdplot3d)
endif()

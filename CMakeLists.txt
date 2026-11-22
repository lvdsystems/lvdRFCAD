cmake_minimum_required(VERSION 3.24)
project(lvdRFcad VERSION 0.5.0 DESCRIPTION "RF, SPICE and antenna design CAD" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

# All executables in one folder so they share the deployed Qt runtime.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(LVD_BUILD_TESTS "Build the test suite" ON)
option(LVD_DEPLOY_QT "Copy the Qt runtime next to the application after building (Windows)" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Test)
find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development)

# Python.h declares a member called "slots", so Qt keywords are disabled
# project-wide: use Q_SIGNALS, Q_SLOTS and Q_EMIT.
add_compile_definitions(QT_NO_KEYWORDS)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

include(FetchContent)
set(PYBIND11_FINDPYTHON ON)
FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v3.0.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(pybind11)

# Eigen is header-only; only its include folder is used.
FetchContent_Declare(eigen
    URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
    URL_HASH SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(eigen)
add_library(lvd_eigen INTERFACE)
target_include_directories(lvd_eigen SYSTEM INTERFACE ${eigen_SOURCE_DIR})
target_compile_definitions(lvd_eigen INTERFACE EIGEN_MPL2_ONLY)

if(LVD_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
endif()

if(WIN32)
    include(cmake/Ngspice.cmake)
    include(cmake/OpenEMS.cmake)
endif()
include(cmake/Hdf5.cmake)

# Development builds use the Python installation found at configure time.
# An installed application looks for a bundled "python" folder first.
execute_process(
    COMMAND ${Python_EXECUTABLE} -c "import sys; print(sys.base_prefix)"
    OUTPUT_VARIABLE LVD_PYTHON_HOME
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
file(TO_CMAKE_PATH "${LVD_PYTHON_HOME}" LVD_PYTHON_HOME)

get_target_property(_lvd_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(LVD_QT_BIN_DIR "${_lvd_qmake}" DIRECTORY)
get_filename_component(LVD_QT_PLATFORM_PLUGINS "${LVD_QT_BIN_DIR}/../plugins/platforms" ABSOLUTE)

# Environment for tests: Qt and Python runtimes through PATH, and the
# offscreen platform plugin from the Qt installation (the deploy step copies
# only the "windows" plugin next to the binaries).
file(TO_NATIVE_PATH "${LVD_QT_BIN_DIR}" _qt_bin)
file(TO_NATIVE_PATH "${LVD_PYTHON_HOME}" _python_home)
set(LVD_TEST_PATH_MODIFICATION "PATH=path_list_prepend:${_qt_bin}" "PATH=path_list_prepend:${_python_home}")
set(LVD_TEST_GUI_ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${LVD_QT_PLATFORM_PLUGINS}")

add_subdirectory(src/core)
add_subdirectory(src/python)
add_subdirectory(src/rfdata)
add_subdirectory(src/rfsim)
add_subdirectory(src/spice)
add_subdirectory(src/widgets)
add_subdirectory(src/plot)
if(EXISTS ${CMAKE_SOURCE_DIR}/src/plot3d/CMakeLists.txt)
    add_subdirectory(src/plot3d)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/src/antenna/CMakeLists.txt)
    add_subdirectory(src/antenna)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/src/fullwave/CMakeLists.txt)
    add_subdirectory(src/fullwave)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/src/simulation_ui/CMakeLists.txt)
    add_subdirectory(src/simulation_ui)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/src/matching/CMakeLists.txt)
    add_subdirectory(src/matching)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/src/sparam_ui/CMakeLists.txt)
    add_subdirectory(src/sparam_ui)
endif()
add_subdirectory(src/spice_ui)
add_subdirectory(src/antenna_ui)
add_subdirectory(src/schematic_editor)
add_subdirectory(src/library_ui)
add_subdirectory(src/gui)
add_subdirectory(src/cli)
add_subdirectory(src/app)

if(LVD_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(WIN32)
    include(cmake/Packaging.cmake)
endif()

if(LVD_BUILD_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tools/screenshots/CMakeLists.txt)
    add_subdirectory(tools/screenshots)
endif()

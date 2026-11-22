# Installation layout and the portable ZIP package (CPack).
#
#   <prefix>/lvdrfcad.exe, lvdrfcad-cli.exe   the programs
#   <prefix>/*.dll, platforms/, styles/ ...   Qt runtime (windeployqt), MSVC runtime, python3xx.dll
#   <prefix>/python/                         embedded Python: DLLs/ and Lib/ (standard library)
#   <prefix>/libraries/                      built-in component and antenna libraries
#   <prefix>/ngspice/, openems/              the SPICE and full-wave solvers
#   <prefix>/examples/, docs/, LICENSE, README.md
#   <prefix>/THIRD-PARTY-NOTICES.md, licenses/   third-party licences
#
# Build the packages with:
#   cmake --build --preset windows --target portable    (ZIP)
#   cmake --build --preset windows --target installer   (setup.exe, needs Inno Setup)

include(GNUInstallDirs)

install(TARGETS lvdrfcad lvdrfcad-cli RUNTIME DESTINATION .)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/libraries/ DESTINATION libraries
    FILES_MATCHING PATTERN "*.lvdlib" PATTERN "*.lvdantlib" PATTERN "*.csv" PATTERN "substrates.json")
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md
        DESTINATION .)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/licenses/ DESTINATION licenses)
# ngspice shared library, its DLLs, XSPICE code models and start-up script
# (cmake/Ngspice.cmake); the application finds it in the "ngspice" folder.
if(LVD_NGSPICE_RUNTIME_DIR)
    install(DIRECTORY "${LVD_NGSPICE_RUNTIME_DIR}/" DESTINATION ngspice)
endif()
# openEMS and nf2ff (cmake/OpenEMS.cmake), run as separate processes.
if(LVD_OPENEMS_RUNTIME_DIR)
    install(DIRECTORY "${LVD_OPENEMS_RUNTIME_DIR}/" DESTINATION openems)
endif()
install(DIRECTORY ${CMAKE_SOURCE_DIR}/docs/manual/ DESTINATION docs/manual
    PATTERN "images/README.md" EXCLUDE)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/examples/ DESTINATION examples)
install(FILES
    ${CMAKE_SOURCE_DIR}/docs/file-format.md
    ${CMAKE_SOURCE_DIR}/docs/libraries.md
    ${CMAKE_SOURCE_DIR}/docs/python-api.md
    DESTINATION docs)

# MSVC runtime next to the programs (app-local deployment).
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
include(InstallRequiredSystemLibraries)

# Embedded Python: the DLLs next to the programs, the standard library in python/.
# Test suites, IDLE, Tk and pip bootstrap are left out to keep the package small.
get_filename_component(_python_dll_dir "${Python_EXECUTABLE}" DIRECTORY)
file(GLOB _python_dlls "${_python_dll_dir}/python3*.dll")
install(FILES ${_python_dlls} DESTINATION .)
install(DIRECTORY "${LVD_PYTHON_HOME}/DLLs/" DESTINATION python/DLLs
    PATTERN "_tkinter*" EXCLUDE
    PATTERN "tcl*" EXCLUDE
    PATTERN "tk*" EXCLUDE
    PATTERN "*_test*" EXCLUDE)
install(DIRECTORY "${LVD_PYTHON_HOME}/Lib/" DESTINATION python/Lib
    PATTERN "__pycache__" EXCLUDE
    PATTERN "site-packages" EXCLUDE
    PATTERN "test" EXCLUDE
    PATTERN "tests" EXCLUDE
    PATTERN "idlelib" EXCLUDE
    PATTERN "tkinter" EXCLUDE
    PATTERN "turtledemo" EXCLUDE
    PATTERN "ensurepip" EXCLUDE
    PATTERN "lib2to3" EXCLUDE)
install(FILES "${LVD_PYTHON_HOME}/LICENSE.txt" DESTINATION python RENAME PYTHON-LICENSE.txt)

# Qt runtime: windeployqt on the installed programs.
find_program(LVD_WINDEPLOYQT_INSTALL windeployqt HINTS "${LVD_QT_BIN_DIR}")
if(LVD_WINDEPLOYQT_INSTALL)
    install(CODE "
        execute_process(
            COMMAND \"${LVD_WINDEPLOYQT_INSTALL}\" --no-translations --no-compiler-runtime
                    --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
                    \"\${CMAKE_INSTALL_PREFIX}/lvdrfcad.exe\" \"\${CMAKE_INSTALL_PREFIX}/lvdrfcad-cli.exe\"
            RESULT_VARIABLE _deploy_result)
        if(NOT _deploy_result EQUAL 0)
            message(FATAL_ERROR \"windeployqt failed: \${_deploy_result}\")
        endif()
    ")
endif()

set(CPACK_PACKAGE_NAME "lvdRFcad")
set(CPACK_PACKAGE_VENDOR "lvd RF cad contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "RF, SPICE and antenna design CAD")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "lvdRFcad-${PROJECT_VERSION}-win64")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
include(CPack)

# Inno Setup script for a setup.exe; compile it with ISCC when Inno Setup is installed.
configure_file(${CMAKE_SOURCE_DIR}/packaging/lvdrfcad.iss.in ${CMAKE_BINARY_DIR}/package/lvdrfcad.iss @ONLY)

# Convenience targets (build them with --config RelWithDebInfo or Release):
#   portable   the ZIP package
#   installer  setup.exe: installs into package/stage, then compiles the Inno Setup script
add_custom_target(portable
    COMMAND ${CMAKE_CPACK_COMMAND} -C $<CONFIG> --config ${CMAKE_BINARY_DIR}/CPackConfig.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Building the portable ZIP package"
    VERBATIM)
add_dependencies(portable lvdrfcad lvdrfcad-cli)

find_program(LVD_ISCC ISCC
    HINTS "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6" "$ENV{ProgramFiles\(x86\)}/Inno Setup 6")
if(LVD_ISCC)
    add_custom_target(installer
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/package/stage
        COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --prefix ${CMAKE_BINARY_DIR}/package/stage
        COMMAND ${LVD_ISCC} /Q ${CMAKE_BINARY_DIR}/package/lvdrfcad.iss
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Building the setup.exe with Inno Setup"
        VERBATIM)
    add_dependencies(installer lvdrfcad lvdrfcad-cli)
else()
    message(STATUS "Inno Setup not found: the 'installer' target is not available")
endif()

# ngspice 47 for 64-bit Windows (requirement SP-1).
#
# Two packages of the same release are downloaded once at configure time:
# - the shared-library build: ngspice.dll, its OpenMP runtime and the XSPICE
#   code models;
# - the console build: sndfile.dll and samplerate.dll, which ngspice.dll
#   imports but the shared-library package does not contain, and
#   ngspice_con.exe for reference tests (never installed).
# They are copied next to the binaries as bin/ngspice/. The application loads
# the DLL at run time (QLibrary), so there is no link-time dependency.

set(LVD_NGSPICE_VERSION 47)

FetchContent_Declare(ngspice_dll
    URL https://sourceforge.net/projects/ngspice/files/ng-spice-rework/47/ngspice-47_dll_64.7z/download
    URL_HASH SHA256=5BF5EEC7DB989F74849CDB32F7DF62693F4F149CCEEEBCBD222EB35C2BB821EB
    DOWNLOAD_NAME ngspice-47_dll_64.7z
    SOURCE_SUBDIR do-not-configure)
FetchContent_Declare(ngspice_console
    URL https://sourceforge.net/projects/ngspice/files/ng-spice-rework/47/ngspice-47_64.7z/download
    URL_HASH SHA256=59225971BD68CDD1199443649AA4615A9E6D684933F205AB49006A3942518F5A
    DOWNLOAD_NAME ngspice-47_64.7z
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(ngspice_dll ngspice_console)

set(LVD_NGSPICE_PACKAGE "${ngspice_dll_SOURCE_DIR}")
if(EXISTS "${LVD_NGSPICE_PACKAGE}/Spice64_dll")
    set(LVD_NGSPICE_PACKAGE "${LVD_NGSPICE_PACKAGE}/Spice64_dll")
endif()
set(LVD_NGSPICE_CONSOLE_PACKAGE "${ngspice_console_SOURCE_DIR}")
if(EXISTS "${LVD_NGSPICE_CONSOLE_PACKAGE}/Spice64")
    set(LVD_NGSPICE_CONSOLE_PACKAGE "${LVD_NGSPICE_CONSOLE_PACKAGE}/Spice64")
endif()
foreach(_file "${LVD_NGSPICE_PACKAGE}/dll-vs/ngspice.dll" "${LVD_NGSPICE_CONSOLE_PACKAGE}/bin/sndfile.dll"
              "${LVD_NGSPICE_CONSOLE_PACKAGE}/bin/ngspice_con.exe")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "ngspice package has an unexpected layout: ${_file} is missing")
    endif()
endforeach()

# Runtime folder next to the executables.
set(LVD_NGSPICE_RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin/ngspice")
file(COPY "${LVD_NGSPICE_PACKAGE}/dll-vs/ngspice.dll" "${LVD_NGSPICE_PACKAGE}/dll-vs/libomp140.x86_64.dll"
          "${LVD_NGSPICE_CONSOLE_PACKAGE}/bin/sndfile.dll" "${LVD_NGSPICE_CONSOLE_PACKAGE}/bin/samplerate.dll"
     DESTINATION "${LVD_NGSPICE_RUNTIME_DIR}")
file(GLOB _lvd_ngspice_codemodels "${LVD_NGSPICE_PACKAGE}/lib/ngspice/*.cm")
file(COPY ${_lvd_ngspice_codemodels} DESTINATION "${LVD_NGSPICE_RUNTIME_DIR}/lib/ngspice")
# Start-up script read by ngspice (SPICE_SCRIPTS is set by the application);
# the packaged one loads code models from C:/Spice64.
file(WRITE "${LVD_NGSPICE_RUNTIME_DIR}/share/ngspice/scripts/spinit"
    "* ngspice start-up script for lvd RF cad.\n"
    "* The application loads the code models from lib/ngspice itself.\n"
    "alias exit quit\n"
    "unset osdi_enabled\n")
foreach(_doc COPYING README AUTHORS)
    if(EXISTS "${LVD_NGSPICE_CONSOLE_PACKAGE}/docs/${_doc}")
        file(COPY "${LVD_NGSPICE_CONSOLE_PACKAGE}/docs/${_doc}" DESTINATION "${LVD_NGSPICE_RUNTIME_DIR}")
    endif()
endforeach()

set(LVD_NGSPICE_CONSOLE "${LVD_NGSPICE_CONSOLE_PACKAGE}/bin/ngspice_con.exe")

# openEMS 0.37.0 for 64-bit Windows (requirements AN-9, section 10).
#
# The official MSVC build is downloaded once at configure time and the files
# that openEMS.exe and nf2ff.exe need (found with dumpbin /dependents) are
# copied next to the binaries as bin/openems/. The application runs both as
# separate processes; nothing links against them. AppCSXCAD, its Qt copy,
# the Octave and Python interfaces and the tutorials are not shipped.

set(LVD_OPENEMS_VERSION 0.37.0)

FetchContent_Declare(openems
    URL https://github.com/thliebig/openEMS-Project/releases/download/v0.37.0/openEMS_x64_v0.37.0_msvc.zip
    URL_HASH SHA256=e4f14fd6e9be7d0c1fd9c244768c21c1c2b62b8aa81f08253ebf6e06040cda5a
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(openems)

set(LVD_OPENEMS_PACKAGE "${openems_SOURCE_DIR}")
if(EXISTS "${LVD_OPENEMS_PACKAGE}/openEMS/openEMS.exe")
    set(LVD_OPENEMS_PACKAGE "${LVD_OPENEMS_PACKAGE}/openEMS")
endif()

set(_lvd_openems_files
    openEMS.exe openEMS.dll nf2ff.exe nf2ff.dll CSXCAD.dll fparser.dll
    boost_program_options-vc145-mt-x64-1_91.dll boost_thread-vc145-mt-x64-1_91.dll
    double-conversion.dll fmt.dll jpeg62.dll libexpat.dll liblzma.dll libpng16.dll lz4.dll pugixml.dll
    szip.dll tiff.dll verdict.dll z.dll
    msvcp140.dll vcruntime140.dll vcruntime140_1.dll
    vtkCommonColor-9.3.dll vtkCommonComputationalGeometry-9.3.dll vtkCommonCore-9.3.dll
    vtkCommonDataModel-9.3.dll vtkCommonExecutionModel-9.3.dll vtkCommonMath-9.3.dll vtkCommonMisc-9.3.dll
    vtkCommonSystem-9.3.dll vtkCommonTransforms-9.3.dll vtkDICOMParser-9.3.dll vtkFiltersCore-9.3.dll
    vtkFiltersGeneral-9.3.dll vtkFiltersGeometry-9.3.dll vtkFiltersHybrid-9.3.dll vtkFiltersSources-9.3.dll
    vtkFiltersVerdict-9.3.dll vtkImagingCore-9.3.dll vtkImagingSources-9.3.dll vtkIOCore-9.3.dll
    vtkIOGeometry-9.3.dll vtkIOImage-9.3.dll vtkIOLegacy-9.3.dll vtkIOPLY-9.3.dll vtkIOXML-9.3.dll
    vtkIOXMLParser-9.3.dll vtkkissfft-9.3.dll vtkloguru-9.3.dll vtkmetaio-9.3.dll vtkRenderingCore-9.3.dll
    vtksys-9.3.dll vtktoken-9.3.dll)

set(LVD_OPENEMS_RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin/openems")
foreach(_file IN LISTS _lvd_openems_files)
    if(NOT EXISTS "${LVD_OPENEMS_PACKAGE}/${_file}")
        message(FATAL_ERROR "openEMS package has an unexpected layout: ${_file} is missing")
    endif()
    file(COPY "${LVD_OPENEMS_PACKAGE}/${_file}" DESTINATION "${LVD_OPENEMS_RUNTIME_DIR}")
endforeach()

file(WRITE "${LVD_OPENEMS_RUNTIME_DIR}/NOTICE.txt"
"openEMS ${LVD_OPENEMS_VERSION} for Windows, as shipped with lvd RF cad
=========================================================

These files are an unmodified subset of the official openEMS Windows release
openEMS_x64_v${LVD_OPENEMS_VERSION}_msvc.zip from
https://github.com/thliebig/openEMS-Project/releases/tag/v${LVD_OPENEMS_VERSION}
(only the files openEMS.exe and nf2ff.exe need).

Components and licences (complete sources at the addresses given):
- openEMS, nf2ff: GNU General Public License v3 or later,
  https://github.com/thliebig/openEMS (tag v${LVD_OPENEMS_VERSION})
- CSXCAD: GNU Lesser General Public License v3, https://github.com/thliebig/CSXCAD
- fparser: LGPL v3, https://github.com/thliebig/fparser
- HDF5 (linked into openEMS): BSD 3-clause, https://github.com/HDFGroup/hdf5
- VTK 9.3: BSD 3-clause, https://vtk.org
- Boost 1.91: Boost Software License 1.0, https://www.boost.org
- zlib, libpng, libjpeg-turbo, libtiff, liblzma, lz4, szip, expat, pugixml,
  fmt, double-conversion, verdict: their permissive licences, see the
  respective projects
- Microsoft Visual C++ runtime: redistributable under the Visual Studio licence

The GNU GPL v3 text is in the LICENSE file of lvd RF cad.
")

# HDF5 C library (BSD-3), static, for reading openEMS far fields and field
# dumps (requirements section 10, decision 2). Built as an external project
# into its own prefix so its targets and install rules stay out of ours.
# Only the core C library: no tools, high-level library, examples, tests or
# compression filters (openEMS writes uncompressed data).

include(ExternalProject)

set(LVD_HDF5_PREFIX ${CMAKE_BINARY_DIR}/_deps/hdf5-install)
set(_hdf5_config $<IF:$<CONFIG:Debug>,Debug,Release>)

ExternalProject_Add(lvd_hdf5_build
    URL https://github.com/HDFGroup/hdf5/archive/refs/tags/2.2.0.tar.gz
    URL_HASH SHA256=5b8d75125ae7b4fef55d3b39e8d3dbfd238cdf63b6518afd67fa69ac80f06542
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX ${CMAKE_BINARY_DIR}/_deps/hdf5
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${LVD_HDF5_PREFIX}
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_STATIC_LIBS=ON
        -DBUILD_TESTING=OFF
        -DHDF5_BUILD_TOOLS=OFF
        -DHDF5_BUILD_UTILS=OFF
        -DHDF5_BUILD_EXAMPLES=OFF
        -DHDF5_BUILD_HL_LIB=OFF
        -DHDF5_BUILD_CPP_LIB=OFF
        -DHDF5_BUILD_FORTRAN=OFF
        -DHDF5_BUILD_JAVA=OFF
        -DHDF5_BUILD_DOC=OFF
        -DHDF5_ENABLE_ZLIB_SUPPORT=OFF
        -DHDF5_ENABLE_SZIP_SUPPORT=OFF
        -DHDF5_ENABLE_PLUGIN_SUPPORT=OFF
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${_hdf5_config} --parallel
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config ${_hdf5_config}
    BUILD_BYPRODUCTS
        ${LVD_HDF5_PREFIX}/lib/libhdf5.lib
        ${LVD_HDF5_PREFIX}/lib/libhdf5_D.lib
)

file(MAKE_DIRECTORY ${LVD_HDF5_PREFIX}/include)
add_library(lvd_hdf5 STATIC IMPORTED GLOBAL)
set_target_properties(lvd_hdf5 PROPERTIES
    IMPORTED_LOCATION ${LVD_HDF5_PREFIX}/lib/libhdf5.lib
    IMPORTED_LOCATION_DEBUG ${LVD_HDF5_PREFIX}/lib/libhdf5_D.lib
    INTERFACE_INCLUDE_DIRECTORIES ${LVD_HDF5_PREFIX}/include
    INTERFACE_LINK_LIBRARIES "shlwapi")
add_dependencies(lvd_hdf5 lvd_hdf5_build)

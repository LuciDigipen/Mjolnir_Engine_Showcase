# Release parents already build Release tools, only development trees need a sub-build.
if(NOT MIR_OPTIMIZE_ASSET_COOKER OR CMAKE_BUILD_TYPE STREQUAL "Release")
    return()
endif()
if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Release offline tools require a single-config generator")
endif()

include(ExternalProject)
set(offline_binary_dir "${CMAKE_BINARY_DIR}/tools-release")
set(offline_cmake_args
    "-DCMAKE_BUILD_TYPE:STRING=Release"
    "-DMIR_TOOLS_ONLY:BOOL=ON"
    "-DBUILD_TESTING:BOOL=OFF"
    "-DMIR_LARGE_WORLD:BOOL=${MIR_LARGE_WORLD}"
    "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}"
    "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}")
foreach(variable CMAKE_TOOLCHAIN_FILE VCPKG_INSTALLED_DIR VCPKG_TARGET_TRIPLET VCPKG_HOST_TRIPLET SLANGC_EXECUTABLE)
    if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
        list(APPEND offline_cmake_args "-D${variable}:STRING=${${variable}}")
    endif()
endforeach()
list(APPEND offline_cmake_args "-DVCPKG_MANIFEST_INSTALL:BOOL=OFF")

ExternalProject_Add(mir_offline_tools
    SOURCE_DIR "${CMAKE_SOURCE_DIR}"
    BINARY_DIR "${offline_binary_dir}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS ${offline_cmake_args}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target AssetCooker TerrainTool --parallel 4
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS
        "${offline_binary_dir}/tools/AssetCooker/AssetCooker${CMAKE_EXECUTABLE_SUFFIX}"
        "${offline_binary_dir}/tools/TerrainTool/TerrainTool${CMAKE_EXECUTABLE_SUFFIX}"
    INSTALL_COMMAND "")

foreach(tool AssetCooker TerrainTool)
    add_executable(${tool} IMPORTED GLOBAL)
    set_property(TARGET ${tool} PROPERTY IMPORTED_LOCATION
        "${offline_binary_dir}/tools/${tool}/${tool}${CMAKE_EXECUTABLE_SUFFIX}")
    add_dependencies(${tool} mir_offline_tools)
endforeach()

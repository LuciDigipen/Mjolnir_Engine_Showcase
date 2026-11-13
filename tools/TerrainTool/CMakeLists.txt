find_package(nlohmann_json CONFIG REQUIRED)

# /RTC1 cannot combine with the Debug /O2 below and /Od /Ob0 would only be overridden; cl defaults to /Od.
if(MSVC)
    string(REGEX REPLACE "/RTC1|/Ob0|/Od" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

# Offline terrain pipeline; the engine never links it.
add_library(terrain_bake STATIC
    TerrainBake.cpp
    TerrainBaseCache.cpp
    TerrainCandidate.cpp
    TerrainContinuity.cpp
    TerrainDerivatives.cpp
    TerrainMixedContinuity.cpp
    TerrainGenerator.cpp
    TerrainHeightSource.cpp
    TerrainIncremental.cpp
    TerrainMaterial.cpp
    TerrainNoise.cpp
    TerrainPackage.cpp
    TerrainProgress.cpp
    TerrainPrune.cpp
    TerrainPublish.cpp
    TerrainRecipe.cpp
    TerrainRegions.cpp
    TerrainReport.cpp
    TerrainSeams.cpp
    TerrainSync.cpp
    TerrainValidate.cpp
)
target_include_directories(terrain_bake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(terrain_bake
    PUBLIC mir::terrain mir::assets mir::core
    PRIVATE mir::jobs nlohmann_json::nlohmann_json)

# The root CMakeLists runs its sync (and package without the editor) on every build.
add_executable(TerrainTool main.cpp)
engine_target_utf8(TerrainTool)
target_link_libraries(TerrainTool PRIVATE terrain_bake)

set(terrain_bake_debug_o2 "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/O2>")
target_compile_options(terrain_bake PRIVATE ${terrain_bake_debug_o2})
target_compile_options(TerrainTool PRIVATE ${terrain_bake_debug_o2})

if(BUILD_TESTING)
    add_executable(terrain_tool_tests
        tests/TerrainBake.tests.cpp
        tests/TerrainBaseCache.tests.cpp
        tests/TerrainGenerator.tests.cpp
        tests/TerrainHeightmap.tests.cpp
        tests/TerrainIncremental.tests.cpp
        tests/TerrainDerivatives.tests.cpp
        tests/TerrainMaterial.tests.cpp
        tests/TerrainOverlayMesh.tests.cpp
        tests/TerrainPackage.tests.cpp
        tests/TerrainPrecision.tests.cpp
        tests/TerrainPrune.tests.cpp
        tests/TerrainPublish.tests.cpp
        tests/TerrainRecipe.tests.cpp
        tests/TerrainRegions.tests.cpp
        tests/TerrainRenderMesh.tests.cpp
        tests/TerrainSeams.tests.cpp
        tests/TerrainSync.tests.cpp
        tests/TerrainToolCli.tests.cpp
        tests/TerrainValidate.tests.cpp
        tests/TerrainWeightField.tests.cpp
    )
    engine_target_utf8(terrain_tool_tests)
    target_link_libraries(terrain_tool_tests PRIVATE terrain_bake mir::renderer nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
    target_compile_definitions(terrain_tool_tests PRIVATE TERRAIN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_compile_definitions(terrain_tool_tests PRIVATE TERRAIN_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
    target_compile_definitions(terrain_tool_tests PRIVATE TERRAIN_TOOL_EXE="$<TARGET_FILE:TerrainTool>")
    add_dependencies(terrain_tool_tests TerrainTool)
    catch_discover_tests(terrain_tool_tests)
endif()

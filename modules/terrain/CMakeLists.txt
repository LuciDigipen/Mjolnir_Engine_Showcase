engine_add_module(terrain)

target_sources(engine_terrain PRIVATE
    src/terrain/TerrainModule.cpp
    src/terrain/ComponentRegistration.cpp
    src/terrain/TerrainAreaSamples.cpp
    src/terrain/TerrainBaseHeights.cpp
    src/terrain/TerrainBaseWeights.cpp
    src/terrain/TerrainCollision.cpp
    src/terrain/TerrainCollisionResidency.cpp
    src/terrain/TerrainEditLayer.cpp
    src/terrain/TerrainEditStore.cpp
    src/terrain/TerrainAssetPath.cpp
    src/terrain/TerrainByteSource.cpp
    src/terrain/TerrainExecutor.cpp
    src/terrain/TerrainGround.cpp
    src/terrain/TerrainHeightQuery.cpp
    src/terrain/TerrainLayerWeightQuery.cpp
    src/terrain/TerrainLayout.cpp
    src/terrain/TerrainLod.cpp
    src/terrain/TerrainMaterialUv.cpp
    src/terrain/TerrainMeshResidency.cpp
    src/terrain/TerrainOverlay.cpp
    src/terrain/TerrainOverlayHandover.cpp
    src/terrain/TerrainOverlayMesh.cpp
    src/terrain/TerrainPackFormat.cpp
    src/terrain/TerrainPackWriter.cpp
    src/terrain/TerrainRecipeMaterials.cpp
    src/terrain/TerrainRegion.cpp
    src/terrain/TerrainRegionGrid.cpp
    src/terrain/TerrainRegionWeights.cpp
    src/terrain/TerrainRelease.cpp
    src/terrain/TerrainReleaseLock.cpp
    src/terrain/TerrainReleaseSwitch.cpp
    src/terrain/TerrainRenderMesh.cpp
    src/terrain/TerrainScene.cpp
    src/terrain/TerrainStreamer.cpp
    src/terrain/TerrainStreamingConfiguration.cpp
    src/terrain/TerrainStreamingSettings.cpp
    src/terrain/TerrainSurface.cpp
    src/terrain/TerrainTileMesh.cpp
    src/terrain/TerrainViewSelection.cpp
    src/terrain/TerrainWeights.cpp
    src/terrain/TileAddressing.cpp
    src/terrain/TileHeights.cpp
)
find_package(nlohmann_json CONFIG REQUIRED)

# Terrain's public headers expose nlohmann::json, not serial's API.
target_link_libraries(engine_terrain PUBLIC mir::assets mir::core nlohmann_json::nlohmann_json PRIVATE mir::serial)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# /RTC1 cannot combine with TerrainLod.cpp's Debug /O2 and /Od /Ob0 would only be overridden; cl defaults to /Od.
# Tests keep all three.
if(MSVC)
    string(REGEX REPLACE "/RTC1|/Ob0|/Od" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()
set_property(SOURCE src/terrain/TerrainLod.cpp APPEND PROPERTY COMPILE_OPTIONS "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/O2>")

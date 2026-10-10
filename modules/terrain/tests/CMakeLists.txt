engine_add_module_tests(terrain
    TerrainAreaSamples.tests.cpp
    TerrainAssetPath.tests.cpp
    TerrainHeightQuery.tests.cpp
    TerrainIdentity.tests.cpp
    TerrainLayout.tests.cpp
    TerrainPackFormat.tests.cpp
    TerrainPackIntegrity.tests.cpp
    TerrainRegion.tests.cpp
    TerrainRelease.tests.cpp
    TerrainScene.tests.cpp
    TerrainStreamer.tests.cpp
    TerrainStreamingSettings.tests.cpp
    TerrainSurface.tests.cpp
    TerrainTileMesh.tests.cpp
    TerrainViewSelection.tests.cpp
    TileAddressing.tests.cpp
    TileHeights.tests.cpp
)

# TerrainInstance must round-trip SceneIO; terrain itself never links scene.
target_link_libraries(engine_terrain_tests PRIVATE mir::scene)

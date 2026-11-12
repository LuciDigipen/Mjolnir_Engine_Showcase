engine_add_module_tests(terrain NO_SYSTEM_TIES
    TerrainAreaSamples.tests.cpp
    TerrainAssetPath.tests.cpp
    TerrainCollider.tests.cpp
    TerrainCollision.tests.cpp
    TerrainCollisionResidency.tests.cpp
    TerrainCollisionResidencyJolt.tests.cpp
    TerrainEditLayer.tests.cpp
    TerrainEditStore.tests.cpp
    TerrainGround.tests.cpp
    TerrainHeightQuery.tests.cpp
    TerrainIdentity.tests.cpp
    TerrainLayout.tests.cpp
    TerrainMaterialUv.tests.cpp
    TerrainMeshResidency.tests.cpp
    TerrainOverlay.tests.cpp
    TerrainPackFormat.tests.cpp
    TerrainPackIntegrity.tests.cpp
    TerrainPrefab.tests.cpp
    TerrainRegion.tests.cpp
    RunGuarded.tests.cpp
    TerrainRelease.tests.cpp
    TerrainReleaseSwitch.tests.cpp
    TerrainScene.tests.cpp
    TerrainStreamer.tests.cpp
    TerrainStreamingConfiguration.tests.cpp
    TerrainStreamingSettings.tests.cpp
    TerrainSurface.tests.cpp
    TerrainTileMesh.tests.cpp
    TerrainViewSelection.tests.cpp
    TileAddressing.tests.cpp
    TileHeights.tests.cpp
)

# TerrainInstance must round-trip SceneIO, and the collision tests check contact against
# real Jolt bodies; terrain itself never links scene or physics.
target_link_libraries(engine_terrain_tests PRIVATE mir::scene mir::physics mir::prefab terrain_bake)
target_compile_definitions(engine_terrain_tests PRIVATE
    TERRAIN_COLLISION_RECIPE="${PROJECT_SOURCE_DIR}/tools/TerrainTool/tests/fixtures/mixed-2x2.terrain.json")

# the crash runs in a child process, so the test run survives it
if(WIN32 AND BUILD_TESTING)
    add_executable(engine_terrain_crash_probe TerrainCrashProbe.cpp)
    engine_target_utf8(engine_terrain_crash_probe)
    target_link_libraries(engine_terrain_crash_probe PRIVATE mir::terrain)
    target_sources(engine_terrain_tests PRIVATE TerrainCrash.tests.cpp)
    add_dependencies(engine_terrain_tests engine_terrain_crash_probe)
    target_compile_definitions(engine_terrain_tests
        PRIVATE MIR_TERRAIN_CRASH_PROBE_PATH="$<TARGET_FILE:engine_terrain_crash_probe>")
endif()

engine_add_module(terrain_runtime PCH_HEADERS glm/glm.hpp)

target_sources(engine_terrain_runtime PRIVATE
    src/terrain_runtime/TerrainGroundBridge.cpp
    src/terrain_runtime/TerrainJobExecutor.cpp
    src/terrain_runtime/TerrainMaterialSlots.cpp
    src/terrain_runtime/TerrainRender.cpp
    src/terrain_runtime/TerrainRuntime.cpp
    src/terrain_runtime/TerrainRuntimeModule.cpp
    src/terrain_runtime/TerrainWeightResidency.cpp
)
target_link_libraries(engine_terrain_runtime PRIVATE mir::jobs)

# A bridge: terrain stays free of Vulkan and Jolt, so its tests and TerrainTool need neither.
# PUBLIC: the headers hold renderer, physics and terrain types
target_link_libraries(engine_terrain_runtime
    PUBLIC mir::core
           mir::physics
           mir::project
           mir::renderer
           mir::terrain
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

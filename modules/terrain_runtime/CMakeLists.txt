engine_add_module(terrain_runtime)

target_sources(engine_terrain_runtime PRIVATE
    src/terrain_runtime/TerrainGroundBridge.cpp
    src/terrain_runtime/TerrainRender.cpp
    src/terrain_runtime/TerrainRuntime.cpp
    src/terrain_runtime/TerrainRuntimeModule.cpp
)

# A bridge: terrain stays free of Vulkan and Jolt, so its tests and TerrainTool need neither.
# PUBLIC: the headers hold renderer, physics and terrain types
target_link_libraries(engine_terrain_runtime
    PUBLIC mir::core
           mir::physics
           mir::project
           mir::renderer
           mir::terrain
)

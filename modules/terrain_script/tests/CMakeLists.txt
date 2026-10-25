engine_add_module_tests(terrain_script
    TerrainBindings.tests.cpp
)

target_link_libraries(engine_terrain_script_tests PRIVATE mir::core mir::terrain)
target_compile_definitions(engine_terrain_script_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

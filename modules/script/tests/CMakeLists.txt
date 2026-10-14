engine_add_module_tests(script
    WorldBindings.tests.cpp
    InputBindings.tests.cpp
    PhysicsBindings.tests.cpp
    CollisionBindings.tests.cpp
    AudioBindings.tests.cpp
    TerrainContactScript.tests.cpp
)


# terrain only for TerrainInstance, which the TerrainCheck script asks about by name.
target_link_libraries(engine_script_tests PRIVATE mir::audio mir::input mir::physics mir::terrain)
target_compile_definitions(engine_script_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

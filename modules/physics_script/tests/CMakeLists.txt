engine_add_module_tests(physics_script
    PhysicsBindings.tests.cpp
    CollisionBindings.tests.cpp
    TerrainContactScript.tests.cpp
)

target_link_libraries(engine_physics_script_tests PRIVATE mir::core mir::physics mir::terrain)
target_compile_definitions(engine_physics_script_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

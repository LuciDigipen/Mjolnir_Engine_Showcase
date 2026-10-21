engine_add_module_tests(script
    WorldBindings.tests.cpp
    InputBindings.tests.cpp
    PhysicsBindings.tests.cpp
    CollisionBindings.tests.cpp
    AudioBindings.tests.cpp
    PostProcessingBindings.tests.cpp
    ScriptSystem.tests.cpp
    TerrainContactScript.tests.cpp
    TerrainBindings.tests.cpp
)


target_link_libraries(engine_script_tests PRIVATE
    mir::audio mir::input mir::physics mir::renderer mir::terrain sol2::sol2 PkgConfig::LuaJIT)
target_compile_definitions(engine_script_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

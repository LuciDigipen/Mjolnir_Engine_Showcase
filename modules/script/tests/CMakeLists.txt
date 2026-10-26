engine_add_module_tests(script
    WorldBindings.tests.cpp
    ScriptSystem.tests.cpp
    ScriptFixedSystem.tests.cpp
    ScriptHostRunning.tests.cpp
    ScriptResolveSystem.tests.cpp
    ScriptModule.tests.cpp
)


target_link_libraries(engine_script_tests PRIVATE
    mir::assets mir::script_bindings)
target_compile_definitions(engine_script_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

engine_add_module_tests(renderer_script
    PostProcessingBindings.tests.cpp
)

target_link_libraries(engine_renderer_script_tests PRIVATE mir::core mir::renderer)
target_compile_definitions(engine_renderer_script_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

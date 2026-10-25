engine_add_module_tests(input_script
    InputBindings.tests.cpp
)

target_link_libraries(engine_input_script_tests PRIVATE mir::core mir::input)
target_compile_definitions(engine_input_script_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

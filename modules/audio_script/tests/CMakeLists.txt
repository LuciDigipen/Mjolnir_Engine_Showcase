engine_add_module_tests(audio_script
    AudioBindings.tests.cpp
)

target_link_libraries(engine_audio_script_tests PRIVATE mir::core mir::audio)
target_compile_definitions(engine_audio_script_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

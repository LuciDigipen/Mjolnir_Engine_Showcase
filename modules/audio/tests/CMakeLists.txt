engine_add_module_tests(audio
    AudioSettings.tests.cpp
    BankEvents.tests.cpp
)

target_compile_definitions(engine_audio_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

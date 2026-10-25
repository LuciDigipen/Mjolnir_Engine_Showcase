engine_add_module(audio_script)

target_sources(engine_audio_script PRIVATE
    src/audio_script/AudioBindings.cpp
    src/audio_script/AudioScriptModule.cpp
)

# PUBLIC script_bindings: AudioBindings.h names sol::state
target_link_libraries(engine_audio_script
    PUBLIC mir::script
           mir::script_bindings
    PRIVATE mir::core
            mir::audio
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

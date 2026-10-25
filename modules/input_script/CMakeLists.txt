engine_add_module(input_script)

target_sources(engine_input_script PRIVATE
    src/input_script/InputBindings.cpp
    src/input_script/InputScriptModule.cpp
)

# PUBLIC script_bindings: InputBindings.h names sol::state
target_link_libraries(engine_input_script
    PUBLIC mir::script
           mir::script_bindings
    PRIVATE mir::core
            mir::input
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

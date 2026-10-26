engine_add_module(renderer_script)

target_sources(engine_renderer_script PRIVATE
    src/renderer_script/MeshBindings.cpp
    src/renderer_script/PostProcessingBindings.cpp
    src/renderer_script/RendererScriptModule.cpp
)

# PUBLIC script_bindings: PostProcessingBindings.h and MeshBindings.h name sol::state
target_link_libraries(engine_renderer_script
    PUBLIC mir::script
           mir::script_bindings
    PRIVATE mir::core
            mir::renderer
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

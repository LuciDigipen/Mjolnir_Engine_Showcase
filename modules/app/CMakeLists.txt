engine_add_module(app)

target_link_libraries(engine_app
    PUBLIC mir::assets
           mir::core
           mir::project
           mir::window
           mir::renderer
           mir::gameui
           mir::gameshell
           mir::imguiruntime
           mir::scene
           mir::script
           mir::audio_script
           mir::input_script
           mir::physics_script
           mir::renderer_script
           mir::terrain_script
    PRIVATE mir::input
            mir::camera
            mir::animation
            mir::audio
            mir::physics
            mir::prefab
            mir::serial
            mir::terrain
            mir::terrain_runtime
)

# App's yes/no prompts
find_package(tinyfiledialogs CONFIG REQUIRED)
target_link_libraries(engine_app PRIVATE tinyfiledialogs::tinyfiledialogs)

target_sources(engine_app PRIVATE
    src/app/App.cpp
    src/app/ErrorReport.cpp
    src/app/Prompt.cpp
)

# PUBLIC: App.h holds the editor's members only with it, so everything that
# includes App.h has to agree on which App it is
target_compile_definitions(engine_app PUBLIC MIR_WITH_EDITOR=$<BOOL:${MJOLNIR_WITH_EDITOR}>)

# the editor, and the asset import queue that only its panel drives
if(MJOLNIR_WITH_EDITOR)
    target_link_libraries(engine_app PUBLIC mir::editor)
endif()

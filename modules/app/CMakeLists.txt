engine_add_module(app)

# PUBLIC: App.h holds every module as a member
target_link_libraries(engine_app
    PUBLIC mir::animation
           mir::assets
           mir::audio
           mir::audio_script
           mir::camera
           mir::core
           mir::gameshell
           mir::gameui
           mir::gameui_imgui
           mir::imguiruntime
           mir::input
           mir::input_script
           mir::physics
           mir::physics_script
           mir::project
           mir::renderer
           mir::renderer_script
           mir::scene
           mir::script
           mir::terrain
           mir::terrain_runtime
           mir::terrain_script
           mir::window
    PRIVATE mir::prefab
            mir::serial
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

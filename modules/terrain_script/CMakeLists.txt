engine_add_module(terrain_script)

target_sources(engine_terrain_script PRIVATE
    src/terrain_script/TerrainBindings.cpp
    src/terrain_script/TerrainScriptModule.cpp
)

# PUBLIC script_bindings: TerrainBindings.h names sol::state
target_link_libraries(engine_terrain_script
    PUBLIC mir::script
           mir::script_bindings
    PRIVATE mir::core
            mir::terrain
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

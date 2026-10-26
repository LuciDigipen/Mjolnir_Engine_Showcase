engine_add_module(physics_layers)

target_sources(engine_physics_layers PRIVATE
    src/physics_layers/PhysicsLayerSettings.cpp
)

# a plain library: base and other plain libraries only. Jolt-free, so the editor can edit the layers
# without linking physics. PUBLIC: SetupError is in a public header
target_link_libraries(engine_physics_layers
    PUBLIC mir::core_base
    PRIVATE mir::serial
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

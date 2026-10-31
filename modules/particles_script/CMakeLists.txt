engine_add_module(particles_script)

# ParticleBindings.h exposes sol::state, other implementation dependencies stay private.
target_link_libraries(engine_particles_script
    PUBLIC mir::script mir::script_bindings
    PRIVATE mir::core mir::particles)

target_sources(engine_particles_script PRIVATE
    src/particles_script/ParticleBindings.cpp
    src/particles_script/ParticlesScriptModule.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

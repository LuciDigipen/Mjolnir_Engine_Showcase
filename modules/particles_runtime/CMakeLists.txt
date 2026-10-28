engine_add_module(particles_runtime)

target_sources(engine_particles_runtime PRIVATE
    src/particles_runtime/ParticlesRuntimeModule.cpp
)

# PUBLIC because ModuleContext appears in the bridge header.
target_link_libraries(engine_particles_runtime PUBLIC mir::core PRIVATE mir::particles mir::renderer)

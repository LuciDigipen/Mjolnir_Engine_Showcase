engine_add_module_tests(particles
    ParticleAuthoring.tests.cpp
    ParticleEffect.tests.cpp
    ParticleRibbon.tests.cpp
    ParticleRuntime.tests.cpp
    ParticleLifecycle.tests.cpp)
target_link_libraries(engine_particles_tests PRIVATE mir::scene)

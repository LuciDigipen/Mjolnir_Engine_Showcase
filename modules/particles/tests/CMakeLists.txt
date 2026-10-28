# @file CMakeLists.txt
# @author sumin.park
# @brief Registers headless particle asset and lifecycle tests.
# @copyright Copyright (c) 2026 DigiPen (USA) Corporation

engine_add_module_tests(particles
    ParticleEffect.tests.cpp
    ParticleRibbon.tests.cpp
    ParticleRuntime.tests.cpp
    ParticleLifecycle.tests.cpp)
target_link_libraries(engine_particles_tests PRIVATE mir::scene)

# @file CMakeLists.txt
# @author sumin.park
# @brief Builds the CPU particle asset and playback module.
# @copyright Copyright (c) 2026 DigiPen (USA) Corporation

engine_add_module(particles)

find_package(nlohmann_json CONFIG REQUIRED)
# Public headers expose core world/coordinate types and asset identities.
target_link_libraries(engine_particles PUBLIC mir::core mir::assets PRIVATE nlohmann_json::nlohmann_json)

target_sources(engine_particles PRIVATE
    src/particles/ComponentRegistration.cpp
    src/particles/ParticleEffect.cpp
    src/particles/ParticleRibbon.cpp
    src/particles/ParticleRuntime.cpp
    src/particles/ParticlesModule.cpp
    src/particles/ParticleWorld.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

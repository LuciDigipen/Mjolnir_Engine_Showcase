add_executable(ParticleGpuCheck main.cpp)
find_package(glfw3 CONFIG REQUIRED)
target_link_libraries(ParticleGpuCheck PRIVATE mir::particles_gpu mir::particles_runtime mir::particles mir::window glfw)
set_target_properties(ParticleGpuCheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_dependencies(ParticleGpuCheck MjolnirEngine)
add_test(NAME smoke.particle-gpu
    COMMAND ParticleGpuCheck "${CMAKE_BINARY_DIR}/particle-gpu-smoke.png")
set_tests_properties(smoke.particle-gpu PROPERTIES LABELS smoke RUN_SERIAL TRUE TIMEOUT 180)

# Optional installed capture API, nothing is downloaded or linked into the game.
find_path(RENDERDOC_INCLUDE_DIR renderdoc_app.h PATHS "$ENV{ProgramFiles}/RenderDoc")
if(RENDERDOC_INCLUDE_DIR)
    target_include_directories(ParticleGpuCheck PRIVATE "${RENDERDOC_INCLUDE_DIR}")
    target_compile_definitions(ParticleGpuCheck PRIVATE MIR_PARTICLE_RENDERDOC=1)
endif()

# Cook test inputs through the same compiler and asset path used by the game.
set(particle_fixture_source "${CMAKE_BINARY_DIR}/particle_gpu_source")
file(MAKE_DIRECTORY "${particle_fixture_source}")
configure_file(../AssetCooker/tests/fixtures/checker.png "${particle_fixture_source}/checker.png" COPYONLY)
configure_file(../AssetCooker/tests/particle_fixtures/exhaust.emitter.json "${particle_fixture_source}/exhaust.emitter.json" COPYONLY)
configure_file(../AssetCooker/tests/particle_fixtures/accelerated.emitter.json "${particle_fixture_source}/accelerated.emitter.json" COPYONLY)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/particle_gpu_cooked.stamp"
    COMMAND AssetCooker --source-dir "${particle_fixture_source}" --id-root assets --out "${CMAKE_BINARY_DIR}/particle_gpu_cooked"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/particle_gpu_cooked.stamp"
    DEPENDS AssetCooker "${particle_fixture_source}/exhaust.emitter.json" "${particle_fixture_source}/accelerated.emitter.json" "${particle_fixture_source}/checker.png"
    VERBATIM)
add_custom_target(ParticleGpuCheck_assets DEPENDS "${CMAKE_BINARY_DIR}/particle_gpu_cooked.stamp")
add_dependencies(ParticleGpuCheck ParticleGpuCheck_assets)

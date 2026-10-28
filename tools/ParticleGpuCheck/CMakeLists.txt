add_executable(ParticleGpuCheck main.cpp)
find_package(glfw3 CONFIG REQUIRED)
target_link_libraries(ParticleGpuCheck PRIVATE mir::renderer mir::window glfw)
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

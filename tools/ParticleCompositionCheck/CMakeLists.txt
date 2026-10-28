# @file CMakeLists.txt
# @author sumin.park
# @brief Builds the static particle composition verification fixture.
# @copyright Copyright (c) 2026 DigiPen (USA) Corporation

add_executable(ParticleCompositionCheck main.cpp)
target_link_libraries(ParticleCompositionCheck PRIVATE mir::app)
# Share the engine's executable-relative shaders, assets, DLLs and fonts.
set_target_properties(ParticleCompositionCheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_dependencies(ParticleCompositionCheck MjolnirEngine)
add_test(NAME smoke.particle-composition
    COMMAND ParticleCompositionCheck raster "${CMAKE_BINARY_DIR}/particle-probe-smoke.png")
set_tests_properties(smoke.particle-composition PROPERTIES LABELS smoke RUN_SERIAL TRUE TIMEOUT 120)

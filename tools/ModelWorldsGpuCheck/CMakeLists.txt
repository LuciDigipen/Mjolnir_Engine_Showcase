add_executable(ModelWorldsGpuCheck main.cpp)
target_link_libraries(ModelWorldsGpuCheck PRIVATE mir::animation mir::assets mir::core mir::jobs mir::renderer mir::window)
set_target_properties(ModelWorldsGpuCheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_dependencies(ModelWorldsGpuCheck MjolnirEngine)
# the cooked HelloWorld assets, which hold a model with a mesh, a material and a texture
add_test(NAME smoke.model-worlds-gpu COMMAND ModelWorldsGpuCheck "${CMAKE_BINARY_DIR}/model-worlds-gpu"
    "${CMAKE_BINARY_DIR}/cooked")
set_tests_properties(smoke.model-worlds-gpu PROPERTIES LABELS smoke RUN_SERIAL TRUE TIMEOUT 180)

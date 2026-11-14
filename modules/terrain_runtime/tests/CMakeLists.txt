engine_add_module_tests(terrain_runtime TerrainGpuUploader.tests.cpp TerrainJobExecutor.tests.cpp
    TerrainMaterialSlots.tests.cpp TerrainWeightResidency.tests.cpp)
target_link_libraries(engine_terrain_runtime_tests PRIVATE mir::jobs)
target_include_directories(engine_terrain_runtime_tests PRIVATE ../../terrain/tests ../../renderer/tests)

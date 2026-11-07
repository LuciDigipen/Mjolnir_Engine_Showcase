engine_add_module_tests(scene
    ComponentJson.tests.cpp
    SceneIO.tests.cpp
    ScenePrefabInstance.tests.cpp
    PlaySnapshot.tests.cpp
    EntitySnapshot.tests.cpp
    SceneDuplicate.tests.cpp
    SceneModule.tests.cpp
)
target_link_libraries(engine_scene_tests PRIVATE mir::jobs)

engine_add_module(prefab)

target_link_libraries(engine_prefab
    PUBLIC mir::core
           mir::assets
           mir::serial
           mir::scene
)

target_sources(engine_prefab PRIVATE
    src/prefab/CompiledPrefab.cpp
    src/prefab/PrefabAsset.cpp
    src/prefab/PrefabAuthoring.cpp
    src/prefab/PrefabCapture.cpp
    src/prefab/PrefabCoordinator.cpp
    src/prefab/PrefabDocument.cpp
    src/prefab/PrefabFormat.cpp
    src/prefab/PrefabModule.cpp
    src/prefab/PrefabId.cpp
    src/prefab/PrefabInstantiate.cpp
    src/prefab/PrefabOverrides.cpp
    src/prefab/PrefabRegistry.cpp
    src/prefab/PrefabSave.cpp
    src/prefab/PrefabSceneInstance.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

engine_add_module(prefab)

target_link_libraries(engine_prefab
    PUBLIC mir::core
           mir::assets
           mir::serial
    PRIVATE mir::scene
)

target_sources(engine_prefab PRIVATE
    src/prefab/CompiledPrefab.cpp
    src/prefab/PrefabDocument.cpp
    src/prefab/PrefabFormat.cpp
    src/prefab/PrefabId.cpp
    src/prefab/PrefabInstantiate.cpp
    src/prefab/PrefabRegistry.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

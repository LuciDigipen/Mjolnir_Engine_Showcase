engine_add_module(prefab)

target_link_libraries(engine_prefab
    PUBLIC mir::core
           mir::assets
           mir::serial
)

target_sources(engine_prefab PRIVATE
    src/prefab/PrefabDocument.cpp
    src/prefab/PrefabFormat.cpp
    src/prefab/PrefabId.cpp
    src/prefab/PrefabRegistry.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

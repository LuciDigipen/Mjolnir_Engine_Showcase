engine_add_module(scene)

target_sources(engine_scene PRIVATE
    src/scene/ComponentJson.cpp
    src/scene/EntityCodec.cpp
    src/scene/EntitySnapshot.cpp
    src/scene/SceneDuplicate.cpp
    src/scene/SceneEntityRefs.cpp
    src/scene/SceneInstanceCodec.cpp
    src/scene/SceneIO.cpp
    src/scene/SceneLoader.cpp
    src/scene/SceneModule.cpp
    src/scene/PlaySnapshot.cpp)

# core's Entity/World types and nlohmann::json are in scene's public headers
target_link_libraries(engine_scene PUBLIC mir::core
                                             mir::serial)
target_link_libraries(engine_scene PRIVATE mir::jobs)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

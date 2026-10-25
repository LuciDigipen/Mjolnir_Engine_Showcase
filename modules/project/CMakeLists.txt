engine_add_module(project)

target_sources(engine_project PRIVATE
    src/project/GameProject.cpp
    src/project/ProjectSettings.cpp
)

# a plain library: base and other plain libraries only.
# PUBLIC: SetupError and nlohmann::json are in the public headers
target_link_libraries(engine_project PUBLIC mir::core_base mir::serial)

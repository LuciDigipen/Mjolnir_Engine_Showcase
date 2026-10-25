add_executable(MjolnirErrorReporter WIN32)
target_sources(MjolnirErrorReporter PRIVATE main.cpp)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(tinyfiledialogs CONFIG REQUIRED)
target_link_libraries(MjolnirErrorReporter PRIVATE nlohmann_json::nlohmann_json tinyfiledialogs::tinyfiledialogs)

if(MSVC)
    set_target_properties(MjolnirErrorReporter PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

engine_add_module(script)

find_package(sol2 CONFIG REQUIRED)

# luajit has no CMake config package - just a .pc file - so it's discovered
# through pkg-config instead of find_package(). sol2 auto-detects LuaJIT via
# LUAJIT_VERSION in its own headers; nothing else here needs to know which
# Lua flavor is underneath.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LuaJIT REQUIRED IMPORTED_TARGET luajit)

target_link_libraries(engine_script
    # PUBLIC assets: ScriptReloadWatcher.h, inside ScriptModule.h, holds AssetIds
    PUBLIC  mir::assets
            mir::core
    PRIVATE sol2::sol2
            PkgConfig::LuaJIT
)
target_include_directories(engine_script PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include_bindings)

# What a bridge library (audio_script, physics_script, ...) links to add Lua bindings.
# Its own include root, so a library that links only script never sees sol2
add_library(engine_script_bindings INTERFACE)
add_library(mir::script_bindings ALIAS engine_script_bindings)
target_include_directories(engine_script_bindings INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include_bindings)
target_link_libraries(engine_script_bindings INTERFACE mir::script sol2::sol2 PkgConfig::LuaJIT)

target_sources(engine_script PRIVATE
    src/script/ComponentRegistration.cpp
    src/script/ScriptFixedSystem.cpp
    src/script/ScriptHost.cpp
    src/script/ScriptModule.cpp
    src/script/ScriptReloadWatcher.cpp
    src/script/ScriptResolveSystem.cpp
    src/script/ScriptSystem.cpp
    src/script/bindings/EntityBindings.cpp
    src/script/bindings/WorldBindings.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

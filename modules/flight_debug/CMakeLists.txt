engine_add_module(flight_debug)

target_sources(engine_flight_debug PRIVATE
    src/flight_debug/FlightDebugModule.cpp
    src/flight_debug/FlightDebugRows.cpp
)

# A Tools drawer over the Play world; imgui stays private, so no ImGui type reaches a public header.
target_link_libraries(engine_flight_debug
    PUBLIC mir::core
    PRIVATE mir::flight
            mir::imguiruntime
            mir::input
            imgui::imgui
)

# The window is a Play-world tool in an editor build and a developer toggle in a game build.
target_compile_definitions(engine_flight_debug PRIVATE MIR_WITH_EDITOR=$<BOOL:${MJOLNIR_WITH_EDITOR}>)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

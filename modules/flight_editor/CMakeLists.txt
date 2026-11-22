# not part of a build without the editor (MJOLNIR_WITH_EDITOR OFF)
if(NOT MJOLNIR_WITH_EDITOR)
    return()
endif()

engine_add_module(flight_editor)

target_sources(engine_flight_editor PRIVATE
    src/flight_editor/FlightEditorModule.cpp
    src/flight_editor/FlightModelCreate.cpp
    src/flight_editor/FlightModelDraft.cpp
    src/flight_editor/FlightModelTemplates.cpp
)

# PUBLIC: FlightEditorModule.h takes the EditorModule it registers with, and the
# draft header reuses flight's config and flight_debug's save callback
target_link_libraries(engine_flight_editor
    PUBLIC mir::editor
           mir::flight
           mir::flight_debug
    PRIVATE mir::core
            imgui::imgui
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

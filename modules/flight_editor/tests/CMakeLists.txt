engine_add_module_tests(flight_editor
    FlightModelCreate.tests.cpp
    FlightModelDraft.tests.cpp
    FlightModelTemplates.tests.cpp
)

target_compile_definitions(engine_flight_editor_tests PRIVATE MIR_GAMES_DIR="${CMAKE_SOURCE_DIR}/games")

# The templates and drafts are flight types; the saving helper comes from flight_debug.
target_link_libraries(engine_flight_editor_tests PRIVATE mir::flight mir::flight_debug)

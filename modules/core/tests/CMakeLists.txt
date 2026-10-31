engine_add_module_tests(core_base
    Paths.tests.cpp
    FileIO.tests.cpp
    Process.tests.cpp
    LogHistory.tests.cpp
    LogRepeats.tests.cpp
    SetupError.tests.cpp
    WorldCoordinates.tests.cpp
    RegistrationList.tests.cpp
)

# each crash runs in a child process, so the test run survives it
if(WIN32 AND BUILD_TESTING)
    add_executable(engine_core_crash_probe CrashProbe.cpp)
    engine_target_utf8(engine_core_crash_probe)
    target_link_libraries(engine_core_crash_probe PRIVATE mir::core_base)
    target_sources(engine_core_base_tests PRIVATE CrashHandler.tests.cpp)
    add_dependencies(engine_core_base_tests engine_core_crash_probe)
    target_compile_definitions(engine_core_base_tests
        PRIVATE MIR_CRASH_PROBE_PATH="$<TARGET_FILE:engine_core_crash_probe>")
endif()

engine_add_module_tests(core
    Archetype.tests.cpp
    Query.tests.cpp
    CommandBuffer.tests.cpp
    System.tests.cpp
    Stages.tests.cpp
    Requests.tests.cpp
    ModuleUnwind.tests.cpp
    FixedTimestep.tests.cpp
    GroundQuery.tests.cpp
    Transform.tests.cpp
    TransformHierarchy.tests.cpp
    Resources.tests.cpp
    ComponentHooks.tests.cpp
    Entity.tests.cpp
    ComponentFieldChanges.tests.cpp
    ComponentRegistry.tests.cpp
    EntityPresetRegistry.tests.cpp
    RuntimeQuery.tests.cpp
)

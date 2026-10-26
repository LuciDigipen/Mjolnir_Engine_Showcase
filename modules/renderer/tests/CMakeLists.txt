engine_add_module_tests(renderer
    AntiAliasingEffect.tests.cpp
    BloomEffect.tests.cpp
    CameraMath.tests.cpp
    DepthOfFieldEffect.tests.cpp
    FilmEffect.tests.cpp
    Material.tests.cpp
    MaterialRecordTable.tests.cpp
    MeshStreaming.tests.cpp
    FogEffect.tests.cpp
    HdrEffect.tests.cpp
    MotionBlurEffect.tests.cpp
    RendererCapabilities.tests.cpp
    TextureTable.tests.cpp
    PostEffectRegistry.tests.cpp
    PostProcessingSettings.tests.cpp
    PostProcessingState.tests.cpp
    RenderSettings.tests.cpp
    RenderSystem.tests.cpp
    RtInstanceLayout.tests.cpp
    Shapes.tests.cpp
    ViewSubmission.tests.cpp
)

file(TO_CMAKE_PATH "${SLANGC_EXECUTABLE}" MIR_SLANGC_EXECUTABLE_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/shaders/raytrace.slang" MIR_RAYTRACE_SLANG_PATH)
target_compile_definitions(engine_renderer_tests PRIVATE
    MIR_SLANGC_EXECUTABLE="${MIR_SLANGC_EXECUTABLE_PATH}"
    MIR_RAYTRACE_SLANG="${MIR_RAYTRACE_SLANG_PATH}")

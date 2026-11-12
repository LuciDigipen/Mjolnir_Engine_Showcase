engine_add_module_tests(renderer
    ActiveCamera.tests.cpp
    AntiAliasingEffect.tests.cpp
    BloomEffect.tests.cpp
    CameraCut.tests.cpp
    CameraGizmo.tests.cpp
    CameraMath.tests.cpp
    DepthOfFieldEffect.tests.cpp
    DescriptorLayoutLimits.tests.cpp
    DrawRecordAllocator.tests.cpp
    DrawRecordLayout.tests.cpp
    FilmEffect.tests.cpp
    GpuRecords.tests.cpp
    Material.tests.cpp
    MaterialRecordTable.tests.cpp
    MaterialReflection.tests.cpp
    MaterialResolution.tests.cpp
    SurfaceMaterialCatalog.tests.cpp
    SurfaceMaterialLibrary.tests.cpp
    SurfaceMaterialAsync.tests.cpp
    SurfaceMaterialLayout.tests.cpp
    MeshAssetDecoder.tests.cpp
    MeshLoadTracker.tests.cpp
    TextureLoadTracker.tests.cpp
    MeshResolveSystem.tests.cpp
    MeshStreaming.tests.cpp
    FogEffect.tests.cpp
    HdrEffect.tests.cpp
    MotionBlurEffect.tests.cpp
    RendererCapabilities.tests.cpp
    TextureTable.tests.cpp
    PostEffectRegistry.tests.cpp
    PostProcessingSettings.tests.cpp
    PostProcessingState.tests.cpp
    RayFacing.tests.cpp
    RenderSettings.tests.cpp
    RenderSystem.tests.cpp
    RtInstanceLayout.tests.cpp
    ShaderLibrary.tests.cpp
    ShaderCompiler.tests.cpp
    ShaderReload.tests.cpp
    Shapes.tests.cpp
    TextureMips.tests.cpp
    ViewSubmission.tests.cpp
)

file(TO_CMAKE_PATH "${SLANGC_EXECUTABLE}" MIR_SLANGC_EXECUTABLE_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/shaders/raytrace.slang" MIR_RAYTRACE_SLANG_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/shaders/SurfaceLayout.slang" MIR_SURFACE_LAYOUT_SLANG_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/materials/default.slang" MIR_DEFAULT_MATERIAL_SLANG_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/materials/default.material.json"
    MIR_DEFAULT_MATERIAL_JSON_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/games/TerrainCheck/assets/materials/icelandicSand.slang"
    MIR_TERRAIN_MATERIAL_SLANG_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/games/TerrainCheck/assets/materials/icelandicSand.material.json"
    MIR_TERRAIN_MATERIAL_JSON_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/shaders/SurfaceTerrainForward.slang"
    MIR_TERRAIN_FORWARD_SLANG_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/shaders/SurfaceTerrainRt.slang"
    MIR_TERRAIN_RT_SLANG_PATH)
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/shader_build/shader_manifest.json" MIR_SHADER_MANIFEST_PATH)
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/shader_build/surface_materials.json" MIR_SURFACE_CATALOG_PATH)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_BINARY_DIR}" MIR_RENDERER_TEST_DIR_PATH)
target_compile_definitions(engine_renderer_tests PRIVATE
    MIR_SLANGC_EXECUTABLE="${MIR_SLANGC_EXECUTABLE_PATH}"
    MIR_RAYTRACE_SLANG="${MIR_RAYTRACE_SLANG_PATH}"
    MIR_SURFACE_LAYOUT_SLANG="${MIR_SURFACE_LAYOUT_SLANG_PATH}"
    MIR_DEFAULT_MATERIAL_SLANG="${MIR_DEFAULT_MATERIAL_SLANG_PATH}"
    MIR_DEFAULT_MATERIAL_JSON="${MIR_DEFAULT_MATERIAL_JSON_PATH}"
    MIR_TERRAIN_MATERIAL_SLANG="${MIR_TERRAIN_MATERIAL_SLANG_PATH}"
    MIR_TERRAIN_MATERIAL_JSON="${MIR_TERRAIN_MATERIAL_JSON_PATH}"
    MIR_TERRAIN_FORWARD_SLANG="${MIR_TERRAIN_FORWARD_SLANG_PATH}"
    MIR_TERRAIN_RT_SLANG="${MIR_TERRAIN_RT_SLANG_PATH}"
    MIR_SHADER_MANIFEST="${MIR_SHADER_MANIFEST_PATH}"
    MIR_SURFACE_CATALOG="${MIR_SURFACE_CATALOG_PATH}"
    MIR_RENDERER_TEST_DIR="${MIR_RENDERER_TEST_DIR_PATH}"
    MIR_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

# the mesh load tests build a JobExecutor
target_link_libraries(engine_renderer_tests PRIVATE mir::jobs)

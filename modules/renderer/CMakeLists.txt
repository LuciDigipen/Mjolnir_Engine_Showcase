engine_add_module(renderer)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/DlssConfig.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated_include/renderer/DlssConfig.h
    @ONLY)
target_include_directories(engine_renderer PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated_include)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/RtxgiConfig.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated_include/renderer/RtxgiConfig.h
    @ONLY)

# volk: function pointers w/o boiler plate
find_package(volk CONFIG REQUIRED)
# VMA : vulkan memory manager
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Stb REQUIRED)
target_include_directories(engine_renderer PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(engine_renderer
    PUBLIC volk::volk_headers
            mir::assets
            mir::core
            glm::glm
            nlohmann_json::nlohmann_json
    PRIVATE GPUOpen::VulkanMemoryAllocator
            imgui::imgui
            mir::serial
)

# compile volk ourself instead of linking prebuilt library
# due to platform definition issues
get_target_property(_volk_includes volk::volk_headers INTERFACE_INCLUDE_DIRECTORIES)
find_file(VOLK_SOURCE volk.c PATHS ${_volk_includes} NO_DEFAULT_PATH REQUIRED)
target_sources(engine_renderer PRIVATE
    ${VOLK_SOURCE}
    src/renderer/vulkan/FxCompositionPass.cpp
)

# for volk macro definition
engine_target_vulkan_platform_defines(engine_renderer)

# makes VMA compatible with volk
target_compile_definitions(engine_renderer PRIVATE
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    VMA_VULKAN_VERSION=1004000
)

target_sources(engine_renderer PRIVATE
    src/renderer/ActiveCamera.cpp
    src/renderer/AntiAliasingEffect.cpp
    src/renderer/BloomEffect.cpp
    src/renderer/CameraCut.cpp
    src/renderer/CameraMath.cpp
    src/renderer/ComponentRegistration.cpp
    src/renderer/DepthOfFieldEffect.cpp
    src/renderer/DescriptorLayoutLimits.cpp
    src/renderer/FilmEffect.cpp
    src/renderer/MeshResolveSystem.cpp
    src/renderer/MeshStreaming.cpp
    src/renderer/FogEffect.cpp
    src/renderer/HdrEffect.cpp
    src/renderer/Material.cpp
    src/renderer/MaterialRecordTable.cpp
    src/renderer/MaterialReflection.cpp
    src/renderer/MaterialResolution.cpp
    src/renderer/SurfaceMaterialCatalog.cpp
    src/renderer/SurfaceMaterialLibrary.cpp
    src/renderer/MotionBlurEffect.cpp
    src/renderer/PostEffectRegistry.cpp
    src/renderer/PostProcessingSettings.cpp
    src/renderer/PostProcessingState.cpp
    src/renderer/Renderer.cpp
    src/renderer/RendererCapabilities.cpp
    src/renderer/RendererModule.cpp
    src/renderer/RenderSettings.cpp
    src/renderer/Shapes.cpp
    src/renderer/TextureTable.cpp
    src/renderer/ViewSubmission.cpp
    src/renderer/VulkanRenderer.cpp
    src/renderer/VulkanRendererFrame.cpp
    src/renderer/vulkan/Bloom.cpp
    src/renderer/vulkan/CameraBuffer.cpp
    src/renderer/vulkan/DebugDraw.cpp
    src/renderer/vulkan/DepthOfField.cpp
    src/renderer/vulkan/Device.cpp
    src/renderer/vulkan/FrameCapture.cpp
    src/renderer/vulkan/FrameSync.cpp
    src/renderer/vulkan/Fxaa.cpp
    src/renderer/vulkan/GpuTiming.cpp
    src/renderer/vulkan/ImGuiOverlay.cpp
    src/renderer/vulkan/MaterialStore.cpp
    src/renderer/vulkan/MaterialTable.cpp
    src/renderer/vulkan/MeshUploader.cpp
    src/renderer/vulkan/ModelDescriptors.cpp
    src/renderer/vulkan/MotionBlur.cpp
    src/renderer/vulkan/PostProcess.cpp
    src/renderer/vulkan/PostProcessingStack.cpp
    src/renderer/vulkan/RayTracer.cpp
    src/renderer/vulkan/RasterSurfaces.cpp
    src/renderer/vulkan/SsgiTrace.cpp
    src/renderer/vulkan/ResourceStore.cpp
    src/renderer/vulkan/ShadowMaps.cpp
    src/renderer/vulkan/Swapchain.cpp
    src/renderer/vulkan/VelocityPass.cpp
    src/renderer/vulkan/VmaImpl.cpp
    src/renderer/vulkan/VulkanUtil.cpp
)
if(MIR_WITH_DLSS)
    include(FindNGX)
    target_sources(engine_renderer PRIVATE src/renderer/vulkan/DlssRayReconstruction.cpp)
    target_link_libraries(engine_renderer PRIVATE NGX::NGX)
endif()

if(MIR_WITH_RTXGI)
    include(Rtxgi)
    target_sources(engine_renderer PRIVATE src/renderer/vulkan/Ddgi.cpp)
    target_link_libraries(engine_renderer PRIVATE RTXGI::DDGI)
endif()

if(ENGINE_FAMILY_DESKTOP)
    if(ENGINE_PLATFORM STREQUAL "Windows")
    target_sources(engine_renderer PRIVATE src/renderer/vulkan/VulkanSurfaceWin32.cpp)
    elseif(ENGINE_PLATFORM STREQUAL "Linux")
    target_sources(engine_renderer PRIVATE src/renderer/vulkan/VulkanSurfaceLinux.cpp)
    else()
        message(FATAL_ERROR "No native window backend for ${ENGINE_PLATFORM}")
    endif()
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

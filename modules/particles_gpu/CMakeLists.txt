engine_add_module(particles_gpu)

find_package(VulkanMemoryAllocator CONFIG REQUIRED)
# PUBLIC because the FX registration API uses renderer and core types.
target_link_libraries(engine_particles_gpu PUBLIC mir::renderer PRIVATE GPUOpen::VulkanMemoryAllocator)
target_compile_definitions(engine_particles_gpu PRIVATE VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
target_sources(engine_particles_gpu PRIVATE
    src/particles_gpu/ParticlesGpuModule.cpp
    src/particles_gpu/ParticleDataset.cpp
    src/particles_gpu/ParticleDispatch.cpp
    src/particles_gpu/ParticleSpriteRenderer.cpp
    src/particles_gpu/ParticleVulkan.cpp)

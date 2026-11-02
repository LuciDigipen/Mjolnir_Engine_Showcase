/**
 * @file MaterialStore.cpp
 * @author Sumin Park
 * @brief Material pipelines, set 0 and the shared pipeline layout.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "MaterialStore.h"
#include "Device.h"
#include "MaterialTable.h"
#include "ResourceStore.h"
#include "RasterSurfaces.h"
#include "Slots.h"
#include "VulkanUtil.h"
#include "VulkanConstants.h"
#include "VulkanPushConstants.h"

#include <renderer/MaterialResolution.h>
#include <renderer/ShaderLibrary.h>

#include <core/fs/Paths.h>
#include <core/fs/FileIO.h>

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mir::vk
{
    namespace
    {
        constexpr std::string_view kModelShaderName = "model";
        constexpr uint32_t kRecordRefBinding = 2;

        std::optional<std::vector<std::byte>> BuildMigrationDefaults(const MaterialLayout &candidateLayout,
            std::string_view parametersJson, std::string *error)
        {
            // A field the accepted layout lacks takes the catalog's authored
            // value, then the typed default; names it already holds are ignored.
            const std::optional<std::string> defaults =
                CompleteMaterialParameters(candidateLayout, "{}", parametersJson, error);
            if (!defaults)
                return std::nullopt;
            return PackMaterialRecord(candidateLayout, *defaults, error);
        }

        std::optional<MaterialLayout> LoadLayout(const SurfaceMaterialRegistration &registration,
            std::string *error)
        {
            const std::filesystem::path path = ShaderPath(registration.reflectionFile);
            const std::optional<std::string> text = ReadText(path);
            if (!text)
            {
                if (error != nullptr)
                    *error = "material reflection is missing: " + path.string();
                return std::nullopt;
            }
            return ParseMaterialLayout(*text, error);
        }
    }

    bool MaterialStore::Initialize(Device &device, ResourceStore &resources, MaterialTable *table,
        VkFormat colorFormat, const std::array<VkDescriptorSetLayout, 3> &sharedLayouts)
    {
        mDevice = &device;
        mTable = table;
        mColorFormat = colorFormat;

        std::string catalogError;
        const auto catalog = LoadSurfaceMaterialCatalog(&catalogError);
        if (!catalog)
        {
            MIR_LOG_CRITICAL("MaterialStore: {}", catalogError);
            return false;
        }
        mSurfaceMaterials = *catalog;

        const uint64_t alignment = std::max<uint64_t>(
            mDevice->Properties().limits.minUniformBufferOffsetAlignment, 1);
        mRecordRefStride = static_cast<uint32_t>(
            (sizeof(MaterialRecordRef) + alignment - 1) / alignment * alignment);
        mRecordRefFrameStride = mRecordRefStride * kMaxMaterials;

        if (!CreatePipelineLayout(sharedLayouts))
            return false;

        if (!CreateSkyPipeline(sharedLayouts[0]))
            return false;

        if (mTable != nullptr && !CreateRecordRefBuffer())
            return false;

        mDefaultMaterial = CreateMaterial(MaterialDesc{}, resources, colorFormat);
        if (mDefaultMaterial.IsNull())
            return false;

        // No culling: one error material stands in for both a primitive and its mirrored twin.
        mErrorMaterial = CreateMaterial(MaterialDesc{ .shaderName = "error", .cullMode = VK_CULL_MODE_NONE },
            resources, colorFormat);
        if (mErrorMaterial.IsNull())
            return false;

        mDevice->NameObject(VK_OBJECT_TYPE_PIPELINE,
            reinterpret_cast<uint64_t>(mMaterials[mDefaultMaterial.mIndex].mPipeline), "Default material pipeline");
        mDevice->NameObject(VK_OBJECT_TYPE_PIPELINE,
            reinterpret_cast<uint64_t>(mMaterials[mErrorMaterial.mIndex].mPipeline), "Error material pipeline");
        return true;
    }

    void MaterialStore::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        // Before the pool: frees each material's set into it.
        DestroyMaterials();
        mDefaultMaterial = kNullMaterial;
        mErrorMaterial = kNullMaterial;
        DestroyRecordRefBuffer();

        if (mDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice->Handle(), mDescriptorPool, nullptr);
            mDescriptorPool = VK_NULL_HANDLE;
        }

        if (mSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mSetLayout, nullptr);
            mSetLayout = VK_NULL_HANDLE;
        }

        if (mPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice->Handle(), mPipelineLayout, nullptr);
            mPipelineLayout = VK_NULL_HANDLE;
        }

        if (mSkyPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mSkyPipeline, nullptr);
            mSkyPipeline = VK_NULL_HANDLE;
        }

        if (mSkyPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice->Handle(), mSkyPipelineLayout, nullptr);
            mSkyPipelineLayout = VK_NULL_HANDLE;
        }

        mTable = nullptr;
    }

    bool MaterialStore::CreatePipelineLayout(const std::array<VkDescriptorSetLayout, 3> &sharedLayouts)
    {
        // binding 0 = base color, binding 1 = normal map, binding 2 = the
        // record reference. Legacy shaders still sample 0 and 1; shared-table
        // shaders find their bytes through 2.
        std::array<VkDescriptorSetLayoutBinding, 3> setBindings{ {
            { .binding = 0,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = kRecordRefBinding,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT } } };
        const uint32_t setBindingCount = mTable != nullptr ? static_cast<uint32_t>(setBindings.size()) : 2u;

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = setBindingCount,
            .pBindings = setBindings.data() };

        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &setLayoutInfo, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipelineLayout: vkCreateDescriptorSetLayout failed");
            return false;
        }

        // One set per material. A model primitive eats two (front and mirrored
        // front face), so this is not as generous as it looks.
        std::array<VkDescriptorPoolSize, 2> poolSizes{ {
            { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = kMaxMaterials * kMaterialSetCombinedImages },
            { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
              .descriptorCount = kMaxMaterials * kMaterialSetRecordUniformBuffers } } };
        const uint32_t poolSizeCount = mTable != nullptr ? static_cast<uint32_t>(poolSizes.size()) : 1u;

        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = kMaxMaterials,
            .poolSizeCount = poolSizeCount,
            .pPoolSizes = poolSizes.data() };

        if (vkCreateDescriptorPool(mDevice->Handle(), &poolInfo, nullptr, &mDescriptorPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipelineLayout: vkCreateDescriptorPool failed");
            return false;
        }

        const VkPushConstantRange pushConstantRange{
            .stageFlags = kRasterPushConstantStages,
            .offset = 0,
            .size = sizeof(RasterPushConstants) };

        // set 0 = material (textures and record reference), set 1 = per-view camera UBO,
        // set 2 = per-draw model data, set 3 = shadow maps, set 4 = shared table.
        static_assert(kRasterPipelineSetCount == 5);
        const std::array<VkDescriptorSetLayout, kRasterPipelineSetCount> setLayouts{
            mSetLayout, sharedLayouts[0], sharedLayouts[1], sharedLayouts[2],
            mTable != nullptr ? mTable->SetLayout() : VK_NULL_HANDLE };
        const uint32_t setLayoutCount = mTable != nullptr
            ? kRasterPipelineSetCount
            : kRasterPipelineSetCount - 1;

        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = setLayoutCount,
            .pSetLayouts = setLayouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        if (vkCreatePipelineLayout(mDevice->Handle(), &layoutInfo, nullptr, &mPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreatePipelineLayout: vkCreatePipelineLayout failed");
            return false;
        }

        return true;
    }

    VkPipeline MaterialStore::CreateGraphicsPipeline(const PipelineState &state, VkFormat colorFormat, bool surfaces)
    {
        const std::string shaderName = std::string(state.shaderName) + (surfaces ? "Surface" : "");
        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath(shaderName + ".vertexMain.spv"));
        VkShaderModule fragModule = CreateShaderModule(mDevice->Handle(), ShaderPath(shaderName + ".fragmentMain.spv"));

        if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE)
        {
            if (vertModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
            if (fragModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);
            return VK_NULL_HANDLE;
        }

        const VkPipelineShaderStageCreateInfo stages[]{
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = vertModule,
             .pName = "vertexMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = fragModule,
             .pName = "fragmentMain" } };

        const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = state.topology,
            .primitiveRestartEnable = VK_FALSE };

        const VkPipelineViewportStateCreateInfo viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = nullptr,
            .scissorCount = 1,
            .pScissors = nullptr };

        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE };

        const std::array surfaceBlends{ state.blend, state.blend, state.blend, state.blend };
        const VkPipelineColorBlendStateCreateInfo colorBlend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = VK_FALSE,
            .attachmentCount = surfaces ? kRasterSurfaceCount : 1,
            .pAttachments = surfaces ? surfaceBlends.data() : &state.blend };

        const VkDynamicState dynamicStates[]{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR };

        const VkPipelineDynamicStateCreateInfo dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamicStates };

        constexpr std::array surfaceFormats{ kRasterSurfaceFormat, kRasterSurfaceFormat, kRasterSurfaceFormat, kRasterSurfaceFormat };
        const VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = surfaces ? kRasterSurfaceCount : 1,
            .pColorAttachmentFormats = surfaces ? surfaceFormats.data() : &colorFormat,
            .depthAttachmentFormat = kDepthFormat };

        const VkGraphicsPipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &renderingInfo,
            .stageCount = 2,
            .pStages = stages,
            .pVertexInputState = &state.vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &state.rasterizer,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &state.depthStencil,
            .pColorBlendState = &colorBlend,
            .pDynamicState = &dynamicState,
            .layout = state.layout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0 };

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateGraphicsPipelines(
            mDevice->Handle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

        vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);

        if (result != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateGraphicsPipeline: vkCreateGraphicsPipelines failed for {}: {}",
                shaderName, static_cast<int>(result));
            return VK_NULL_HANDLE;
        }

        return pipeline;
    }

    VkPipeline MaterialStore::BuildPipeline(const MaterialDesc &desc, VkFormat colorFormat, bool surfaces)
    {
        const VkVertexInputBindingDescription vertexBinding{
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

        const std::array<VkVertexInputAttributeDescription, 6> vertexAttributes{ { { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, pos) },
                                                                                 { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, color) },
                                                                                 { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal) },
                                                                                 { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, uv) },
                                                                                 { .location = 4, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(Vertex, alpha) },
                                                                                 { .location = 5, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, tangent) } } };

        const bool blend = desc.alphaMode == MaterialAlphaMode::Blend;

        const PipelineState state{
            .shaderName = desc.shaderName,
            .vertexInput{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                .vertexBindingDescriptionCount = 1,
                .pVertexBindingDescriptions = &vertexBinding,
                // Only the model shader reads uv, alpha and tangent. Declaring
                // them for the others is legal but makes the validation layer
                // complain about attributes no shader consumes.
                .vertexAttributeDescriptionCount =
                    desc.shaderName == kModelShaderName || desc.surfaceMaterial
                    ? static_cast<uint32_t>(vertexAttributes.size())
                    : 3u,
                .pVertexAttributeDescriptions = vertexAttributes.data() },
            .topology = desc.topology,
            .rasterizer{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .depthClampEnable = VK_FALSE,
                .rasterizerDiscardEnable = VK_FALSE,
                .polygonMode = desc.polygonMode,
                .cullMode = desc.cullMode,
                .frontFace = desc.frontFace,
                .depthBiasEnable = VK_FALSE,
                .lineWidth = 1.0f },
            .blend{
                .blendEnable = blend ? VK_TRUE : VK_FALSE,
                .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .alphaBlendOp = VK_BLEND_OP_ADD,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT },
            .depthStencil{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                .depthTestEnable = VK_TRUE,
                .depthWriteEnable = blend || surfaces ? VK_FALSE : VK_TRUE,
                .depthCompareOp = surfaces ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_GREATER,
                .depthBoundsTestEnable = VK_FALSE,
                .stencilTestEnable = VK_FALSE },
            .layout = mPipelineLayout };

        return CreateGraphicsPipeline(state, colorFormat, surfaces);
    }

    bool MaterialStore::CreateSkyPipeline(VkDescriptorSetLayout cameraSetLayout)
    {
        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &cameraSetLayout };

        if (vkCreatePipelineLayout(mDevice->Handle(), &layoutInfo, nullptr, &mSkyPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateSkyPipeline: vkCreatePipelineLayout failed");
            return false;
        }

        mSkyPipeline = BuildSkyPipeline();
        if (mSkyPipeline == VK_NULL_HANDLE)
            return false;

        mDevice->NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(mSkyPipeline), "Sky pipeline");
        return true;
    }

    VkPipeline MaterialStore::BuildSkyPipeline()
    {
        // No vertex input: the shader builds a fullscreen triangle. Depth GEQUAL
        // with no write passes only where nothing drew, at the reverse-Z clear of 0.
        const PipelineState state{
            .shaderName = "sky",
            .vertexInput{ .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO },
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .rasterizer{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .depthClampEnable = VK_FALSE,
                .rasterizerDiscardEnable = VK_FALSE,
                .polygonMode = VK_POLYGON_MODE_FILL,
                .cullMode = VK_CULL_MODE_NONE,
                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                .depthBiasEnable = VK_FALSE,
                .lineWidth = 1.0f },
            .blend{
                .blendEnable = VK_FALSE,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT },
            .depthStencil{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                .depthTestEnable = VK_TRUE,
                .depthWriteEnable = VK_FALSE,
                .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
                .depthBoundsTestEnable = VK_FALSE,
                .stencilTestEnable = VK_FALSE },
            .layout = mSkyPipelineLayout };

        return CreateGraphicsPipeline(state, mColorFormat);
    }

    MaterialHandle MaterialStore::CreateMaterial(MaterialDesc desc, ResourceStore &resources, VkFormat colorFormat)
    {
        std::string resolutionError;
        const auto resolved = ResolveMaterialDesc(desc, mSurfaceMaterials, mTable != nullptr, &resolutionError);
        if (!resolved)
        {
            MIR_LOG_ERROR("CreateMaterial: {}", resolutionError);
            return kNullMaterial;
        }
        desc = *resolved;

        const VkPipeline pipeline = BuildPipeline(desc, colorFormat);
        if (pipeline == VK_NULL_HANDLE)
            return kNullMaterial;

        const TextureHandle baseColorHandle = resources.FindTexture(desc.baseColorTexture) != nullptr
            ? desc.baseColorTexture
            : resources.DefaultWhiteTexture();
        const GpuTexture *const baseColor = resources.FindTexture(baseColorHandle);

        const TextureHandle normalHandle = resources.FindTexture(desc.normalTexture) != nullptr
            ? desc.normalTexture
            : resources.DefaultNormalTexture();
        const GpuTexture *const normal = resources.FindTexture(normalHandle);

        if (baseColor == nullptr || normal == nullptr)
        {
            MIR_LOG_CRITICAL("CreateMaterial: default textures unavailable");
            vkDestroyPipeline(mDevice->Handle(), pipeline, nullptr);
            return kNullMaterial;
        }

        // Shared with every other material that asked for the same state, so
        // this is not destroyed with the material.
        uint32_t samplerSlot = 0;
        const VkSampler sampler = resources.AcquireSampler(desc.sampler, &samplerSlot);
        if (sampler == VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), pipeline, nullptr);
            return kNullMaterial;
        }

        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        const VkDescriptorSetAllocateInfo setAllocInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = mDescriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &mSetLayout };

        if (vkAllocateDescriptorSets(mDevice->Handle(), &setAllocInfo, &descriptorSet) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateMaterial: descriptor set allocation failed");
            vkDestroyPipeline(mDevice->Handle(), pipeline, nullptr);
            return kNullMaterial;
        }

        GpuMaterial material;
        material.mPipeline = pipeline;
        material.mDescriptorSet = descriptorSet;
        material.mParameters = glm::vec4{ static_cast<float>(desc.alphaMode), desc.alphaCutoff,
            desc.cullMode == VK_CULL_MODE_NONE ? 1.0f : 0.0f, 0.0f };
        material.mModelShader = desc.shaderName == kModelShaderName;
        material.mSurfaceMaterial = desc.surfaceMaterial;
        material.mUnlit = desc.shaderName == "unlit";
        material.mCullMode = desc.cullMode;
        material.mFrontFace = desc.frontFace;
        material.mBaseColorView = baseColor->mView;
        material.mSampler = sampler;
        material.mBaseColorSlot = resources.TextureSlot(baseColorHandle);
        material.mSamplerSlot = samplerSlot;
        material.mError = desc.shaderName == "error";
        material.mDesc = desc;
        material.mDesc.record = {};

        const auto rollback = [&]
            {
                if (mTable != nullptr && !material.mRecordId.IsNull())
                    mTable->ReleaseRecord(material.mRecordId);
                if (descriptorSet != VK_NULL_HANDLE)
                    vkFreeDescriptorSets(mDevice->Handle(), mDescriptorPool, 1, &descriptorSet);
                vkDestroyPipeline(mDevice->Handle(), pipeline, nullptr);
            };

        std::span<const std::byte> record = desc.record;
        if (desc.surfaceMaterial)
        {
            if (mTable == nullptr)
            {
                MIR_LOG_ERROR("CreateMaterial: surface materials require the shared material tables");
                rollback();
                return kNullMaterial;
            }

            const SurfaceMaterialRegistration *const registration =
                mSurfaceMaterials.Find(desc.shaderName, desc.surfaceAsset);
            if (registration == nullptr)
            {
                MIR_LOG_CRITICAL("CreateMaterial: no surface material registration for '{}'",
                    desc.surfaceAsset.empty() ? desc.shaderName : desc.surfaceAsset);
                rollback();
                return kNullMaterial;
            }

            const uint32_t index = mSurfaceMaterials.IndexOf(registration);
            if (index == UINT32_MAX)
            {
                MIR_LOG_CRITICAL("CreateMaterial: surface material registration is stale");
                rollback();
                return kNullMaterial;
            }
            material.mRayTracingSbtOffset = SurfaceMaterialCatalog::HitGroupRecordOffset(index);

            if (record.empty())
            {
                std::string packError;
                const auto packed = PackSurfaceMaterialRecord(*registration, &packError);
                if (!packed)
                {
                    MIR_LOG_CRITICAL("CreateMaterial: {}", packError);
                    rollback();
                    return kNullMaterial;
                }
                material.mRecordBytes = *packed;
                record = material.mRecordBytes;
            }

            std::string layoutError;
            const auto layout = LoadLayout(*registration, &layoutError);
            if (!layout)
            {
                MIR_LOG_CRITICAL("CreateMaterial: {}", layoutError);
                rollback();
                return kNullMaterial;
            }
            material.mAcceptedLayout = *layout;
            material.mHasAcceptedLayout = true;
        }

        VkDescriptorBufferInfo recordRefInfo{};
        if (mTable != nullptr)
        {
            material.mRecordId = mTable->CreateRecord(record, desc.recordAlignment);
            if (material.mRecordId.IsNull())
            {
                MIR_LOG_CRITICAL("CreateMaterial: material record table is full");
                rollback();
                return kNullMaterial;
            }

            material.mRecordOffset = mTable->RecordOffset(material.mRecordId);
            material.mCurrentRecordBytes.assign(record.begin(), record.end());
            recordRefInfo = VkDescriptorBufferInfo{
                .buffer = mRecordRefBuffer,
                .offset = 0,
                .range = sizeof(MaterialRecordRef) };
        }

        const VkDescriptorImageInfo baseColorInfo{
            .sampler = sampler,
            .imageView = baseColor->mView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo normalInfo{
            .sampler = sampler,
            .imageView = normal->mView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        std::array<VkWriteDescriptorSet, 3> writes{ {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = descriptorSet,
             .dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &baseColorInfo },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = descriptorSet,
             .dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &normalInfo },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = descriptorSet,
             .dstBinding = kRecordRefBinding,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
             .pBufferInfo = &recordRefInfo } } };
        const uint32_t writeCount = mTable != nullptr ? static_cast<uint32_t>(writes.size()) : 2u;

        vkUpdateDescriptorSets(mDevice->Handle(), writeCount, writes.data(), 0, nullptr);

        const uint32_t index = PlaceSlot(mMaterials, mFreeIndices, material);
        mMaterials[index].mMaterialIndex = index;
        if (mTable != nullptr)
            WriteRecordRefEverywhere(index);

        MIR_LOG_INFO("Material created: {} (index {})", desc.shaderName, index);
        return MaterialHandle{ index, mMaterials[index].mGeneration };
    }

    bool MaterialStore::CreateRecordRefBuffer()
    {
        const VkDeviceSize bytes = VkDeviceSize{ kFramesInFlight } * mRecordRefFrameStride;
        VmaAllocationInfo info{};
        const VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bytes, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        const VmaAllocationCreateInfo allocationInfo{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        if (vmaCreateBuffer(mDevice->Allocator(), &bufferInfo, &allocationInfo,
            &mRecordRefBuffer, &mRecordRefAllocation, &info) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateRecordRefBuffer: allocation failed");
            return false;
        }

        mRecordRefMapped = static_cast<std::byte *>(info.pMappedData);
        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mRecordRefBuffer),
            "Material record references");
        return true;
    }

    void MaterialStore::DestroyRecordRefBuffer()
    {
        if (mRecordRefBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mDevice->Allocator(), mRecordRefBuffer, mRecordRefAllocation);
            mRecordRefBuffer = VK_NULL_HANDLE;
            mRecordRefAllocation = VK_NULL_HANDLE;
            mRecordRefMapped = nullptr;
        }
    }

    void MaterialStore::WriteRecordRef(uint32_t frame, uint32_t material)
    {
        if (mRecordRefMapped == nullptr || material >= mMaterials.size())
            return;

        const GpuMaterial &entry = mMaterials[material];
        if (entry.mPipeline == VK_NULL_HANDLE)
            return;

        const MaterialRecordRef ref{
            .mOffset = entry.mRecordOffset,
            .mSize = mTable != nullptr && !entry.mRecordId.IsNull() ? mTable->RecordSize(entry.mRecordId) : 0 };
        std::memcpy(mRecordRefMapped + RecordRefDynamicOffset(frame, material), &ref, sizeof(ref));
    }

    void MaterialStore::WriteRecordRefEverywhere(uint32_t material)
    {
        if (mRecordRefMapped == nullptr)
            return;

        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            WriteRecordRef(frame, material);
        vmaFlushAllocation(mDevice->Allocator(), mRecordRefAllocation, 0, VK_WHOLE_SIZE);
    }

    void MaterialStore::RefreshRecordRefs(uint32_t frame)
    {
        if (mRecordRefMapped == nullptr)
            return;

        for (uint32_t i = 0; i < mMaterials.size(); ++i)
            WriteRecordRef(frame, i);

        const VkDeviceSize offset = VkDeviceSize{ frame } * mRecordRefFrameStride;
        vmaFlushAllocation(mDevice->Allocator(), mRecordRefAllocation, offset, mRecordRefFrameStride);
    }

    void MaterialStore::FreeMaterial(GpuMaterial &material)
    {
        // The sampler belongs to the resource store's cache, not to the material.
        if (mTable != nullptr && !material.mRecordId.IsNull())
            mTable->ReleaseRecord(material.mRecordId);
        if (material.mDescriptorSet != VK_NULL_HANDLE)
            vkFreeDescriptorSets(mDevice->Handle(), mDescriptorPool, 1, &material.mDescriptorSet);
        if (material.mPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(mDevice->Handle(), material.mPipeline, nullptr);
        if (material.mSurfacePipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(mDevice->Handle(), material.mSurfacePipeline, nullptr);
    }

    bool MaterialStore::DestroyMaterial(MaterialHandle handle)
    {
        if (handle == mDefaultMaterial || handle == mErrorMaterial || FindMaterial(handle) == nullptr)
            return false;

        GpuMaterial &material = mMaterials[handle.mIndex];
        FreeMaterial(material);
        RetireSlot(material);

        mFreeIndices.push_back(handle.mIndex);
        return true;
    }

    bool MaterialStore::UpdateRecord(MaterialHandle handle, std::span<const std::byte> bytes)
    {
        GpuMaterial *const material = const_cast<GpuMaterial *>(FindMaterial(handle));
        if (material == nullptr || mTable == nullptr || material->mRecordId.IsNull())
            return false;
        if (material->mSurfaceMaterial && mRecordValidator)
        {
            const auto *registration = mSurfaceMaterials.Find(material->mDesc.shaderName, material->mDesc.surfaceAsset);
            std::string error;
            if (registration == nullptr || !material->mHasAcceptedLayout ||
                !mRecordValidator(registration->frontend, material->mAcceptedLayout, bytes, error))
                return false;
        }
        std::vector<std::byte> current(bytes.begin(), bytes.end());
        if (!mTable->UpdateRecord(material->mRecordId, bytes))
            return false;

        material->mCurrentRecordBytes = std::move(current);
        return true;
    }

    bool MaterialStore::AcceptedRecord(MaterialHandle handle, MaterialLayout &layout,
        std::span<const std::byte> &bytes) const
    {
        const GpuMaterial *const material = FindMaterial(handle);
        if (material == nullptr || !material->mSurfaceMaterial || !material->mHasAcceptedLayout ||
            material->mCurrentRecordBytes.empty())
            return false;
        layout = material->mAcceptedLayout;
        bytes = material->mCurrentRecordBytes;
        return true;
    }

    bool MaterialStore::PrepareRecordBatch(std::span<const MaterialRecordUpdate> updates, std::string *error)
    {
        DiscardRecordBatch();
        if (mTable == nullptr)
        {
            if (error != nullptr)
                *error = "the shared material tables are unavailable";
            return false;
        }

        mPreparedRecords.reserve(updates.size());
        for (const MaterialRecordUpdate &update : updates)
        {
            const GpuMaterial *const material = FindMaterial(update.mMaterial);
            if (material == nullptr || material->mRecordId.IsNull() || !material->mSurfaceMaterial ||
                !material->mHasAcceptedLayout)
            {
                if (error != nullptr)
                    *error = "a material record update names a material that has no shared record";
                DiscardRecordBatch();
                return false;
            }
            if (update.mBytes.size() != material->mAcceptedLayout.size)
            {
                if (error != nullptr)
                    *error = "a material record update does not cover its accepted layout";
                DiscardRecordBatch();
                return false;
            }
            if (mRecordValidator)
            {
                const SurfaceMaterialRegistration *const registration =
                    mSurfaceMaterials.Find(material->mDesc.shaderName, material->mDesc.surfaceAsset);
                std::string validationError;
                if (registration == nullptr ||
                    !mRecordValidator(registration->frontend, material->mAcceptedLayout, update.mBytes, validationError))
                {
                    if (error != nullptr)
                        *error = registration == nullptr ? "a material record update lost its surface registration" :
                            std::move(validationError);
                    DiscardRecordBatch();
                    return false;
                }
            }
            mPreparedRecords.push_back(PreparedRecord{ .mHandle = update.mMaterial,
                .mRecordId = material->mRecordId,
                .mBytes = std::vector<std::byte>(update.mBytes.begin(), update.mBytes.end()) });
        }
        mRecordBatchPrepared = true;
        return true;
    }

    void MaterialStore::CommitRecordBatch()
    {
        if (!mRecordBatchPrepared)
            return;

        for (PreparedRecord &prepared : mPreparedRecords)
        {
            const GpuMaterial *const found = FindMaterial(prepared.mHandle);
            if (found == nullptr || found->mRecordId != prepared.mRecordId)
                continue;
            mTable->UpdateRecord(prepared.mRecordId, prepared.mBytes);
            GpuMaterial *const material = const_cast<GpuMaterial *>(found);
            material->mCurrentRecordBytes = std::move(prepared.mBytes);
        }
        mPreparedRecords.clear();
        mRecordBatchPrepared = false;
    }

    void MaterialStore::DiscardRecordBatch()
    {
        mPreparedRecords.clear();
        mRecordBatchPrepared = false;
    }

    void MaterialStore::SetRecordValidator(MaterialRecordValidator validator)
    {
        mRecordValidator = std::move(validator);
    }

    void MaterialStore::PublishRecords(uint64_t completedFrame, uint64_t frame, uint32_t frameIndex)
    {
        if (mTable == nullptr)
            return;

        for (const MaterialTable::RecordChange &change : mTable->PublishRecords(completedFrame, frame))
        {
            for (GpuMaterial &material : mMaterials)
            {
                if (material.mRecordId == change.mId)
                {
                    material.mRecordOffset = change.mOffset;
                    break;
                }
            }
        }

        RefreshRecordRefs(frameIndex);
    }

    bool MaterialStore::PrepareMaterialReloadEntry(GpuMaterial &material, ReloadEntry &entry,
        std::string *error)
    {
        if (material.mPipeline == VK_NULL_HANDLE)
            return true;

        entry.mIndex = material.mMaterialIndex;
        entry.mPipeline = BuildPipeline(material.mDesc, mColorFormat);
        if (entry.mPipeline == VK_NULL_HANDLE)
        {
            if (error != nullptr)
                *error = "could not rebuild the pipeline for '" + material.mDesc.shaderName + "'";
            return false;
        }

        if (mTable == nullptr || material.mRecordId.IsNull())
            return true;

        if (!material.mSurfaceMaterial)
        {
            entry.mRecordId = material.mRecordId;
            return true;
        }

        const SurfaceMaterialCatalog &catalog = mReloadCatalogStaged ? mReloadCatalog : mSurfaceMaterials;
        const SurfaceMaterialRegistration *const registration =
            catalog.Find(material.mDesc.shaderName, material.mDesc.surfaceAsset);
        if (registration == nullptr)
        {
            if (error != nullptr)
                *error = "surface material '" + material.mDesc.shaderName + "' lost its registration";
            return false;
        }
        const uint32_t registrationIndex = catalog.IndexOf(registration);
        if (registrationIndex == UINT32_MAX)
        {
            if (error != nullptr)
                *error = "surface material registration is stale";
            return false;
        }
        entry.mRayTracingSbtOffset = SurfaceMaterialCatalog::HitGroupRecordOffset(registrationIndex);

        std::string candidateError;
        auto candidateLayout = LoadLayout(*registration, &candidateError);
        if (!candidateLayout)
        {
            if (error != nullptr)
                *error = candidateError;
            return false;
        }
        const auto defaults = BuildMigrationDefaults(*candidateLayout, registration->parametersJson,
            &candidateError);
        if (!defaults)
        {
            if (error != nullptr)
                *error = "could not pack defaults for '" + material.mDesc.shaderName + "': " + candidateError;
            return false;
        }

        const MaterialLayout empty;
        MaterialMigration migrated = MigrateMaterialRecord(
            material.mHasAcceptedLayout ? material.mAcceptedLayout : empty, *candidateLayout,
            material.mHasAcceptedLayout ? std::span<const std::byte>(material.mCurrentRecordBytes) :
                                          std::span<const std::byte>(),
            *defaults);
        if (!migrated.ok)
        {
            if (error != nullptr)
                *error = material.mDesc.shaderName + ": " + migrated.error;
            return false;
        }

        if (mRecordValidator &&
            !mRecordValidator(registration->frontend, *candidateLayout, migrated.bytes, candidateError))
        {
            if (error != nullptr)
                *error = material.mDesc.shaderName + ": " + candidateError;
            return false;
        }

        entry.mRecordId = mTable->CreateRecord(migrated.bytes, material.mDesc.recordAlignment);
        if (entry.mRecordId.IsNull())
        {
            if (error != nullptr)
                *error = "material record table is full for '" + material.mDesc.shaderName + "'";
            return false;
        }
        entry.mRecordOffset = mTable->RecordOffset(entry.mRecordId);
        entry.mRecordBytes = std::move(migrated.bytes);
        entry.mLayout = std::move(*candidateLayout);
        entry.mHasLayout = true;
        return true;
    }

    bool MaterialStore::ReloadSurfaceMaterialCatalog(std::string *error)
    {
        std::string catalogError;
        const auto catalog = LoadSurfaceMaterialCatalog(&catalogError);
        if (!catalog)
        {
            if (error != nullptr)
                *error = catalogError;
            return false;
        }
        mSurfaceMaterials = *catalog;
        return true;
    }

    bool MaterialStore::PrepareShaderReload(const SurfaceMaterialCatalog &candidateCatalog,
        std::string *error)
    {
        DiscardShaderReload();
        mReloadCatalog = candidateCatalog;
        mReloadCatalogStaged = true;
        mReloadEntries.reserve(mMaterials.size());
        for (GpuMaterial &material : mMaterials)
        {
            ReloadEntry &entry = mReloadEntries.emplace_back();
            if (!PrepareMaterialReloadEntry(material, entry, error))
            {
                DiscardShaderReload();
                return false;
            }
        }

        mReloadSkyPipeline = BuildSkyPipeline();
        if (mReloadSkyPipeline == VK_NULL_HANDLE)
        {
            if (error != nullptr)
                *error = "could not rebuild the sky pipeline";
            DiscardShaderReload();
            return false;
        }
        mReloadPrepared = true;
        return true;
    }

    void MaterialStore::CommitShaderReload()
    {
        if (!mReloadPrepared)
            return;

        for (ReloadEntry &entry : mReloadEntries)
        {
            if (entry.mIndex >= mMaterials.size())
                continue;
            GpuMaterial &material = mMaterials[entry.mIndex];
            if (entry.mPipeline != VK_NULL_HANDLE)
            {
                if (material.mPipeline != VK_NULL_HANDLE)
                    vkDestroyPipeline(mDevice->Handle(), material.mPipeline, nullptr);
                material.mPipeline = entry.mPipeline;
                entry.mPipeline = VK_NULL_HANDLE;
            }
            if (!entry.mRecordId.IsNull() && entry.mRecordId != material.mRecordId)
            {
                if (!material.mRecordId.IsNull())
                    mTable->ReleaseRecord(material.mRecordId);
                material.mRecordId = entry.mRecordId;
                material.mRecordOffset = entry.mRecordOffset;
                material.mCurrentRecordBytes = std::move(entry.mRecordBytes);
                material.mAcceptedLayout = std::move(entry.mLayout);
                material.mHasAcceptedLayout = entry.mHasLayout;
                entry.mRecordId = kNullMaterialRecord;
            }
            if (entry.mRayTracingSbtOffset != UINT32_MAX)
                material.mRayTracingSbtOffset = entry.mRayTracingSbtOffset;
        }

        if (mReloadSkyPipeline != VK_NULL_HANDLE)
        {
            if (mSkyPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice->Handle(), mSkyPipeline, nullptr);
            mSkyPipeline = mReloadSkyPipeline;
            mReloadSkyPipeline = VK_NULL_HANDLE;
            mDevice->NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(mSkyPipeline),
                "Sky pipeline");
        }

        mReloadEntries.clear();
        if (mReloadCatalogStaged)
        {
            mSurfaceMaterials = std::move(mReloadCatalog);
            mReloadCatalogStaged = false;
        }
        mReloadPrepared = false;
        if (mTable != nullptr)
            for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
                RefreshRecordRefs(frame);
    }

    void MaterialStore::DiscardShaderReload()
    {
        DiscardReloadEntries();
        if (mReloadSkyPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mReloadSkyPipeline, nullptr);
            mReloadSkyPipeline = VK_NULL_HANDLE;
        }
        mReloadPrepared = false;
        mReloadCatalogStaged = false;
        mReloadCatalog = {};
    }

    void MaterialStore::DiscardReloadEntries()
    {
        for (ReloadEntry &entry : mReloadEntries)
        {
            if (entry.mPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice->Handle(), entry.mPipeline, nullptr);
            if (mTable != nullptr && !entry.mRecordId.IsNull())
            {
                bool kept = false;
                for (const GpuMaterial &material : mMaterials)
                    if (material.mRecordId == entry.mRecordId)
                        kept = true;
                if (!kept)
                    mTable->ReleaseRecord(entry.mRecordId);
            }
        }
        mReloadEntries.clear();
    }

    std::size_t MaterialStore::ReleaseMaterials()
    {
        const GpuMaterial *const defaultMaterial = FindMaterial(mDefaultMaterial);
        const GpuMaterial *const errorMaterial = FindMaterial(mErrorMaterial);
        if (defaultMaterial == nullptr || errorMaterial == nullptr)
        {
            const std::size_t count = mMaterials.size();
            DestroyMaterials();
            return count;
        }

        // Kept: draws fall back to them.
        const GpuMaterial keptDefault = *defaultMaterial;
        const GpuMaterial keptError = *errorMaterial;
        std::size_t released = 0;
        for (std::size_t i = 0; i < mMaterials.size(); ++i)
        {
            if (i == mDefaultMaterial.mIndex || i == mErrorMaterial.mIndex)
                continue;
            FreeMaterial(mMaterials[i]);
            ++released;
        }

        mMaterials.clear();
        mMaterials.push_back(keptDefault);
        mMaterials.push_back(keptError);
        mMaterials[0].mMaterialIndex = 0;
        mMaterials[1].mMaterialIndex = 1;
        mFreeIndices.clear();
        mDefaultMaterial = MaterialHandle{ 0, keptDefault.mGeneration };
        mErrorMaterial = MaterialHandle{ 1, keptError.mGeneration };
        if (mTable != nullptr)
        {
            for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
                RefreshRecordRefs(frame);
        }
        return released;
    }

    void MaterialStore::DestroyMaterials()
    {
        for (GpuMaterial &material : mMaterials)
            FreeMaterial(material);
        mMaterials.clear();
        mFreeIndices.clear();
    }

    const GpuMaterial *MaterialStore::FindMaterial(MaterialHandle handle) const
    {
        return FindSlot(mMaterials, handle);
    }

    const GpuMaterial *MaterialStore::FindOrDefault(MaterialHandle handle) const
    {
        const GpuMaterial *material = FindMaterial(handle);
        if (material != nullptr && material->mPipeline != VK_NULL_HANDLE)
            return material;

        // Null means nothing was authored; anything else that fails to resolve is a bug worth seeing.
        const GpuMaterial *fallback = FindMaterial(handle.IsNull() ? mDefaultMaterial : mErrorMaterial);
        if (fallback == nullptr || fallback->mPipeline == VK_NULL_HANDLE)
            fallback = FindMaterial(mDefaultMaterial);
        if (fallback == nullptr || fallback->mPipeline == VK_NULL_HANDLE)
            return nullptr;
        return fallback;
    }

    bool MaterialStore::SupportsSurfaceInputs(MaterialHandle handle) const
    {
        const auto *material = FindOrDefault(handle);
        if (!material) return false;
        const auto &desc = material->mDesc;
        // Generic surface materials only compile the single-target SurfaceForward
        // frontend; there is no <stem>Surface multi-target variant for SSGI.
        if (desc.surfaceMaterial) return false;
        return desc.alphaMode == MaterialAlphaMode::Blend || desc.shaderName == "default" ||
            desc.shaderName == "model" || desc.shaderName == "unlit" || desc.shaderName == "error";
    }

    VkPipeline MaterialStore::SurfacePipeline(MaterialHandle handle)
    {
        const auto *resolved = FindOrDefault(handle);
        if (!resolved) return VK_NULL_HANDLE;
        auto &material = mMaterials[size_t(resolved - mMaterials.data())];
        const auto &desc = material.mDesc;
        if (desc.alphaMode == MaterialAlphaMode::Blend || !SupportsSurfaceInputs(handle)) return VK_NULL_HANDLE;
        if (!material.mSurfaceTried)
        {
            material.mSurfaceTried = true;
            material.mSurfacePipeline = BuildPipeline(desc, kRasterSurfaceFormat, true);
        }
        return material.mSurfacePipeline;
    }

    void MaterialStore::ReleaseSurfacePipelines()
    {
        for (auto &material : mMaterials)
        {
            if (material.mSurfacePipeline) vkDestroyPipeline(mDevice->Handle(), material.mSurfacePipeline, nullptr);
            material.mSurfacePipeline = VK_NULL_HANDLE;
            material.mSurfaceTried = false;
        }
    }
}

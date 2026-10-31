/**
 * @file PostProcessingStack.h
 * @author Rahul Nair
 * @brief Every screen-space post effect between the scene/trace pass and the
 *        swapchain - depth of field, motion blur (and its raster velocity
 *        pass), bloom, tonemap + film, FXAA and DLSS Ray Reconstruction -
 *        behind one lifecycle, one apply and one record call per pipeline.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "Bloom.h"
#include "DepthOfField.h"
#include "Fxaa.h"
#include "MotionBlur.h"
#include "PostProcess.h"
#include "VelocityPass.h"

#include <renderer/AntiAliasingEffect.h>
#include <renderer/DlssConfig.h>
#include <renderer/PostProcessingState.h>
#include <renderer/VulkanRenderer.h>

#if MIR_WITH_DLSS
#include "DlssRayReconstruction.h"
#endif

#include <glm/vec2.hpp>
#include <volk.h>

#include <cstdint>
#include <span>

namespace mir::vk
{
    class Device;
    class RayTracer;
    class ResourceStore;

    class PostProcessingStack
    {
    public:
        // DLSS failing to initialize is not fatal: it just stops being offered.
        bool Initialize(Device &device, VkFormat swapchainFormat, VkExtent2D extent,
            VkDescriptorSetLayout cameraSetLayout);
        void Shutdown();
        // The caller idles the device first. DLSS failing to resize is not fatal.
        bool Resize(VkExtent2D extent);

        // Takes the frame's resolved state. Changing the AA mode idles the device and allocates
        // the new mode's images (FXAA's intermediate, RayTracer's DLSS guides and DLSS's output),
        // freeing the old one's; an unchanged mode costs nothing.
        void Apply(const PostProcessingState &state, RayTracer &rayTracer, VkExtent2D extent);

        // False on a build or device without NGX, or after an init/resize/evaluate failure.
        bool DlssAvailable() const;
        void ResetHistory();

        // T0 experiment only, nullopt disables it without reallocating in-flight images.

        struct RasterInputs
        {
            VkImageView sceneColor;
            VkImage depthImage;
            VkImageView depthView;
            // At most kMaxViewsPerFrame; views[0]'s clip planes drive depth of field.
            std::span<const RenderView> views;
            VkDescriptorSet cameraSet;
            // One per view; only read while NeedsVelocity().
            std::span<const uint32_t> cameraOffsets;
            const ResourceStore *resources;
            VkExtent2D extent;
        };

        // Raster: motion blur's velocity pass re-walks the views, so the caller writes their
        // camera data only while this is true.
        bool NeedsVelocity() const { return mMotionBlurEnabled; }

        // Expects the scene color in SHADER_READ_ONLY and depth still a depth attachment; leaves
        // depth in SHADER_READ_ONLY. Runs velocity, depth of field and motion blur, and
        // returns the color the composite reads.
        VkImageView RecordRaster(VkCommandBuffer cmd, uint32_t frame, const RasterInputs &inputs);

        // Ray tracing: the mode the trace runs with - Dlss falls back to Msaa while Ray
        // Reconstruction can't run this frame.
        AntiAliasingMode RtAntiAliasingMode() const;
        uint32_t RtSampleCount() const { return mAntiAliasingRtSampleCount; }

        // Runs Ray Reconstruction, depth of field and motion blur over RayTracer's outputs
        // inside rect, and returns the color the composite reads.
        VkImageView RecordRayTraced(VkCommandBuffer cmd, uint32_t frame, const RayTracer &rayTracer,
            const RenderView &view, glm::vec2 jitterPixels, VkExtent2D extent);

        // Raster, outside any render pass: with FXAA active, tonemaps color into FXAA's
        // intermediate. Returns whether FXAA is active - pass it on to RecordComposite.
        // Renderer calls this after its FX composition hook.
        void RecordBloom(VkCommandBuffer cmd, uint32_t frame, VkImageView color, VkExtent2D extent);
        bool PrepareComposite(VkCommandBuffer cmd, uint32_t frame, VkImageView color, VkExtent2D extent);

        // Inside the caller's swapchain pass: the FXAA pass over the intermediate, or the tonemap
        // over color. Advances film grain, so call it exactly once per frame.
        void RecordComposite(VkCommandBuffer cmd, uint32_t frame, VkImageView color, bool fxaa,
            VkExtent2D extent);

    private:
        Device *mDevice = nullptr;

        PostProcess mPostProcess;
        Fxaa mFxaa;
        Bloom mBloom;
        DepthOfField mDepthOfField;
        VelocityPass mVelocityPass;
        MotionBlur mMotionBlur;
    #if MIR_WITH_DLSS
        DlssRayReconstruction mDlss;
        bool mDlssReady = false;
    #endif

        bool mBloomEnabled = false;
        bool mDepthOfFieldEnabled = false;
        bool mMotionBlurEnabled = false;
        AntiAliasingMode mAntiAliasingMode = AntiAliasingMode::Off;
        uint32_t mAntiAliasingRtSampleCount = 4;
    };
}

/**
 * @file main.cpp
 * @author sumin.park
 * @brief Device regression for streamed texture pixels, mixed uploads and safe cancellation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <renderer/Shapes.h>
#include <renderer/TextureUpload.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <optional>
#include <glm/ext/matrix_clip_space.hpp>

namespace
{
    void Require(bool condition, const char *message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
}

int main(int argc, char **argv)
{
    const std::string prefix = argc > 1 ? argv[1] : "texture-gpu";
    mir::LogConfig logging;
    logging.filePath = prefix + ".log";
    mir::InitLog(logging);
    try
    {
        auto window =
            mir::Window::Create({.mWidth = 640, .mHeight = 360, .mTitle = "Texture GPU check", .mStartHidden = true});
        mir::VulkanRenderer renderer;
        struct Shutdown
        {
            mir::VulkanRenderer &mRenderer;
            bool mActive = true;
            ~Shutdown()
            {
                if (mActive)
                    mRenderer.Shutdown();
            }
        } shutdown{renderer};
        Require(renderer.Initialize({window.get(), "TextureGpuCheck", true}), "Renderer initialization failed");
        renderer.SetAmbientColor(glm::vec3(1));
        renderer.SetDirectionalLight(glm::vec3(0, -1, 0), glm::vec3(0));
        renderer.SetPostProcessing({});
        auto quad = mir::MakeQuad();
        const std::array<glm::vec2, 4> uvs{glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1)};
        for (size_t i = 0; i < quad.vertices.size(); ++i)
        {
            quad.vertices[i].uv = uvs[i];
            quad.vertices[i].pos.z = 0.5f;
            quad.vertices[i].color = glm::vec3(1);
        }
        const auto mesh = renderer.CreateMesh(quad.vertices, quad.indices);
        Require(!mesh.IsNull(), "Reference mesh creation failed");
        std::vector<std::byte> pixels(size_t(4096) * 4096 * 4);
        for (uint32_t y = 0; y < 4096; ++y)
            for (uint32_t x = 0; x < 4096; ++x)
            {
                const size_t offset = (size_t(y) * 4096 + x) * 4;
                pixels[offset] = std::byte(((x / 64 + y / 64) & 1) ? 220 : 40);
                pixels[offset + 1] = std::byte((x / 16) % 256);
                pixels[offset + 2] = std::byte((y / 16) % 256);
                pixels[offset + 3] = std::byte{255};
            }
        const mir::TextureDesc desc{
            .width = 4096, .height = 4096, .pixels = pixels, .srgb = true, .generateMips = true};
        const auto reference = renderer.CreateTexture(desc);
        Require(!reference.IsNull(), "Reference texture creation failed");
        mir::MaterialDesc materialDesc{
            .shaderName = "model", .cullMode = VK_CULL_MODE_NONE, .baseColorTexture = reference};
        materialDesc.sampler.useMips = true;
        materialDesc.model.factors[0].x = 0; // The streaming fixture tests diffuse color under ambient light.
        const auto referenceMaterial = renderer.CreateMaterial(materialDesc);
        Require(!referenceMaterial.IsNull(), "Reference material creation failed");
        mir::DrawItem item;
        item.mesh = mesh;
        item.material = referenceMaterial;
        mir::RenderView view;
        view.items = std::span(&item, 1);
        auto draw = [&] {
            window->PollEvents();
            Require(!window->ShouldClose(), "GPU check window closed early");
            renderer.DrawFrame(std::span(&view, 1));
        };
        for (int i = 0; i < 4; ++i)
            draw();
        renderer.CaptureNextFrame(prefix + "-reference.png");
        draw();
        item.material = renderer.ErrorMaterial();
        renderer.CaptureNextFrame(prefix + "-error.png");
        draw();
        item.material = referenceMaterial;
        const auto baseline = renderer.MeshStreaming();
        auto source = mir::PrepareTextureUpload(desc);
        const std::weak_ptr<const void> cpu = source.keepalive;
        const auto streamed = renderer.QueueTexture(std::move(source));
        Require(renderer.TextureState(streamed) == mir::TextureUploadState::Pending, "Texture was not queued");
        materialDesc.baseColorTexture = streamed;
        const auto streamedMaterial = renderer.CreateMaterial(materialDesc);
        Require(!streamedMaterial.IsNull(), "Pending texture material creation failed");
        item.material = streamedMaterial;
        const auto queuedMesh = renderer.QueueMesh(mir::MeshUploadSource::Own(quad.vertices, quad.indices), {});
        bool releasedBeforeReady = false, meshFinishedFirst = false;
        uint32_t frames = 0;
        while (renderer.TextureState(streamed) != mir::TextureUploadState::Ready && frames++ < 120)
        {
            draw();
            const auto stats = renderer.MeshStreaming();
            Require(stats.lastFrameBytes <= stats.desc.frameBytes, "Texture transfer exceeded the frame budget");
            Require(stats.stagingUsedBytes <= stats.desc.stagingBytes, "Texture transfer exceeded the ring");
            Require(stats.queueWaits == baseline.queueWaits && stats.deviceWaits == baseline.deviceWaits,
                    "Asynchronous texture transfer waited for the device or queue");
            releasedBeforeReady |= cpu.expired() && renderer.TextureState(streamed) != mir::TextureUploadState::Ready;
            meshFinishedFirst |= renderer.MeshState(queuedMesh) == mir::MeshUploadState::Ready &&
                                 renderer.TextureState(streamed) != mir::TextureUploadState::Ready;
        }
        Require(renderer.TextureState(streamed) == mir::TextureUploadState::Ready && frames > 1,
                "Texture did not complete across multiple frames");
        Require(releasedBeforeReady, "Texture source survived past the final staging copy");
        Require(meshFinishedFirst, "Partial texture upload starved a waiting mesh");
        item.material = streamedMaterial;
        renderer.CaptureNextFrame(prefix + "-streamed.png");
        draw();
        const auto expected = mir::ReadBytes(prefix + "-reference.png");
        const auto actual = mir::ReadBytes(prefix + "-streamed.png");
        const auto error = mir::ReadBytes(prefix + "-error.png");
        Require(expected && actual && error && *expected == *actual && *expected != *error,
                "Streamed texture differs from synchronous pixels or the test drew only a fallback");
        // Each PBR input must change actual raster and RT pixels, not merely survive a cook.
        auto pbrQuad = quad;
        for (auto &v : pbrQuad.vertices)
        {
            v.pos.z = -2;
            v.normal = {0, 0, 1};
            v.tangent = {1, 0, 0, 1};
        }
        const auto pbrMesh = renderer.CreateMesh(pbrQuad.vertices, pbrQuad.indices);
        Require(!pbrMesh.IsNull(), "PBR mesh creation failed");
        const std::array<std::byte, 4> normalPixels{std::byte{220}, std::byte{128}, std::byte{220}, std::byte{255}};
        const auto normal = renderer.CreateTexture({.width = 1, .height = 1, .pixels = normalPixels, .srgb = false});
        Require(!normal.IsNull(), "PBR normal texture creation failed");
        mir::MaterialDesc pbr{.shaderName = "model", .cullMode = VK_CULL_MODE_NONE};
        pbr.model.factors[0] = {0, .7f, 1, 1};
        std::vector<mir::MaterialHandle> pbrMaterials;
        const auto add = [&](const char *name, const mir::MaterialDesc &desc) {
            auto handle = renderer.CreateMaterial(desc);
            Require(!handle.IsNull(), "PBR material creation failed");
            pbrMaterials.push_back(handle);
            return std::pair<std::string, mir::MaterialHandle>{name, handle};
        };
        std::vector<std::pair<std::string, mir::MaterialHandle>> cases{add("baseline", pbr)};
        auto variant = pbr;
        variant.model.factors[0].x = 1;
        cases.push_back(add("metallic", variant));
        variant = pbr;
        variant.model.factors[0].y = .15f;
        cases.push_back(add("roughness", variant));
        variant = pbr;
        variant.model.textures[0].texture = normal;
        cases.push_back(add("normal", variant));
        const auto queuedNormal = renderer.QueueTexture(
            mir::PrepareTextureUpload({.width = 1, .height = 1, .pixels = normalPixels, .srgb = false}));
        Require(!queuedNormal.IsNull(), "Queued PBR normal allocation failed");
        variant = pbr;
        variant.model.textures[0].texture = queuedNormal;
        cases.push_back(add("streamed-normal", variant));
        variant = pbr;
        variant.model.textures[0].texture = normal;
        variant.model.factors[0].z = 0;
        cases.push_back(add("normal-disabled", variant));
        variant = pbr;
        variant.model.factors[0].x = 1;
        variant.model.textures[1].texture = normal;
        cases.push_back(add("metallic-roughness-map", variant));
        variant = pbr;
        variant.model.textures[2].texture = normal;
        cases.push_back(add("occlusion", variant));
        variant = pbr;
        variant.model.factors[1] = glm::vec4(.1f, .6f, .3f, 0);
        cases.push_back(add("emissive", variant));
        variant = pbr;
        variant.model.factors[2].w = 0;
        cases.push_back(add("specular", variant));
        variant = pbr;
        variant.model.factors[2] = glm::vec4(2, .3f, .1f, 1);
        cases.push_back(add("specular-color", variant));
        variant = pbr;
        variant.model.factors[3].x = 2;
        cases.push_back(add("ior", variant));
        variant = pbr;
        variant.model.factors[3].y = 1;
        variant.model.factors[3].z = .1f;
        cases.push_back(add("clearcoat", variant));
        variant.model.textures[8].texture = normal;
        cases.push_back(add("clearcoat-normal", variant));
        variant.model.textures[8].texture = {};
        variant.model.textures[7].texture = normal;
        cases.push_back(add("clearcoat-roughness-map", variant));
        view.viewProj = glm::perspectiveRH_ZO(1.0f, 640.f / 360.f, .1f, 100.f);
        view.viewProj[1][1] *= -1;
        item.mesh = pbrMesh;
        item.tint = {.7f, .25f, .08f, 1};
        renderer.SetAmbientColor(glm::vec3(.2f));
        renderer.SetDirectionalLight(glm::vec3(0, 0, 1), glm::vec3(1));
        for (bool trace : {false, true})
        {
            if (trace && !renderer.RayTracingSupported())
                continue;
            std::optional<std::vector<std::byte>> baselinePixels;
            std::optional<std::vector<std::byte>> normalImage;
            std::optional<std::vector<std::byte>> metalImage, coatImage;
            for (const auto &[name, handle] : cases)
            {
                item.material = handle;
                const auto render = [&] {
                    if (trace)
                        renderer.TraceFrame(std::span(&view, 1));
                    else
                        draw();
                };
                for (int frame = 0; frame < 4; ++frame)
                    render();
                const auto path = prefix + "-pbr-" + (trace ? "rt-" : "raster-") + name + ".png";
                renderer.CaptureNextFrame(path);
                render();
                const auto pixels = mir::ReadBytes(path);
                Require(pixels.has_value(), "PBR frame capture missing");
                if (name == "baseline")
                    baselinePixels = pixels;
                else if (name == "normal-disabled")
                    Require(*pixels == *baselinePixels, "Zero normal strength did not preserve base shading");
                else
                    Require(*pixels != *baselinePixels, "A PBR channel did not change the rendered pixels");
                if (name == "normal")
                    normalImage = pixels;
                if (name == "streamed-normal")
                    Require(normalImage && *pixels == *normalImage,
                            "Queued PBR normal did not match synchronous shading");
                if (name == "metallic")
                    metalImage = pixels;
                if (name == "metallic-roughness-map")
                    Require(metalImage && *pixels != *metalImage, "Metallic/roughness texture had no effect");
                if (name == "clearcoat")
                    coatImage = pixels;
                if (name == "clearcoat-normal" || name == "clearcoat-roughness-map")
                    Require(coatImage && *pixels != *coatImage, "Clearcoat texture had no effect");
            }
        }
        MIR_LOG_INFO("PBR pixel checks passed for {} materials (RT supported: {})", cases.size(),
                     renderer.RayTracingSupported());
        item.mesh = mesh;
        item.material = streamedMaterial;
        item.tint = glm::vec4(1);
        view.viewProj = glm::mat4(1);
        const std::array pbrTextures{normal, queuedNormal};
        renderer.ReleaseModelResources(std::span(&pbrMesh, 1), pbrMaterials, pbrTextures);
        auto cancelledSource = mir::PrepareTextureUpload(desc);
        const std::weak_ptr<const void> cancelledCpu = cancelledSource.keepalive;
        const auto cancelled = renderer.QueueTexture(std::move(cancelledSource));
        draw();
        Require(renderer.TextureState(cancelled) == mir::TextureUploadState::Uploading,
                "Cancellation fixture did not start");
        Require(renderer.RetireTexture(cancelled), "Partial texture retirement failed");
        Require(cancelledCpu.expired(), "Partial cancellation retained CPU pixels");
        Require(renderer.TextureState(cancelled) == mir::TextureUploadState::Stale,
                "Retired texture handle stayed valid");
        const auto pending = renderer.QueueTexture(mir::PrepareTextureUpload(desc));
        Require(renderer.RetireTexture(pending), "Pending texture retirement failed");
        view.items = {};
        const std::array meshes{mesh, queuedMesh};
        const std::array materials{referenceMaterial, streamedMaterial};
        const std::array textures{reference, streamed};
        renderer.ReleaseModelResources(meshes, materials, textures);
        for (int i = 0; i < 5; ++i)
            draw();
        Require(renderer.MeshStreaming().pendingBytes == 0 && renderer.MeshStreaming().uploadingBytes == 0,
                "Cancelled texture left upload work behind");
        renderer.Shutdown();
        shutdown.mActive = false;
        Require(mir::ErrorCount() == 0, "GPU validation or renderer errors were logged");
        MIR_LOG_INFO("Texture GPU check passed: {} frames, matching pixels, no transfer waits", frames);
        return 0;
    }
    catch (const std::exception &error)
    {
        MIR_LOG_ERROR("Texture GPU check: {}", error.what());
        return 1;
    }
}

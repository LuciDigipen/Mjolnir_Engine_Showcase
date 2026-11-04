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
        auto window = mir::Window::Create({ .mWidth = 640, .mHeight = 360,
            .mTitle = "Texture GPU check", .mStartHidden = true });
        mir::VulkanRenderer renderer;
        struct Shutdown
        {
            mir::VulkanRenderer &mRenderer;
            bool mActive = true;
            ~Shutdown() { if (mActive) mRenderer.Shutdown(); }
        } shutdown{ renderer };
        Require(renderer.Initialize({ window.get(), "TextureGpuCheck", true }), "Renderer initialization failed");
        renderer.SetAmbientColor(glm::vec3(1));
        renderer.SetDirectionalLight(glm::vec3(0, -1, 0), glm::vec3(0));
        renderer.SetPostProcessing({});
        auto quad = mir::MakeQuad();
        const std::array<glm::vec2, 4> uvs{ glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1) };
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
                pixels[offset + 3] = std::byte{ 255 };
            }
        const mir::TextureDesc desc{ .width = 4096, .height = 4096, .pixels = pixels,
            .srgb = true, .generateMips = true };
        const auto reference = renderer.CreateTexture(desc);
        Require(!reference.IsNull(), "Reference texture creation failed");
        mir::MaterialDesc materialDesc{ .shaderName = "model", .cullMode = VK_CULL_MODE_NONE,
            .baseColorTexture = reference };
        materialDesc.sampler.useMips = true;
        const auto referenceMaterial = renderer.CreateMaterial(materialDesc);
        Require(!referenceMaterial.IsNull(), "Reference material creation failed");
        mir::DrawItem item;
        item.mesh = mesh;
        item.material = referenceMaterial;
        mir::RenderView view;
        view.items = std::span(&item, 1);
        auto draw = [&]
            {
                window->PollEvents();
                Require(!window->ShouldClose(), "GPU check window closed early");
                renderer.DrawFrame(std::span(&view, 1));
            };
        for (int i = 0; i < 4; ++i) draw();
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
        auto cancelledSource = mir::PrepareTextureUpload(desc);
        const std::weak_ptr<const void> cancelledCpu = cancelledSource.keepalive;
        const auto cancelled = renderer.QueueTexture(std::move(cancelledSource));
        draw();
        Require(renderer.TextureState(cancelled) == mir::TextureUploadState::Uploading, "Cancellation fixture did not start");
        Require(renderer.RetireTexture(cancelled), "Partial texture retirement failed");
        Require(cancelledCpu.expired(), "Partial cancellation retained CPU pixels");
        Require(renderer.TextureState(cancelled) == mir::TextureUploadState::Stale, "Retired texture handle stayed valid");
        const auto pending = renderer.QueueTexture(mir::PrepareTextureUpload(desc));
        Require(renderer.RetireTexture(pending), "Pending texture retirement failed");
        view.items = {};
        const std::array meshes{ mesh, queuedMesh };
        const std::array materials{ referenceMaterial, streamedMaterial };
        const std::array textures{ reference, streamed };
        renderer.ReleaseModelResources(meshes, materials, textures);
        for (int i = 0; i < 5; ++i) draw();
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

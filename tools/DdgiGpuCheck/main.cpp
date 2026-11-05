/**
 * @file main.cpp
 * @author Rahul Nair
 * @brief GPU regression for material DDGI, coverage and shader reload lifetime.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <renderer/CameraMath.h>
#include <renderer/Shapes.h>
#include <renderer/SurfaceMaterialCatalog.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <filesystem>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void Require(bool ok, const std::string &message)
    {
        if (!ok) throw std::runtime_error(message);
    }

    double ImageEnergy(const std::string &path)
    {
        int width, height, channels;
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
            stbi_load(path.c_str(), &width, &height, &channels, 3), stbi_image_free);
        Require(bool(pixels), "Could not read capture: " + path);
        double sum = 0;
        for (int i = 0; i < width * height * 3; ++i) sum += pixels.get()[i];
        return sum / double(width * height * 3);
    }
}

int main(int argc, char **argv)
{
    const std::string prefix = argc > 1 ? argv[1] : "ddgi-gpu";
    mir::LogConfig logging;
    logging.filePath = prefix + ".log";
    mir::InitLog(logging);
    try
    {
        auto window = mir::Window::Create({ .mWidth = 320, .mHeight = 180,
            .mTitle = "DDGI GPU check", .mStartHidden = true });
        mir::VulkanRenderer renderer;
        struct Shutdown
        {
            mir::VulkanRenderer &renderer;
            ~Shutdown() { renderer.Shutdown(); }
        } shutdown{ renderer };
        Require(renderer.Initialize({ window.get(), "DdgiGpuCheck", true, false }), "Renderer initialization failed");
        Require(renderer.RayTracingSupported(), "DDGI GPU check requires ray tracing hardware");
        renderer.SetAmbientColor(glm::vec3(0));
        renderer.SetDirectionalLight(glm::vec3(0, 1, 0), glm::vec3(0));
        renderer.SetSkyVisible(false);
        mir::PostProcessingState post;
        post.antiAliasing.mode = mir::AntiAliasingMode::Off;
        renderer.SetPostProcessing(post);

        const auto catalog = mir::LoadSurfaceMaterialCatalog();
        Require(catalog.has_value(), "Material catalog missing");
        const auto makeMaterial = [&](const char *asset)
            {
                const auto *registration = catalog->FindByAsset(asset, "mesh");
                Require(registration != nullptr, std::string("Missing fixture material: ") + asset);
                mir::MaterialDesc desc;
                desc.shaderName = registration->stem;
                desc.surfaceAsset = asset;
                desc.surfaceMaterial = true;
                const auto material = renderer.CreateMaterial(desc);
                Require(!material.IsNull(), "Surface material creation failed");
                return material;
            };
        const auto first = makeMaterial("assets/materials/default.material.json");
        const auto second = makeMaterial("games/Sponza/assets/materials/sponza_matte.material.json");
        const auto shape = mir::MakeCube();
        const auto cube = renderer.CreateMesh(shape.vertices, shape.indices);
        Require(!cube.IsNull(), "Cube upload failed");
        std::vector<mir::DrawItem> items;
        const auto add = [&](glm::vec3 position, glm::vec3 scale, mir::MaterialHandle material, glm::vec4 tint)
            {
                mir::DrawItem item;
                item.mesh = cube;
                item.material = material;
                item.model = glm::scale(glm::translate(glm::mat4(1), position), scale);
                item.previousModel = item.model;
                item.normalMatrix = glm::inverseTranspose(glm::mat3(item.model));
                item.tint = tint;
                items.push_back(item);
            };
        add({ 0,-1.1f,-3 }, { 8,.2f,8 }, first, { 1,1,1,1 });
        add({ 0,1,-6 }, { 8,4,.2f }, second, { 1,1,1,1 });
        add({ -1,-.4f,-3 }, { 1.2f,1.2f,1.2f }, first, { 1,.1f,.1f,1 });
        add({ 1,-.4f,-3 }, { 1.2f,1.2f,1.2f }, second, { 1,1,1,1 });
        mir::RenderView view;
        view.items = items;
        view.rayTracingItems = items;
        view.viewProj = mir::MakeViewProjection(glm::mat4(1), mir::Camera{}, 320.0f/180.0f);
        view.nearZ = .1f;
        view.farZ = 100;
        view.fovYDegrees = 60;
        const auto draw = [&](int frames)
            {
                for (int i = 0; i < frames; ++i)
                {
                    window->PollEvents();
                    Require(!window->ShouldClose(), "GPU check window closed early");
                    renderer.TraceFrame(std::span(&view, 1));
                }
            };
        const auto capture = [&](const char *suffix)
            {
                const std::string path = prefix + suffix + ".png";
                renderer.CaptureNextFrame(path);
                draw(1);
                return ImageEnergy(path);
            };
        mir::DdgiSettings gi;
        gi.enabled = true;
        gi.origin = { 0, 1, -3 };
        gi.probeCounts = { 8, 6, 10 };
        gi.probeSpacing = 1;
        gi.probesPerFrame = 240;
        gi.raysPerProbe = 128;
        gi.intensity = 0;
        renderer.SetGlobalIllumination(gi);
        draw(80);
        Require(renderer.GiStatus().starts_with("Active"), "DDGI never activated: " + renderer.GiStatus());
        const auto zero = capture("-zero");
        gi.intensity = 1;
        renderer.SetGlobalIllumination(gi);
        draw(8);
        const auto lit = capture("-lit");
        Require(lit > zero + .1, "No measurable indirect lighting from material GI");
        renderer.mShowGiVolume = true;
        draw(8);
        const auto probeStates = renderer.GiProbeStates();
        Require(probeStates.size() == gi.ProbeCount(), "GPU probe-state readback did not complete");
        for (const auto &probe : probeStates)
            for (float offset : probe.offset)
                Require(std::isfinite(offset), "Probe relocation contains a nonfinite offset");
        renderer.mShowGiVolume = false;

        // Prepare/commit matching base and DDGI shader generations atomically.
        std::string error;
        Require(renderer.ShaderReloadAvailable(), "Shader reload unavailable");
        const auto shaders = mir::ShaderPath("shader_manifest.json").parent_path();
        Require(renderer.PrepareShaderReload(shaders, &error), "Reload preparation failed: " + error);
        renderer.CommitPreparedShaderReload();
        draw(80);
        Require(renderer.GiStatus().starts_with("Active"), "DDGI did not recover after shader reload");
        const auto reloaded = capture("-reloaded");
        Require(reloaded > zero + .1, "Reload lost indirect lighting");

        // Reject an incomplete candidate without replacing the live generation.
        const auto incomplete = std::filesystem::path(prefix + "-missing-variant");
        std::filesystem::create_directories(incomplete);
        struct RemoveFixture
        {
            std::filesystem::path path;
            ~RemoveFixture() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
        } removeFixture{ incomplete };
        for (const auto &file : std::filesystem::directory_iterator(shaders))
            if (file.is_regular_file())
                std::filesystem::copy_file(file.path(), incomplete / file.path().filename(),
                    std::filesystem::copy_options::overwrite_existing);
        const auto *defaultMaterial = catalog->FindByAsset("assets/materials/default.material.json", "mesh");
        std::filesystem::remove(incomplete / (defaultMaterial->stem + ".gi.closestHitSurface.spv"));
        error.clear();
        Require(!renderer.PrepareShaderReload(incomplete, &error), "Missing GI variant was accepted");
        Require(!error.empty(), "Missing GI variant produced no diagnostic");
        draw(8);
        Require(renderer.GiStatus().starts_with("Active"), "Rejected reload discarded working DDGI");

        // An incomplete scene holds/fades history, then recovers without an invalid table.
        view.rayTracingSceneComplete = false;
        draw(40);
        view.rayTracingSceneComplete = true;
        draw(80);
        Require(renderer.GiStatus().starts_with("Active"), "DDGI did not recover after incomplete residency");
        gi.origin[0] += .5;
        renderer.SetGlobalIllumination(gi);
        renderer.RequestSwapchainRecreate();
        draw(80);
        Require(renderer.GiStatus().starts_with("Active"), "DDGI did not recover after volume edit/resize");
        gi.enabled = false;
        renderer.SetGlobalIllumination(gi);
        draw(4);
        gi.enabled = true;
        renderer.SetGlobalIllumination(gi);
        draw(80);
        Require(renderer.GiStatus().starts_with("Active"), "DDGI did not recover after re-enable");
        Require(mir::ErrorCount() == 0, "Vulkan or renderer errors were logged");
        MIR_LOG_INFO("DDGI GPU check passed: zero {}, lit {}, reloaded {}", zero, lit, reloaded);
        MIR_LOG_INFO("DDGI completed GPU timings: {}", renderer.GiStatus());
        return 0;
    }
    catch (const std::exception &error)
    {
        MIR_LOG_ERROR("DDGI GPU check failed: {}", error.what());
        return 1;
    }
}

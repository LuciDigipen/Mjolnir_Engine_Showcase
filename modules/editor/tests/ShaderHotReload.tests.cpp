/**
 * @file ShaderHotReload.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/ShaderHotReload.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */
#include <editor/ShaderHotReload.h>
#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>
#include <jobs/BackgroundJobQueue.h>
#include <jobs/JobExecutor.h>
#include <renderer/MaterialReflection.h>
#include <renderer/ShaderReload.h>
#include <renderer/TerrainMaterialAsset.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <thread>
#include <future>

namespace
{
    std::string Utf8(const std::filesystem::path &path)
    {
        const auto bytes = path.generic_u8string();
        return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    }
    bool Pump(mir::ShaderHotReload &watcher, const std::function<bool()> &done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            watcher.Update();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
    struct Fixture
    {
        explicit Fixture(std::size_t workers = 1) : jobs(workers) {}
        mir::JobExecutor jobs;
        std::filesystem::path root = mir::UniqueShaderJobDirectory(std::filesystem::temp_directory_path() /
            std::filesystem::u8path("mjolnir shader-\xe6\xb5\x8b\xe8\xaf\x95"));
        std::filesystem::path shaders = root / "runtime";
        std::filesystem::path include = root / "value.slang";
        std::filesystem::path source = root / "probe.slang";
        mir::ShaderManifest manifest;
        int prepares = 0, commits = 0, discards = 0;
        bool reject = false;
        ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
        mir::ShaderReloadBackend Backend()
        {
            return {[] { return true; }, [this](const auto &path, std::string *error)
                {
                    ++prepares;
                    if (reject) { *error = "injected late owner failure"; return false; }
                    return mir::LoadShaderManifest(path / "shader_manifest.json", error).has_value();
                }, [this] { ++commits; }, [this] { ++discards; }};
        }
        void Seed()
        {
            REQUIRE(mir::WriteText(source, "#include \"value.slang\"\nRWStructuredBuffer<float> result;\n"
                "[shader(\"compute\")][numthreads(1,1,1)] void probeMain() { result[0] = Value(); }\n"));
            REQUIRE(mir::WriteText(include, "float Value() { return 1.0; }\n"));
            manifest.slangc = std::filesystem::u8path(MIR_EDITOR_SLANGC);
            manifest.workingDirectory = root;
            mir::ShaderManifestOutput output;
            output.output = "probe.spv";
            output.reflection = "probe.spv.reflection.json";
            output.depfile = "probe.spv.d";
            output.entry = "probeMain";
            output.stage = "compute";
            output.sources = {Utf8(source)};
            output.arguments = {Utf8(manifest.slangc), Utf8(source), "-target", "spirv", "-profile", "spirv_1_5",
                "-fvk-use-entrypoint-name", "-entry", output.entry, "-o", output.output,
                "-reflection-json", output.reflection, "-depfile", output.depfile};
            manifest.outputs.push_back(output);
            const auto artifact = mir::CompileShaderArtifact(manifest, output, root / "seed");
            INFO(artifact.diagnostic);
            REQUIRE(artifact.success);
            mir::ShaderGenerationStore store(shaders);
            uint64_t generation;
            std::string error;
            REQUIRE(store.BeginPublish(&generation, &error));
            for (const auto &path : {artifact.output.output, artifact.output.reflection, artifact.output.depfile})
                REQUIRE(store.StageFromFile(Utf8(std::filesystem::u8path(path).filename()), std::filesystem::u8path(path), &error));
            const auto text = mir::SerializeShaderManifest(manifest);
            REQUIRE(store.StageFile("shader_manifest.json", std::as_bytes(std::span(text.data(), text.size())), &error));
            const std::string catalog = R"({"version":1,"materials":[]})";
            REQUIRE(store.StageFile("surface_materials.json", std::as_bytes(std::span(catalog.data(), catalog.size())), &error));
            REQUIRE(store.Finish(&error));
            REQUIRE(store.PublishPointer(&error));
        }
    };
}

TEST_CASE("Watcher discovers new includes and keeps them across restart", "[editor][shader][watcher]")
{
    const auto workers = GENERATE(0u, 1u);
    CAPTURE(workers);
    Fixture fixture(workers);
    fixture.Seed();
    {
        mir::ShaderHotReload watcher;
        watcher.Initialize(fixture.root, fixture.shaders, fixture.Backend(), fixture.jobs);
        REQUIRE(watcher.Enabled());
        const auto stamp = std::filesystem::last_write_time(fixture.include);
        REQUIRE(mir::WriteText(fixture.include, "float Value() { return 2.0; }\n"));
        std::filesystem::last_write_time(fixture.include, stamp);
        REQUIRE(Pump(watcher, [&] { return fixture.commits == 1; }));
        const auto newInclude = fixture.root / "new-value.slang";
        REQUIRE(mir::WriteText(newInclude, "float NewValue() { return 3.0; }\n"));
        REQUIRE(mir::WriteText(fixture.include, "#include \"new-value.slang\"\nfloat Value() { return NewValue(); }\n"));
        REQUIRE(Pump(watcher, [&] { return fixture.commits == 2; }));
        REQUIRE(mir::WriteText(newInclude, "float NewValue() { return 4.0; }\n"));
        REQUIRE(Pump(watcher, [&] { return fixture.commits == 3; }));
    }
    mir::ShaderHotReload restarted;
    restarted.Initialize(fixture.root, fixture.shaders, fixture.Backend(), fixture.jobs);
    REQUIRE(restarted.Enabled());
    REQUIRE(mir::WriteText(fixture.root / "new-value.slang", "float NewValue() { return 5.0; }\n"));
    REQUIRE(Pump(restarted, [&] { return fixture.commits == 4; }));
    restarted.Shutdown();
    const auto jobs = fixture.shaders / ".jobs";
    CHECK(std::filesystem::is_empty(jobs));
}

TEST_CASE("Watcher keeps last-good state after a late owner rejects preparation", "[editor][shader][watcher]")
{
    Fixture fixture;
    fixture.Seed();
    mir::ShaderGenerationStore reader(fixture.shaders);
    const auto before = reader.CurrentId();
    mir::ShaderHotReload watcher;
    watcher.Initialize(fixture.root, fixture.shaders, fixture.Backend(), fixture.jobs);
    fixture.reject = true;
    REQUIRE(mir::WriteText(fixture.include, "float Value() { return 2.0; }\n"));
    REQUIRE(Pump(watcher, [&] { return fixture.discards == 1; }));
    CHECK(reader.CurrentId() == before);
    CHECK(fixture.commits == 0);
    fixture.reject = false;
    REQUIRE(mir::WriteText(fixture.include, "float Value() { return 3.0; }\n"));
    REQUIRE(Pump(watcher, [&] { return fixture.commits == 1; }));
    CHECK(reader.CurrentId() > before);
}

TEST_CASE("Watcher expands all registered frontends under current custom asset roots", "[editor][shader][watcher]")
{
    Fixture fixture;
    fixture.Seed();
    const auto project = fixture.root / "game";
    const auto content = project / "custom-content";
    const auto other = fixture.root / "other-game";
    std::filesystem::create_directories(content);
    std::filesystem::create_directories(other);
    const auto shader = content / "shared.slang";
    REQUIRE(mir::WriteText(shader,
        "struct V { float4 position : SV_Position; };\n"
        "[shader(\"vertex\")] V vertexMain(uint id : SV_VertexID) { V v; v.position=float4(0,0,0,1); return v; }\n"
        "[shader(\"fragment\")] float4 fragmentMain() : SV_Target { return float4(1,0,0,1); }\n"
        "struct Payload { float value; };\n"
        "[shader(\"closesthit\")] void closestHitSurface(inout Payload p, in BuiltInTriangleIntersectionAttributes a) { p.value=1; }\n"
        "[shader(\"compute\")][numthreads(1,1,1)] void layoutMain() {}\n"));
    fixture.manifest.assetRoots = {{content, "custom"}, {other, "other"}};
    fixture.manifest.commonArguments = {"-target", "spirv", "-profile", "spirv_1_5", "-fvk-use-entrypoint-name"};
    for (const auto &name : {"mesh", "terrain"})
    {
        mir::ShaderFrontendTemplate frontend;
        frontend.frontend = name;
        frontend.placeholders = {"{materialSource}"};
        frontend.vertex = {"{materialSource}", "-entry", "vertexMain", "-stage", "vertex"};
        frontend.fragment = {"{materialSource}", "-entry", "fragmentMain", "-stage", "fragment"};
        frontend.raytracing = {"{materialSource}", "-entry", "closestHitSurface", "-stage", "closesthit"};
        frontend.layout = {"{materialSource}", "-entry", "layoutMain", "-stage", "compute"};
        fixture.manifest.frontendTemplates.push_back(std::move(frontend));
    }
    mir::ShaderGenerationStore store(fixture.shaders);
    uint64_t generation;
    REQUIRE(store.BeginPublish(&generation));
    const auto text = mir::SerializeShaderManifest(fixture.manifest);
    REQUIRE(store.StageFile("shader_manifest.json", std::as_bytes(std::span(text.data(), text.size()))));
    REQUIRE(store.Finish());
    REQUIRE(store.PublishPointer());
    store.Unlock();
    mir::ShaderHotReload watcher;
    watcher.Initialize(project, fixture.shaders, fixture.Backend(), fixture.jobs);
    REQUIRE(watcher.Enabled());
    const std::string material = R"({"version":1,"shader":{"file":"shared.slang","type":"Custom"},"frontends":["mesh","terrain"],"parameters":{}})";
    REQUIRE(mir::WriteText(other / "ignored.material.json", material));
    REQUIRE(mir::WriteText(content / "new.material.json", material));
    REQUIRE(Pump(watcher, [&] { return fixture.commits == 1; }));
    const auto current = mir::ShaderGenerationStore::ReadCurrent(fixture.shaders);
    REQUIRE(current);
    const auto catalog = mir::ReadText(store.GenerationPath(current->mId) / "surface_materials.json");
    REQUIRE(catalog);
    const auto json = nlohmann::json::parse(*catalog);
    REQUIRE(json.at("materials").size() == 2);
    CHECK(json.at("materials")[0].at("frontend") == "mesh");
    CHECK(json.at("materials")[1].at("frontend") == "terrain");
    REQUIRE(mir::WriteText(content / "another.material.json", material));
    REQUIRE(Pump(watcher, [&] { return fixture.commits == 2; }));
    const auto manifest = mir::LoadShaderManifest(store.GenerationPath(*store.CurrentId()) / "shader_manifest.json");
    REQUIRE(manifest);
    CHECK(manifest->outputs.size() == 9);
}

#if defined(_WIN32)
TEST_CASE("Watcher shutdown cancels an active compiler and removes its owned artifacts", "[editor][shader][watcher]")
{
    Fixture fixture;
    fixture.Seed();
    const auto script = fixture.root / "blocked.ps1";
    const auto barrier = fixture.root / "started";
    REQUIRE(mir::WriteText(script, "[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'started'),'ready'); Start-Sleep -Seconds 20\n"));
    fixture.manifest.slangc = "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe";
    auto &output = fixture.manifest.outputs.front();
    output.arguments = {Utf8(fixture.manifest.slangc), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
        "-File", Utf8(script), "-o", output.output, "-reflection-json", output.reflection, "-depfile", output.depfile};
    mir::ShaderGenerationStore store(fixture.shaders);
    uint64_t generation;
    REQUIRE(store.BeginPublish(&generation));
    const auto text = mir::SerializeShaderManifest(fixture.manifest);
    REQUIRE(store.StageFile("shader_manifest.json", std::as_bytes(std::span(text.data(), text.size()))));
    REQUIRE(store.Finish());
    REQUIRE(store.PublishPointer());
    store.Unlock();
    mir::ShaderHotReload watcher;
    watcher.Initialize(fixture.root, fixture.shaders, fixture.Backend(), fixture.jobs);
    REQUIRE(watcher.Enabled());
    REQUIRE(mir::WriteText(fixture.include, "float Value() { return 2.0; }\n"));
    REQUIRE(Pump(watcher, [&] { return std::filesystem::exists(barrier); }));
    const auto started = std::chrono::steady_clock::now();
    watcher.Shutdown();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(fixture.commits == 0);
    CHECK(store.CurrentId() == generation);
    CHECK(std::filesystem::is_empty(fixture.shaders / ".jobs"));
}

TEST_CASE("Watcher retries a busy publication without blocking frames or losing newer saves", "[editor][shader][watcher]")
{
    Fixture fixture;
    fixture.Seed();
    mir::ShaderHotReload watcher;
    watcher.Initialize(fixture.root, fixture.shaders, fixture.Backend(), fixture.jobs);
    mir::ShaderGenerationStore lock(fixture.shaders);
    uint64_t id;
    REQUIRE(lock.BeginPublish(&id));
    REQUIRE(mir::WriteText(fixture.include, "float Value() { return 2.0; }\n"));
    REQUIRE(Pump(watcher, [&] { return watcher.LastDiagnostic().find("acquire") != std::string::npos; }));
    const auto started = std::chrono::steady_clock::now();
    watcher.Update();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100));
    REQUIRE(mir::WriteText(fixture.include, "float Value() { return 3.0; }\n"));
    watcher.Update();
    lock.Abort();
    lock.Unlock();
    REQUIRE(Pump(watcher, [&] { return fixture.commits == 1; }));
    CHECK(fixture.prepares == 1);
}

TEST_CASE("Watcher rebases a completed job without dropping externally published outputs", "[editor][shader][watcher]")
{
    Fixture fixture;
    fixture.Seed();
    mir::ShaderHotReload watcher;
    watcher.Initialize(fixture.root, fixture.shaders, fixture.Backend(), fixture.jobs);
    mir::ShaderGenerationStore external(fixture.shaders);
    uint64_t generation;
    REQUIRE(external.BeginPublish(&generation));
    REQUIRE(mir::WriteText(fixture.include, "float Value() { return 2.0; }\n"));
    REQUIRE(Pump(watcher, [&] { return watcher.LastDiagnostic().find("acquire") != std::string::npos; }));
    auto output = fixture.manifest.outputs.front();
    output.output = "external.spv";
    output.reflection = "external.spv.reflection.json";
    output.depfile = "external.spv.d";
    fixture.manifest.outputs.push_back(output);
    const auto manifestText = mir::SerializeShaderManifest(fixture.manifest);
    REQUIRE(external.StageFile("shader_manifest.json", std::as_bytes(std::span(manifestText.data(), manifestText.size()))));
    const auto before = external.GenerationPath(*external.CurrentId());
    for (const auto &suffix : {"", ".reflection.json", ".d"})
        REQUIRE(external.StageFromFile(std::string("external.spv") + suffix, before / (std::string("probe.spv") + suffix)));
    REQUIRE(external.Finish());
    REQUIRE(external.PublishPointer());
    external.Unlock();
    REQUIRE(Pump(watcher, [&] { return fixture.commits == 1; }));
    const auto finalManifest = mir::LoadShaderManifest(external.GenerationPath(*external.CurrentId()) / "shader_manifest.json");
    REQUIRE(finalManifest);
    CHECK(finalManifest->outputs.size() == 2);
    CHECK(finalManifest->Find("external.spv"));
}
#endif

TEST_CASE("Watcher cancels queued compilation without waiting for unrelated shared jobs", "[editor][shader][watcher]")
{
    Fixture fixture;
    fixture.Seed();
    std::promise<void> release, started;
    auto released = release.get_future().share();
    auto running = started.get_future();
    mir::BackgroundJobQueue other(fixture.jobs, 1);
    {
        struct ReleaseWorker
        {
            std::promise<void> &release;
            ~ReleaseWorker() { release.set_value(); }
        } releaseWorker{release};
        other.Post([released, &started] { started.set_value(); released.wait(); });
        REQUIRE(running.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        mir::ShaderHotReload watcher;
        watcher.Initialize(fixture.root, fixture.shaders, fixture.Backend(), fixture.jobs);
        REQUIRE(watcher.Enabled());
        REQUIRE(mir::WriteText(fixture.include, "float Value() { return 2.0; }\n"));
        const auto jobs = fixture.shaders / ".jobs";
        REQUIRE(Pump(watcher, [&] { return std::filesystem::exists(jobs) && !std::filesystem::is_empty(jobs); }));
        const auto before = std::chrono::steady_clock::now();
        watcher.Shutdown();
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
        CHECK(fixture.commits == 0);
        CHECK(std::filesystem::is_empty(jobs));
    }
    other.WaitIdle();
    // Closing the shader lane leaves the executor usable by other consumers.
    bool completed = false;
    other.Post([&] { completed = true; });
    other.WaitIdle();
    CHECK(completed);
    mir::ShaderHotReload restarted;
    restarted.Initialize(fixture.root, fixture.shaders, fixture.Backend(), fixture.jobs);
    REQUIRE(mir::WriteText(fixture.include, "float Value() { return 3.0; }\n"));
    REQUIRE(Pump(restarted, [&] { return fixture.commits == 1; }));
}

TEST_CASE("Watcher compiles real manifest frontends and owns new material layout outputs", "[editor][shader][watcher]")
{
    const auto frontend = GENERATE(std::string("mesh"), std::string("terrain"));
    CAPTURE(frontend);
    Fixture fixture;
    fixture.Seed();
    std::string error;
    const auto production = mir::LoadShaderManifest(MIR_EDITOR_SHADER_MANIFEST, &error);
    INFO(error);
    REQUIRE(production);
    fixture.manifest.frontendTemplates = production->frontendTemplates;
    fixture.manifest.commonArguments = production->commonArguments;
    const auto content = fixture.root / "content";
    std::filesystem::create_directories(content);
    fixture.manifest.assetRoots = {{content, "custom"}};
    const auto definition = mir::ReadText(production->workingDirectory / "assets/materials/default.slang");
    REQUIRE(definition);
    const auto shader = content / "material.slang";
    REQUIRE(mir::WriteText(shader, *definition));
    mir::ShaderGenerationStore store(fixture.shaders);
    uint64_t generation;
    REQUIRE(store.BeginPublish(&generation));
    const auto manifestText = mir::SerializeShaderManifest(fixture.manifest);
    REQUIRE(store.StageFile("shader_manifest.json", std::as_bytes(std::span(manifestText.data(), manifestText.size()))));
    REQUIRE(store.Finish());
    REQUIRE(store.PublishPointer());
    store.Unlock();

    std::map<std::string, uint64_t> accepted, pending;
    const auto seedBytes = mir::ReadBytes(store.GenerationPath(generation) / "probe.spv");
    REQUIRE(seedBytes);
    accepted["probe.spv"] = mir::HashShaderContent(*seedBytes);
    auto backend = fixture.Backend();
    backend.prepare = [&](const auto &directory, std::string *diagnostic)
    {
        const auto manifest = mir::LoadShaderManifest(directory / "shader_manifest.json", diagnostic);
        if (!manifest) return false;
        pending.clear();
        std::vector<std::string> managed;
        for (const auto &output : manifest->outputs)
        {
            const auto bytes = mir::ReadBytes(directory / std::filesystem::u8path(output.output));
            if (!bytes) return false;
            pending[output.output] = mir::HashShaderContent(*bytes);
            // Runtime pipelines own non-layout outputs; use the renderer's
            // metadata rule for reflection-only layouts and real coverage check.
            if (output.entry != "layoutMain" || output.type == "surface") managed.push_back(output.output);
        }
        const auto coverage = mir::ValidateShaderOwnerCoverage(managed, accepted, pending);
        if (!coverage.compatible) *diagnostic = coverage.differences.front();
        return coverage.compatible;
    };
    backend.commit = [&] { accepted = pending; ++fixture.commits; };
    mir::ShaderHotReload watcher;
    watcher.Initialize(fixture.root, fixture.shaders, std::move(backend), fixture.jobs);
    REQUIRE(watcher.Enabled());
    const auto material = nlohmann::json{{"version", 1}, {"shader", {{"file", "material.slang"}, {"type", "DefaultMaterial"}}},
        {"frontends", {frontend}}, {"parameters", nlohmann::json::object()}};
    REQUIRE(mir::WriteText(content / "new.material.json", material.dump()));
    const auto publish = [&](int expectedCommits)
    {
        REQUIRE(Pump(watcher, [&] { return fixture.commits == expectedCommits || !watcher.LastDiagnostic().empty(); }));
        INFO(watcher.LastDiagnostic());
        REQUIRE(fixture.commits == expectedCommits);
    };
    const auto layout = [&]() -> mir::MaterialLayout
    {
        const auto directory = store.GenerationPath(*store.CurrentId());
        const auto manifest = mir::LoadShaderManifest(directory / "shader_manifest.json");
        REQUIRE(manifest);
        std::optional<mir::MaterialLayout> result;
        for (const auto &output : manifest->outputs)
        {
            if (output.frontend != frontend) continue;
            CHECK(output.type == "surface");
            CHECK(output.specialize == (output.entry == "vertexMain" ? "" : "DefaultMaterial"));
            if (output.entry != "layoutMain") continue;
            const auto reflection = mir::ReadText(directory / std::filesystem::u8path(output.reflection));
            REQUIRE(reflection);
            result = mir::ParseMaterialLayout(*reflection, &error);
            INFO(error);
            REQUIRE(result);
        }
        REQUIRE(result);
        return *result;
    };
    publish(1);
    const auto before = layout();
    auto changed = *definition;
    const auto field = changed.find("float4 surfaceParams;");
    REQUIRE(field != std::string::npos);
    changed.insert(field, "float4 reloadField;\n    ");
    REQUIRE(mir::WriteText(shader, changed));
    publish(2);
    const auto after = layout();
    CHECK(after.size > before.size);
    CHECK(after.fields.size() == before.fields.size() + 1);

    if (frontend == "terrain")
    {
        const auto layeredPath = content / "relief.terrainmaterial.json";
        const std::string layered = R"({"version":1,"blend":"height","layers":[
            {"name":"base","material":"new.material.json"},{"name":"grass","material":"new.material.json"}]})";
        REQUIRE(mir::WriteText(layeredPath, layered));
        std::optional<std::string> result;
        watcher.QueueTerrainMaterial(layeredPath, [&](std::string diagnostic) { result = std::move(diagnostic); });
        REQUIRE(Pump(watcher, [&] { return result.has_value(); }));
        INFO(*result);
        REQUIRE(result->empty());
        REQUIRE(fixture.commits == 3);
        const auto published = store.CurrentId();
        REQUIRE(published);
        struct RestoreShaderRoot
        {
            std::filesystem::path previous = mir::ShaderRootOverride();
            ~RestoreShaderRoot() { mir::SetShaderRootOverride(std::move(previous)); }
        } restore;
        mir::SetShaderRootOverride(store.GenerationPath(*published));
        const auto catalog = mir::LoadSurfaceMaterialCatalog(&error);
        INFO(error);
        REQUIRE(catalog);
        const auto *registration = catalog->FindByAsset("content/relief.terrainmaterial.json", "terrain");
        REQUIRE(registration);
        CHECK(registration->type == "Layered2<DefaultMaterial, DefaultMaterial>");
        CHECK(mir::ReadTerrainMaterial(layered, registration->asset, &*catalog, &error));
        CHECK(mir::PackSurfaceMaterialRecord(*registration, &error));
        CHECK(std::filesystem::is_regular_file(mir::ShaderPath(registration->stem + ".fragmentMain.spv")));
        CHECK(std::filesystem::is_regular_file(mir::ShaderPath(registration->stem + ".closestHitSurface.spv")));

        REQUIRE(mir::WriteText(layeredPath, "{}"));
        result.reset();
        watcher.QueueTerrainMaterial(layeredPath, [&](std::string diagnostic) { result = std::move(diagnostic); });
        REQUIRE(Pump(watcher, [&] { return result.has_value(); }));
        CHECK_FALSE(result->empty());
        CHECK(store.CurrentId() == published);
    }
}

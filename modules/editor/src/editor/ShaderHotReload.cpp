/**
 * @file ShaderHotReload.cpp
 * @author Jackey Cen
 * @brief Asynchronous shader jobs and immutable generation publication.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */
#include <editor/ShaderHotReload.h>

#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <renderer/VulkanRenderer.h>
#include <renderer/TerrainMaterialAsset.h>
#include <jobs/BackgroundJobQueue.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr auto kSettleTime = std::chrono::milliseconds(150);
        std::string Utf8(const std::filesystem::path &path)
        {
            const auto bytes = path.generic_u8string();
            return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
        }
        std::filesystem::path Resolve(std::string_view value, const std::filesystem::path &cwd)
        {
            auto path = std::filesystem::u8path(value);
            if (path.is_relative()) path = cwd / path;
            return std::filesystem::weakly_canonical(path);
        }
        bool Within(const std::filesystem::path &path, const std::filesystem::path &root)
        {
            const auto relative = path.lexically_relative(root);
            return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
        }
        std::filesystem::path FindAsset(std::string_view logical, const std::vector<std::filesystem::path> &roots)
        {
            const auto relative = std::filesystem::u8path(logical);
            if (!relative.is_absolute() && std::none_of(relative.begin(), relative.end(),
                [](const auto &part) { return part == ".."; }))
                for (const auto &root : roots)
                {
                    const auto candidate = std::filesystem::weakly_canonical(root / relative);
                    if (Within(candidate, root) && std::filesystem::is_regular_file(candidate)) return candidate;
                }
            throw std::runtime_error("asset is missing under approved roots: " + std::string(logical));
        }
        std::string Name(const std::string &path) { return Utf8(std::filesystem::u8path(path).filename()); }
        std::string ResolveLayerShaders(Json &layers, const Json &materials, const ShaderManifest &manifest,
            const std::vector<std::filesystem::path> &roots, std::vector<std::string> &shaders)
        {
            std::string type = "Layered" + std::to_string(layers.size()) + "<";
            for (auto &layer : layers)
            {
                const auto &asset = layer.at("asset").get_ref<const std::string &>();
                const auto child = std::find_if(materials.begin(), materials.end(), [&](const auto &entry)
                    { return entry.at("asset") == asset && entry.at("frontend") == "terrain"; });
                if (child == materials.end())
                    throw std::runtime_error("Layer '" + layer.at("name").get<std::string>() +
                        "' has no terrain shader registration: " + asset);
                layer["type"] = child->at("type");
                layer["shader"] = child->at("shader");
                const auto path = Resolve(layer.at("shader").get<std::string>(), manifest.workingDirectory);
                if (std::ranges::none_of(roots, [&](const auto &root) { return Within(path, root); }))
                    throw std::runtime_error("Layer shader is outside approved roots: " + Utf8(path));
                if (!shaders.empty()) type += ", ";
                type += layer.at("type").get_ref<const std::string &>();
                const auto shader = Utf8(path);
                if (std::ranges::find(shaders, shader) == shaders.end()) shaders.push_back(shader);
            }
            return type + ">";
        }
        void AppendMaterialOutputs(const ShaderManifest &manifest, const std::string &frontend,
            const std::string &stem, const std::string &type, const std::vector<std::string> &shaders,
            bool layered, std::vector<ShaderManifestOutput> &outputs)
        {
            const auto templateIt = std::find_if(manifest.frontendTemplates.begin(), manifest.frontendTemplates.end(),
                [&](const auto &candidate) { return candidate.frontend == frontend; });
            if (templateIt == manifest.frontendTemplates.end()) throw std::runtime_error("no registered frontend template for " + frontend);
            const auto replace = [&](std::string argument)
                {
                    const std::pair<std::string, std::string> tokens[]{
                        {"{type}", type}, {"{stem}", stem}};
                    for (const auto &[token, value] : tokens)
                        for (auto position = argument.find(token); position != std::string::npos; position = argument.find(token))
                            argument.replace(position, token.size(), value);
                    if (argument.find('{') != std::string::npos) throw std::runtime_error("unknown frontend placeholder");
                    return argument;
                };
            const std::array<const std::vector<std::string> *, 7> stages{
                &templateIt->vertex, &templateIt->fragment, &templateIt->raytracing,
                &templateIt->layout, &templateIt->surfaceFragment, &templateIt->giRaytracing, &templateIt->probeRaytracing};
            const std::array<std::string, 7> entries{"vertexMain", "fragmentMain", "closestHitSurface", "layout", "fragmentMain", "closestHitSurface", "closestHitSurface"};
            const std::array<std::string, 7> kinds{"vertex", "fragment", "closesthit", "compute", "fragment", "closesthit", "closesthit"};
            for (size_t stage = 0; stage < stages.size(); ++stage)
            {
                if (stages[stage]->empty()) continue;
                ShaderManifestOutput output;
                output.type = "surface";
                output.frontend = frontend;
                output.output = stem + (stage == 4 ? ".surface." : stage == 5 ? ".gi." : stage == 6 ? ".probe." : ".") + entries[stage] + ".spv";
                output.reflection = output.output + ".reflection.json";
                output.depfile = output.output + ".d";
                output.stage = kinds[stage];
                output.entry = stage == 3 ? "layoutMain" : entries[stage];
                output.specialize = stage == 0 ? "" : type;
                output.arguments = {Utf8(manifest.slangc)};
                output.arguments.insert(output.arguments.end(), manifest.commonArguments.begin(), manifest.commonArguments.end());
                bool materialSource = false;
                for (const auto &argument : *stages[stage])
                {
                    if (argument == "{materialSource}")
                    {
                        materialSource = true;
                        if (stage != 0 || !layered)
                        {
                            output.sources.insert(output.sources.end(), shaders.begin(), shaders.end());
                            output.arguments.insert(output.arguments.end(), shaders.begin(), shaders.end());
                        }
                        continue;
                    }
                    auto expanded = replace(argument);
                    if (expanded.ends_with(".slang")) output.sources.push_back(Utf8(Resolve(expanded, manifest.workingDirectory)));
                    output.arguments.push_back(std::move(expanded));
                }
                if (!materialSource) throw std::runtime_error("frontend template omits material source");
                const std::vector<std::string> tail{"-reflection-json", output.reflection, "-depfile", output.depfile, "-o", output.output};
                output.arguments.insert(output.arguments.end(), tail.begin(), tail.end());
                if (std::none_of(outputs.begin(), outputs.end(), [&](const auto &existing) { return existing.output == output.output; }))
                    outputs.push_back(std::move(output));
            }
        }
        void RemoveJob(const std::filesystem::path &path)
        {
            if (path.empty()) return;
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
    ShaderHotReload::ShaderHotReload() = default;
    ShaderHotReload::~ShaderHotReload() { Shutdown(); }

    void ShaderHotReload::Initialize(const std::filesystem::path &projectRoot, VulkanRenderer *renderer, JobExecutor &jobs)
    {
        if (renderer == nullptr) return;
        Initialize(projectRoot, ExecutableDir() / "shaders", {
            [renderer] { return renderer->ShaderReloadAvailable(); },
            [renderer](const auto &directory, auto *error) { return renderer->PrepareShaderReload(directory, error); },
            [renderer] { renderer->CommitPreparedShaderReload(); },
            [renderer] { renderer->DiscardPreparedShaderReload(); }}, jobs);
    }

    void ShaderHotReload::Initialize(const std::filesystem::path &projectRoot,
        const std::filesystem::path &shaderRoot, ShaderReloadBackend backend, JobExecutor &jobs)
    {
        Shutdown();
        mProjectRoot = std::filesystem::weakly_canonical(projectRoot);
        mShaderDir = shaderRoot;
        mBackend = std::move(backend);
        mEnabled = mBackend.available && mBackend.prepare && mBackend.commit && mBackend.discard && RefreshManifest();
        if (mEnabled)
        {
            mQueue = std::make_unique<BackgroundJobQueue>(jobs, 1);
            PollMaterials();
        }
        else MIR_LOG_INFO("Shader hot reload disabled: {}", mLastDiagnostic);
    }

    void ShaderHotReload::DropReady()
    {
        if (mReady) RemoveJob(mReady->directory);
        mReady.reset();
    }
    void ShaderHotReload::Shutdown()
    {
        mEnabled = false;
        mCancel = true;
        // Close only our lane: queued callbacks are cancelled, running compilers
        // observe mCancel, and borrowed watcher state stays alive until they finish.
        mQueue.reset();
        mCompilation = {};
        RemoveJob(mCompilationDirectory);
        mCompilationDirectory.clear();
        DropReady();
        mPendingSources.clear();
        mPendingMaterials.clear();
        while (!mMaterialRequests.empty())
            CompleteMaterial(mMaterialRequests.begin()->first, "Shader publication stopped.");
        mMaterialFiles.clear();
        mMaterialsScanned = false;
        mMaterialEpoch = 0;
        mDependencies.Clear();
    }

    bool ShaderHotReload::RefreshManifest()
    {
        ShaderGenerationStore store(mShaderDir);
        const auto current = ShaderGenerationStore::ReadCurrent(mShaderDir);
        mReadDirectory = current ? store.GenerationPath(current->mId) : mShaderDir;
        const auto manifest = LoadShaderManifest(mReadDirectory / "shader_manifest.json", &mLastDiagnostic);
        if (!manifest) return false;
        mManifest = *manifest;
        if (!std::filesystem::is_regular_file(mManifest.slangc))
        {
            mLastDiagnostic = "slangc is missing: " + Utf8(mManifest.slangc);
            return false;
        }
        for (auto &output : mManifest.outputs)
        {
            output.output = Name(output.output);
            output.reflection = Name(output.reflection);
            output.depfile = Name(output.depfile);
            for (auto &source : output.sources) source = Utf8(Resolve(source, mManifest.workingDirectory));
        }
        mLoadedGeneration = store.CurrentId().value_or(0);
        RebuildDependencies(mReadDirectory);
        return true;
    }
    void ShaderHotReload::RebuildDependencies(const std::filesystem::path &directory)
    {
        auto readView = mManifest;
        // A changed material type may leave its old immutable outputs in the generation.
        // Watch the specializations the current catalog still uses.
        if (const auto text = ReadText(directory / "surface_materials.json"))
        {
            const auto catalog = Json::parse(*text, nullptr, false);
            if (catalog.contains("materials") && catalog.at("materials").is_array())
            {
                std::set<std::string> stems;
                for (const auto &material : catalog.at("materials"))
                    if (material.contains("stem") && material.at("stem").is_string())
                        stems.insert(material.at("stem").get<std::string>() + ".");
                std::erase_if(readView.outputs, [&](const auto &output) {
                    if (output.type != "surface") return false;
                    const auto name = Name(output.output);
                    return std::ranges::none_of(stems, [&](const auto &stem) { return name.starts_with(stem); });
                });
            }
        }
        for (auto &output : readView.outputs)
            output.depfile = Utf8(directory / std::filesystem::u8path(Name(output.depfile)));
        mDependencies.RebuildFromManifest(readView, &mLastDiagnostic);
        mDependencies.Poll();
    }
    std::vector<std::filesystem::path> ShaderHotReload::ApprovedRoots() const
    {
        std::vector<std::filesystem::path> roots;
        const auto engineRoot = Resolve("assets", mManifest.workingDirectory);
        for (const auto &entry : mManifest.assetRoots)
        {
            const auto root = Resolve(Utf8(entry.root), mManifest.workingDirectory);
            if (root != engineRoot && !Within(root, mProjectRoot)) continue;
            if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
        }
        return roots;
    }
    void ShaderHotReload::PollMaterials()
    {
        std::map<std::string, ShaderGenerationFile> found;
        for (const auto &root : ApprovedRoots())
        {
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator it(root,
                std::filesystem::directory_options::skip_permission_denied, error), end; it != end; it.increment(error))
            {
                if (error) { error.clear(); continue; }
                if (!it->is_regular_file(error) || !Utf8(it->path().filename()).ends_with(".material.json")) continue;
                const auto path = std::filesystem::weakly_canonical(it->path());
                if (!Within(path, root)) continue;
                const auto key = Utf8(path);
                const auto snapshot = SnapshotShaderFile(path, key);
                if (!snapshot) continue;
                found.emplace(key, *snapshot);
                const auto before = mMaterialFiles.find(key);
                if (mMaterialsScanned && (before == mMaterialFiles.end() ||
                    before->second.mContentHash != snapshot->mContentHash || before->second.mSize != snapshot->mSize))
                {
                    mPendingMaterials.insert(key);
                    ++mMaterialEpoch;
                    mPendingSince = std::chrono::steady_clock::now();
                }
            }
        }
        if (mMaterialsScanned)
            for (const auto &[path, snapshot] : mMaterialFiles)
                if (!found.contains(path))
                {
                    ++mMaterialEpoch;
                    mLastDiagnostic = "material source was removed; keeping the last-good generation: " + path;
                }
        mMaterialFiles = std::move(found);
        mMaterialsScanned = true;
    }
    void ShaderHotReload::QueueTerrainMaterial(const std::filesystem::path &path,
        std::function<void(std::string)> completion)
    {
        if (!mEnabled || !mBackend.available())
        {
            completion("Shader publication is unavailable. " + mLastDiagnostic);
            return;
        }
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(path, error);
        if (error || !IsTerrainMaterialPath(Utf8(canonical)) ||
            !std::ranges::any_of(ApprovedRoots(), [&](const auto &root) { return Within(canonical, root); }))
        {
            completion("The layered material is outside the shader manifest's asset roots.");
            return;
        }
        const auto key = Utf8(canonical);
        mMaterialRequests[key].push_back(std::move(completion));
        mPendingMaterials.insert(key);
        ++mMaterialEpoch;
        mPendingSince = std::chrono::steady_clock::now();
    }
    void ShaderHotReload::CompleteMaterial(const std::string &name, const std::string &error)
    {
        auto requests = mMaterialRequests.extract(name);
        if (!requests.empty())
            for (auto &complete : requests.mapped()) complete(error);
    }
    void ShaderHotReload::Requeue(const Job &job)
    {
        mPendingSources.insert(job.changedSources.begin(), job.changedSources.end());
        if (!job.materialName.empty()) mPendingMaterials.insert(job.materialName);
        mPendingSince = std::chrono::steady_clock::now();
    }
    void ShaderHotReload::StartJob(Job job)
    {
        job.manifest = mManifest;
        job.parent = mLoadedGeneration;
        job.revision = mDependencies.Revision();
        job.materialEpoch = mMaterialEpoch;
        job.directory = UniqueShaderJobDirectory(mShaderDir / ".jobs");
        mCancel = false;
        mCompilationDirectory = job.directory;
        mCompilation = mQueue->Submit([this, job = std::move(job)]() mutable
            {
                for (const auto &output : job.outputs)
                {
                    auto artifact = CompileShaderArtifact(job.manifest, output, job.directory, {}, &mCancel);
                    job.superseded |= artifact.superseded || artifact.cancelled;
                    if (!artifact.success) { job.error = output.output + ": " + artifact.diagnostic; break; }
                    job.inputs.insert(job.inputs.end(), artifact.inputs.begin(), artifact.inputs.end());
                    job.artifacts.push_back(std::move(artifact));
                }
                return std::move(job);
            });
    }
    void ShaderHotReload::PollJob()
    {
        if (mCompilation.valid() && mCompilation.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try { mReady = mCompilation.get(); }
            catch (...)
            {
                RemoveJob(mCompilationDirectory);
                mCompilationDirectory.clear();
                throw;
            }
            mCompilationDirectory.clear();
        }
        if (!mReady) return;
        auto &job = *mReady;
        std::string error;
        if (job.superseded || !mDependencies.IsCurrent(job.revision) || job.materialEpoch != mMaterialEpoch ||
            !VerifyShaderSnapshot(job.inputs, &error))
        {
            Requeue(job);
            mLastDiagnostic = "shader batch superseded by newer source content";
            DropReady();
            return;
        }
        if (!job.error.empty())
        {
            mLastDiagnostic = job.error.substr(0, 4000);
            MIR_LOG_ERROR("Shader reload rejected: {}", mLastDiagnostic);
            CompleteMaterial(job.materialName, mLastDiagnostic);
            DropReady();
            return;
        }
        mPublishBusy = false;
        if (PublishJob(job)) mLastDiagnostic.clear();
        if (!mPublishBusy)
        {
            if (!mPendingMaterials.contains(job.materialName)) CompleteMaterial(job.materialName, mLastDiagnostic);
            DropReady();
        }
    }
    void ShaderHotReload::Update()
    {
        if (!mEnabled || !mBackend.available()) return;
        try
        {
            PollMaterials();
            const auto changed = mDependencies.Poll();
            if (!changed.empty())
            {
                mPendingSources.insert(changed.begin(), changed.end());
                mPendingSince = std::chrono::steady_clock::now();
            }
            if (mCompilation.valid() || mReady)
            {
                if (!changed.empty() || (mReady && mReady->materialEpoch != mMaterialEpoch)) mCancel = true;
                PollJob();
                return;
            }
            if (mPendingSources.empty() && mPendingMaterials.empty()) return;
            if (std::chrono::steady_clock::now() - mPendingSince < kSettleTime) return;
            ShaderGenerationStore current(mShaderDir);
            if (current.CurrentId().value_or(0) != mLoadedGeneration && !RefreshManifest()) return;
            Job job;
            const auto manifestPath = mReadDirectory / "shader_manifest.json";
            if (const auto snapshot = SnapshotShaderFile(manifestPath, Utf8(manifestPath))) job.inputs.push_back(*snapshot);
            if (!mPendingMaterials.empty())
            {
                job.materialName = *mPendingMaterials.begin();
                mPendingMaterials.erase(mPendingMaterials.begin());
                if (!ExpandMaterial(std::filesystem::u8path(job.materialName), job))
                {
                    CompleteMaterial(job.materialName, mLastDiagnostic);
                    return;
                }
            }
            else
            {
                job.changedSources.assign(mPendingSources.begin(), mPendingSources.end());
                mPendingSources.clear();
                for (const auto &path : mDependencies.AffectedOutputs(job.changedSources))
                    if (const auto *output = mManifest.Find(path)) job.outputs.push_back(*output);
            }
            if (job.outputs.empty()) return;
            std::set<std::string> sources;
            for (auto &output : job.outputs)
            {
                if (job.materialName.empty())
                    for (const auto &dependency : mDependencies.DependenciesOf(output.output))
                        if (std::find(output.sources.begin(), output.sources.end(), dependency) == output.sources.end())
                            output.sources.push_back(dependency);
                sources.insert(output.sources.begin(), output.sources.end());
            }
            for (const auto &source : sources)
                if (const auto snapshot = SnapshotShaderFile(Resolve(source, mManifest.workingDirectory), source))
                    job.inputs.push_back(*snapshot);
            StartJob(std::move(job));
        }
        catch (const std::exception &exception) { mLastDiagnostic = exception.what(); }
        catch (...) { mLastDiagnostic = "unknown compiler failure"; }
    }
    bool ShaderHotReload::PublishJob(Job &job)
    {
        ShaderGenerationStore store(mShaderDir);
        uint64_t generation = 0;
        std::string error;
        if (!store.BeginPublish(&generation, &error, false))
        {
            mLastDiagnostic = error;
            mPublishBusy = error == "could not acquire the shader publication lock";
            return false;
        }
        if (store.CurrentId().value_or(0) != job.parent)
        {
            RefreshManifest();
            Requeue(job);
            mLastDiagnostic = "external publication changed the shader parent; recompiling against its manifest";
            return false;
        }
        auto candidateManifest = mManifest;
        for (const auto &output : job.outputs)
        {
            auto found = std::find_if(candidateManifest.outputs.begin(), candidateManifest.outputs.end(),
                [&](const auto &existing) { return existing.output == output.output; });
            if (found == candidateManifest.outputs.end()) candidateManifest.outputs.push_back(output);
            else *found = output;
        }
        for (auto &output : candidateManifest.outputs)
        {
            output.output = Name(output.output);
            output.reflection = Name(output.reflection);
            output.depfile = Name(output.depfile);
        }
        const auto stageText = [&](const std::string &name, const std::string &text)
            { return store.StageFile(name, std::as_bytes(std::span(text.data(), text.size())), &error); };
        for (const auto &artifact : job.artifacts)
            for (const auto &path : {artifact.output.output, artifact.output.reflection, artifact.output.depfile})
                if (!store.StageFromFile(Name(path), std::filesystem::u8path(path), &error))
                { mLastDiagnostic = error; return false; }
        if (!stageText("shader_manifest.json", SerializeShaderManifest(candidateManifest)) ||
            (!job.catalogText.empty() && !stageText("surface_materials.json", job.catalogText)))
        { mLastDiagnostic = error; return false; }
        store.SetInputs(job.inputs);
        if (!store.Finish(&error)) { Requeue(job); mLastDiagnostic = error; return false; }
        try
        {
            if (!mBackend.prepare(store.GenerationPath(generation), &error) || !store.PublishPointer(&error))
            {
                mBackend.discard();
                mLastDiagnostic = error;
                return false;
            }
        }
        catch (...) { mBackend.discard(); throw; }
        mBackend.commit();
        store.Unlock();
        mManifest = std::move(candidateManifest);
        mLoadedGeneration = generation;
        mReadDirectory = store.GenerationPath(generation);
        mLastRevision = job.revision;
        RebuildDependencies(mReadDirectory);
        MIR_LOG_INFO("Shader generation {} published with {} outputs", generation, job.outputs.size());
        return true;
    }
    bool ShaderHotReload::ExpandMaterial(const std::filesystem::path &path, Job &job)
    {
        try
        {
            const auto source = SnapshotShaderFile(path, Utf8(path));
            const auto text = ReadText(path);
            if (!source || !text) throw std::runtime_error("material source is missing");
            job.inputs.push_back(*source);
            const auto material = Json::parse(*text);
            if (material.at("version") != 1) throw std::runtime_error("unsupported material version");
            const auto roots = ApprovedRoots();
            std::filesystem::path owner;
            std::string nameSpace;
            for (const auto &root : roots)
                if (Within(path, root) && Utf8(root).size() > Utf8(owner).size()) owner = root;
            if (owner.empty()) throw std::runtime_error("material is outside approved roots");
            for (const auto &root : mManifest.assetRoots)
                if (Resolve(Utf8(root.root), mManifest.workingDirectory) == owner) nameSpace = root.nameSpace;
            std::vector<std::filesystem::path> search{owner};
            for (const auto &root : roots) if (root != owner) search.push_back(root);
            const auto catalogPath = mReadDirectory / "surface_materials.json";
            const auto catalogSnapshot = SnapshotShaderFile(catalogPath, Utf8(catalogPath));
            const auto catalogText = ReadText(catalogPath);
            if (!catalogSnapshot || !catalogText) throw std::runtime_error("surface catalog is missing");
            job.inputs.push_back(*catalogSnapshot);
            auto catalog = Json::parse(*catalogText);
            const auto asset = Utf8(path.lexically_relative(mManifest.workingDirectory));
            std::string type, shaderIdentity;
            std::vector<std::string> shaders;
            Json layers = Json::array(), parameters = Json::object(), frontends;
            if (IsTerrainMaterialPath(asset))
            {
                std::string diagnostic;
                const auto layered = ReadTerrainMaterial(*text, asset, nullptr, &diagnostic);
                if (!layered) throw std::runtime_error(diagnostic);
                shaderIdentity = asset;
                frontends = Json::array({"terrain"});
                parameters = {{"blendMode", TerrainLayerBlendModeValue(layered->blend)},
                    {"heightContrast", layered->heightContrast}};
                for (const auto &layer : layered->layers)
                {
                    const auto child = FindAsset(layer.material, search);
                    const auto logical = Utf8(child.lexically_relative(mManifest.workingDirectory));
                    parameters["layerOffset" + std::to_string(layers.size())] = 0;
                    layers.push_back({{"name", layer.name}, {"asset", logical}});
                }
                type = ResolveLayerShaders(layers, catalog.at("materials"), mManifest, roots, shaders);
            }
            else
            {
                type = material.at("shader").at("type").get<std::string>();
                const auto shader = FindAsset(material.at("shader").at("file").get<std::string>(), search);
                shaderIdentity = Utf8(shader.lexically_relative(mManifest.workingDirectory));
                shaders.push_back(Utf8(shader));
                parameters = material.value("parameters", Json::object());
                frontends = material.at("frontends");
            }
            for (const auto &frontendValue : frontends)
            {
                const auto frontend = frontendValue.get<std::string>();
                auto stem = MaterialShaderStem(nameSpace, type, shaderIdentity, frontend);
                for (const auto &existing : catalog.at("materials"))
                {
                    if (existing.value("frontend", "") != frontend) continue;
                    const bool sameShader = existing.value("type", "") == type &&
                        Resolve(existing.at("shader").get<std::string>(), mManifest.workingDirectory) ==
                            Resolve(shaderIdentity, mManifest.workingDirectory);
                    // A live layered pipeline retains its stem when the editor changes its child types.
                    if (sameShader || (!layers.empty() && existing.value("asset", "") == asset))
                    {
                        stem = existing.at("stem").get<std::string>();
                        break;
                    }
                }
                AppendMaterialOutputs(mManifest, frontend, stem, type, shaders, !layers.empty(), job.outputs);
                Json entry{{"stem", stem}, {"type", type}, {"frontend", frontend}, {"asset", asset},
                    {"shader", shaderIdentity}, {"parameters", parameters},
                    {"reflection", stem + ".layout.spv.reflection.json"}};
                if (!layers.empty()) entry["layers"] = layers;
                auto &materials = catalog.at("materials");
                auto existing = std::find_if(materials.begin(), materials.end(), [&](const auto &candidate)
                    { return candidate.value("asset", "") == asset && candidate.value("frontend", "") == frontend; });
                if (existing == materials.end()) materials.push_back(std::move(entry));
                else *existing = std::move(entry);
            }
            auto &materials = catalog.at("materials");
            materials.erase(std::remove_if(materials.begin(), materials.end(), [&](const auto &entry)
                {
                    return entry.value("asset", "") == asset &&
                        std::find(frontends.begin(), frontends.end(), entry.at("frontend")) == frontends.end();
                }), materials.end());
            // The published catalog is the dependency graph, including parents in other games of an engine material.
            for (auto &parent : materials)
            {
                const auto childLayers = parent.find("layers");
                if (childLayers == parent.end() || !std::any_of(childLayers->begin(), childLayers->end(),
                    [&](const auto &layer) { return layer.at("asset") == asset; })) continue;
                auto updated = *childLayers;
                std::vector<std::filesystem::path> manifestRoots;
                for (const auto &root : mManifest.assetRoots)
                    manifestRoots.push_back(Resolve(Utf8(root.root), mManifest.workingDirectory));
                std::vector<std::string> parentShaders;
                const auto parentType = ResolveLayerShaders(updated, materials, mManifest, manifestRoots, parentShaders);
                if (updated == *childLayers) continue;
                parent["layers"] = std::move(updated);
                parent["type"] = parentType;
                AppendMaterialOutputs(mManifest, "terrain", parent.at("stem").get<std::string>(),
                    parentType, parentShaders, true, job.outputs);
            }
            job.catalogText = catalog.dump(2);
            return true;
        }
        catch (const std::exception &exception)
        {
            mLastDiagnostic = exception.what();
            MIR_LOG_ERROR("Shader reload rejected: {}", mLastDiagnostic);
            return false;
        }
    }
}

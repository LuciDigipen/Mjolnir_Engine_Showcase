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
        std::string Name(const std::string &path) { return Utf8(std::filesystem::u8path(path).filename()); }
        void RemoveJob(const std::filesystem::path &path)
        {
            if (path.empty()) return;
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
    ShaderHotReload::~ShaderHotReload() { Shutdown(); }

    void ShaderHotReload::Initialize(const std::filesystem::path &projectRoot, VulkanRenderer *renderer)
    {
        if (renderer == nullptr) return;
        Initialize(projectRoot, ExecutableDir() / "shaders", {
            [renderer] { return renderer->ShaderReloadAvailable(); },
            [renderer](const auto &directory, auto *error) { return renderer->PrepareShaderReload(directory, error); },
            [renderer] { renderer->CommitPreparedShaderReload(); },
            [renderer] { renderer->DiscardPreparedShaderReload(); }});
    }

    void ShaderHotReload::Initialize(const std::filesystem::path &projectRoot,
        const std::filesystem::path &shaderRoot, ShaderReloadBackend backend)
    {
        Shutdown();
        mProjectRoot = std::filesystem::weakly_canonical(projectRoot);
        mShaderDir = shaderRoot;
        mBackend = std::move(backend);
        mEnabled = mBackend.available && mBackend.prepare && mBackend.commit && mBackend.discard && RefreshManifest();
        if (mEnabled) PollMaterials();
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
        if (mFuture.valid())
        {
            try { RemoveJob(mFuture.get().directory); }
            catch (...) {}
        }
        DropReady();
        mPendingSources.clear();
        mPendingMaterials.clear();
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
        mFuture = std::async(std::launch::async, [this, job = std::move(job)]() mutable
            {
                try
                {
                    for (const auto &output : job.outputs)
                    {
                        auto artifact = CompileShaderArtifact(job.manifest, output, job.directory, {}, &mCancel);
                        job.superseded |= artifact.superseded || artifact.cancelled;
                        if (!artifact.success) { job.error = output.output + ": " + artifact.diagnostic; break; }
                        job.inputs.insert(job.inputs.end(), artifact.inputs.begin(), artifact.inputs.end());
                        job.artifacts.push_back(std::move(artifact));
                    }
                }
                catch (const std::exception &exception) { job.error = exception.what(); }
                catch (...) { job.error = "unknown compiler failure"; }
                return std::move(job);
            });
    }
    void ShaderHotReload::PollJob()
    {
        if (!mReady && mFuture.valid() && mFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            mReady = mFuture.get();
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
            DropReady();
            return;
        }
        mPublishBusy = false;
        if (PublishJob(job)) mLastDiagnostic.clear();
        if (!mPublishBusy) DropReady();
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
            if (mFuture.valid() || mReady)
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
                if (!ExpandMaterial(std::filesystem::u8path(job.materialName), job)) return;
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
            const auto type = material.at("shader").at("type").get<std::string>();
            const auto shaderFile = material.at("shader").at("file").get<std::string>();
            const auto shaderRelative = std::filesystem::u8path(shaderFile);
            if (shaderRelative.is_absolute() || std::any_of(shaderRelative.begin(), shaderRelative.end(),
                [](const auto &part) { return part == ".."; })) throw std::runtime_error("shader path leaves approved roots");
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
            std::filesystem::path shader;
            for (const auto &root : search)
            {
                const auto candidate = std::filesystem::weakly_canonical(root / shaderRelative);
                if (Within(candidate, root) && std::filesystem::is_regular_file(candidate)) { shader = candidate; break; }
            }
            if (shader.empty()) throw std::runtime_error("material shader is missing under approved roots");
            const auto catalogPath = mReadDirectory / "surface_materials.json";
            const auto catalogSnapshot = SnapshotShaderFile(catalogPath, Utf8(catalogPath));
            const auto catalogText = ReadText(catalogPath);
            if (!catalogSnapshot || !catalogText) throw std::runtime_error("surface catalog is missing");
            job.inputs.push_back(*catalogSnapshot);
            auto catalog = Json::parse(*catalogText);
            const auto asset = Utf8(path.lexically_relative(mManifest.workingDirectory));
            const auto shaderIdentity = Utf8(shader.lexically_relative(mManifest.workingDirectory));
            for (const auto &frontendValue : material.at("frontends"))
            {
                const auto frontend = frontendValue.get<std::string>();
                const auto templateIt = std::find_if(mManifest.frontendTemplates.begin(), mManifest.frontendTemplates.end(),
                    [&](const auto &candidate) { return candidate.frontend == frontend; });
                if (templateIt == mManifest.frontendTemplates.end()) throw std::runtime_error("no registered frontend template for " + frontend);
                auto stem = MaterialShaderStem(nameSpace, type, shaderIdentity, frontend);
                for (const auto &existing : catalog.at("materials"))
                    if (existing.value("type", "") == type && existing.value("frontend", "") == frontend &&
                        Resolve(existing.at("shader").get<std::string>(), mManifest.workingDirectory) == shader)
                    { stem = existing.at("stem").get<std::string>(); break; }
                const auto replace = [&](std::string argument)
                    {
                        const std::pair<std::string, std::string> tokens[]{
                            {"{materialSource}", Utf8(shader)}, {"{type}", type}, {"{stem}", stem}};
                        for (const auto &[token, value] : tokens)
                            for (auto position = argument.find(token); position != std::string::npos; position = argument.find(token))
                                argument.replace(position, token.size(), value);
                        if (argument.find('{') != std::string::npos) throw std::runtime_error("unknown frontend placeholder");
                        return argument;
                    };
                const std::array<const std::vector<std::string> *, 4> stages{
                    &templateIt->vertex, &templateIt->fragment, &templateIt->raytracing, &templateIt->layout};
                const std::array<std::string, 4> entries{"vertexMain", "fragmentMain", "closestHitSurface", "layout"};
                const std::array<std::string, 4> kinds{"vertex", "fragment", "closesthit", "compute"};
                for (size_t stage = 0; stage < stages.size(); ++stage)
                {
                    ShaderManifestOutput output;
                    output.type = type;
                    output.frontend = frontend;
                    output.output = stem + "." + entries[stage] + ".spv";
                    output.reflection = output.output + ".reflection.json";
                    output.depfile = output.output + ".d";
                    output.stage = kinds[stage];
                    output.entry = stage == 3 ? "layoutMain" : entries[stage];
                    output.specialize = stage == 0 ? "" : type;
                    output.sources = {Utf8(shader)};
                    output.arguments = {Utf8(mManifest.slangc)};
                    output.arguments.insert(output.arguments.end(), mManifest.commonArguments.begin(), mManifest.commonArguments.end());
                    bool materialSource = false;
                    for (const auto &argument : *stages[stage])
                    {
                        materialSource |= argument == "{materialSource}";
                        auto expanded = replace(argument);
                        if (expanded.ends_with(".slang")) output.sources.push_back(Utf8(Resolve(expanded, mManifest.workingDirectory)));
                        output.arguments.push_back(std::move(expanded));
                    }
                    if (!materialSource) throw std::runtime_error("frontend template omits material source");
                    const std::vector<std::string> tail{"-reflection-json", output.reflection, "-depfile", output.depfile, "-o", output.output};
                    output.arguments.insert(output.arguments.end(), tail.begin(), tail.end());
                    if (std::none_of(job.outputs.begin(), job.outputs.end(), [&](const auto &existing) { return existing.output == output.output; }))
                        job.outputs.push_back(std::move(output));
                }
                Json entry{{"stem", stem}, {"type", type}, {"frontend", frontend}, {"asset", asset},
                    {"shader", shaderIdentity}, {"parameters", material.value("parameters", Json::object())},
                    {"reflection", stem + ".layout.spv.reflection.json"}};
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
                        std::find(material.at("frontends").begin(), material.at("frontends").end(), entry.at("frontend")) ==
                            material.at("frontends").end();
                }), materials.end());
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

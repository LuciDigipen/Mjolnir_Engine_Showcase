/**
 * @file AssetImportManager.cpp
 * @author Jackey Cen
 * @brief Import batching, source watcher and deferred apply of AssetCooker output.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/AssetImportManager.h>

#include <assets/AssetIdRoot.h>
#include <assets/AssetPath.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>
#include <project/ProjectSettings.h>
#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;
        std::string LowerFilename(const std::filesystem::path &path)
        {
            std::string value = path.filename().string();
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }
        bool IsModel(const std::filesystem::path &path)
        {
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == ".gltf" || ext == ".glb";
        }
        bool IsMaterial(const std::filesystem::path &path) { return LowerFilename(path).ends_with(".material.json"); }
        bool IsWatched(const std::filesystem::path &path)
        {
            if (IsMaterial(path))
                return true;
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == ".gltf" || ext == ".glb" || ext == ".bin" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                   ext == ".slang" || ext == ".json";
        }
        // The key AssetCooker uses for a model under the source asset root
        AssetPathResult LogicalModelPath(std::string_view idRoot, const std::filesystem::path &relative)
        {
            return MakeLogicalAssetPath(idRoot, relative.generic_string());
        }
        uint64_t Signature(const std::filesystem::directory_entry &entry)
        {
            std::error_code error;
            const auto time = entry.last_write_time(error).time_since_epoch().count();
            const auto size = entry.file_size(error);
            return static_cast<uint64_t>(time) ^ (size + 0x9e3779b97f4a7c15ull + (static_cast<uint64_t>(time) << 6));
        }
        Json ReadJson(const std::filesystem::path &path, Json fallback = Json::object())
        {
            if (!std::filesystem::exists(path))
                return fallback;
            std::ifstream input(path);
            Json value;
            input >> value;
            return value;
        }
        void WriteJson(const std::filesystem::path &path, const Json &value)
        {
            const auto pending = path.string() + ".pending";
            std::ofstream(pending, std::ios::trunc) << value.dump(2) << '\n';
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::rename(pending, path);
        }
    } // namespace

    void AssetImportManager::Initialize(const GameProject &project)
    {
        mProject = project;
        mView = {};

        const std::filesystem::path bindingPath = ExecutableDir() / "asset_workspace.json";
        std::string bindingError;
        const std::optional<Json> binding = ReadJsonFile(bindingPath, &bindingError);
        if (!binding || !binding->is_object() || binding->value("version", 0) != 1 ||
            !binding->contains("sourceRoot") || !binding->contains("assetCooker"))
        {
            throw SetupError(ErrorCategory::BuildOutput, "asset_workspace.json is missing or unusable", bindingPath,
                binding ? "it needs version 1, sourceRoot and assetCooker" : bindingError,
                "Rebuild: CMake writes asset_workspace.json next to the editor. Running the editor outside "
                "its build tree is not supported.");
        }

        try
        {
            mSourceRoot = binding->at("sourceRoot").get<std::string>();
            mCooker = binding->at("assetCooker").get<std::string>();
        }
        catch (const Json::exception &error)
        {
            throw SetupError(ErrorCategory::BuildOutput, "asset_workspace.json has a key of the wrong type",
                bindingPath, error.what(), "Rebuild: CMake writes asset_workspace.json next to the editor.");
        }

        mCookedRoot = CookedAssetsDir();
        mSourceAssetRoot = mSourceRoot / std::filesystem::path(project.mAssetIdRoot);
        mSourceSettingsPath = mSourceRoot / std::filesystem::path(project.mDir) / "project_settings.json";
        mResultPath = mCookedRoot / ".asset-import-result.json";

        std::error_code ec;
        if (!std::filesystem::is_directory(mSourceAssetRoot, ec))
        {
            throw SetupError(ErrorCategory::System, "The source asset directory is missing", mSourceAssetRoot,
                "it should have been created at startup", "Create the directory, or check its parent is writable.");
        }
        if (!std::filesystem::is_regular_file(mCooker, ec))
        {
            throw SetupError(ErrorCategory::BuildOutput, "AssetCooker is missing", mCooker,
                "asset_workspace.json points at a file that does not exist", "Rebuild to produce AssetCooker.");
        }

        const std::optional<Json> settings = ReadProjectSettings(mSourceSettingsPath);

        mView.mWatchedRoot = mSourceAssetRoot.generic_string();
        if (settings)
        {
            const Json assets = settings->value("assets", Json::object());
            mView.mAutoImport = assets.is_object() && assets.value("autoImport", false);
        }
        PollWatcher();
        RefreshView();
    }

    void AssetImportManager::QueueFullCook()
    {
        mQueued.fullCook = true;
        MIR_LOG_INFO("Asset import: no cooked manifest, cooking every asset under '{}'", mSourceAssetRoot.string());
    }

    bool AssetImportManager::QueueChanged(const std::filesystem::path &path)
    {
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(mSourceAssetRoot,error);
        if (error) return false;
        const auto candidate = std::filesystem::weakly_canonical(path,error);
        if (error) return false;
        auto a=root.begin(), b=candidate.begin();
        for (;a!=root.end() && b!=candidate.end() && *a==*b;++a,++b) {}
        if (a!=root.end() || b==candidate.end()) return false;
        const std::filesystem::directory_entry entry(candidate,error);
        if (!error && entry.is_regular_file(error))
            mWatched[candidate] = {Signature(entry),std::chrono::steady_clock::now(),false};
        mQueued.changed.insert(candidate);
        mRunningStale |= mView.mProcessRunning;
        return true;
    }

    bool AssetImportManager::SourceHasAssets() const
    {
        std::error_code ec;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(mSourceAssetRoot, ec))
        {
            std::error_code entryEc;
            if (entry.is_regular_file(entryEc) && entry.path().filename() != ".gitkeep")
                return true;
        }
        return false;
    }

    void AssetImportManager::Shutdown()
    {
        if (mProcess.Running())
            mProcess.Terminate();
        mView.mProcessRunning = false;
    }

    void AssetImportManager::SetStatus(const std::string &model, AssetImportStatus status, std::string diagnostic)
    {
        mViewDirty = true;
        mStatus[model] = status;
        if (!diagnostic.empty())
            mDiagnostics[model] = std::move(diagnostic);
    }

    void AssetImportManager::Handle(const AssetImportAction &action)
    {
        if (action.mKind == AssetImportActionKind::None)
            return;
        mView.mLastError.clear();
        switch (action.mKind)
        {
        case AssetImportActionKind::Import:
            QueueExternalImport(action);
            break;
        case AssetImportActionKind::Reimport:
        case AssetImportActionKind::Retry:
            if (IsMaterial(action.mModelPath))
                mQueued.materials.insert(action.mModelPath);
            else
                mQueued.models.insert(action.mModelPath);
            SetStatus(action.mModelPath, AssetImportStatus::Queued);
            break;
        case AssetImportActionKind::ForceReimport:
            if (IsMaterial(action.mModelPath))
                mQueued.materials.insert(action.mModelPath);
            else
                mQueued.models.insert(action.mModelPath);
            mQueued.force = true;
            SetStatus(action.mModelPath, AssetImportStatus::Queued);
            break;
        case AssetImportActionKind::Remove:
            mQueued.removals.insert(action.mModelPath);
            SetStatus(action.mModelPath, AssetImportStatus::Queued);
            break;
        case AssetImportActionKind::Move:
            mQueued.moves[action.mModelPath] = action.mTargetDirectory;
            SetStatus(action.mModelPath, AssetImportStatus::Queued);
            break;
        case AssetImportActionKind::Cancel:
            if (mProcess.Running())
            {
                mCancelRequested = true;
                mProcess.Terminate();
            }
            else
            {
                for (const auto &model : mQueued.models)
                    SetStatus(model, AssetImportStatus::Canceled);
                for (const auto &material : mQueued.materials)
                    SetStatus(material, AssetImportStatus::Canceled);
                for (const auto &model : mQueued.removals)
                    SetStatus(model, AssetImportStatus::Canceled);
                for (const auto &[model, destination] : mQueued.moves)
                    SetStatus(model, AssetImportStatus::Canceled);
                mQueued.Clear();
            }
            break;
        case AssetImportActionKind::SetAutoImport:
            mView.mAutoImport = action.mEnabled;
            SaveAutoImport();
            break;
        case AssetImportActionKind::None:
            break;
        }
        RefreshView();
    }

    std::filesystem::path AssetImportManager::UniqueImportTarget(const std::filesystem::path &source) const
    {
        const std::string stem = source.stem().string();
        std::filesystem::path target = std::filesystem::path("models") / stem;
        for (int suffix = 2; std::filesystem::exists(mSourceAssetRoot / target); ++suffix)
            target = std::filesystem::path("models") / (stem + "-" + std::to_string(suffix));
        return target;
    }

    void AssetImportManager::QueueExternalImport(const AssetImportAction &action)
    {
        try
        {
            auto source = std::filesystem::weakly_canonical(action.mExternalSource);
            std::filesystem::path package;
            if (!action.mPackageRoot.empty())
                package = std::filesystem::weakly_canonical(action.mPackageRoot);
            if (std::filesystem::is_directory(source))
            {
                // A dropped folder is the package root
                std::vector<std::filesystem::path> entries;
                for (const auto &entry : std::filesystem::recursive_directory_iterator(source))
                    if (entry.is_regular_file() && IsModel(entry.path()))
                        entries.push_back(entry.path());
                if (entries.empty())
                    throw std::runtime_error("no .gltf or .glb file in " + source.string());
                if (entries.size() > 1)
                    throw std::runtime_error(source.string() + " holds " + std::to_string(entries.size()) +
                                             " glTF files; drop the one you want instead of the folder");
                package = source;
                source = entries.front();
            }
            if (!std::filesystem::is_regular_file(source))
                throw std::runtime_error("no such file: " + source.string());
            if (!IsModel(source))
                throw std::runtime_error("not a .gltf or .glb file: " + source.string());

            // Already inside the project: re-import it in place rather than
            // copying a second instance of the same package.
            const auto relative = source.lexically_relative(std::filesystem::weakly_canonical(mSourceAssetRoot));
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
            {
                const AssetPathResult logical = LogicalModelPath(mProject.mAssetIdRoot, relative);
                if (!logical.Ok())
                    throw std::runtime_error(source.string() + ": " + std::string(Describe(logical.error)));
                mQueued.models.insert(logical.path);
                SetStatus(logical.path, AssetImportStatus::Queued);
                return;
            }
            mQueued.importSource = source;
            mQueued.packageRoot = package.empty() ? source.parent_path() : package;
            mQueued.targetDirectory = action.mTargetDirectory.empty()
                                          ? UniqueImportTarget(source)
                                          : std::filesystem::path(action.mTargetDirectory);
        }
        catch (const std::exception &error)
        {
            mView.mLastError = error.what();
            MIR_LOG_ERROR("Asset import rejected: {}", error.what());
        }
    }

    void AssetImportManager::StartBatch()
    {
        if (mQueued.Empty() || mProcess.Running())
            return;
        mRunning = std::move(mQueued);
        mQueued.Clear();
        std::vector<std::string> args = {"--out", mCookedRoot.string(), "--result", mResultPath.string()};
        {
            // The active root stays first for external imports; every batch can resolve shared dependencies.
            std::vector<std::string> idRoots = {mProject.mAssetIdRoot, "assets"};
            for (GameProject &game : DiscoverGameProjects())
            {
                // a game with a broken assetsRoot has nothing to cook, and must not stop the others
                try
                {
                    ResolveAssetIdRoot(game);
                    idRoots.push_back(game.mAssetIdRoot);
                }
                catch (const SetupError &error)
                {
                    MIR_LOG_ERROR("Asset import: skipping '{}' in the cook: {} ({})", game.mDir, error.What(),
                        error.Cause());
                }
            }
            std::error_code ec;
            std::set<std::string> addedRoots;
            for (const std::string &idRoot : idRoots)
            {
                // the build skips missing roots too, AssetCooker fails on them
                if (addedRoots.insert(idRoot).second && std::filesystem::is_directory(mSourceRoot / idRoot, ec))
                    args.insert(args.end(), {"--source-dir", (mSourceRoot / idRoot).string(), "--id-root", idRoot});
            }
        }
        for (const auto &model : mRunning.models)
        {
            args.push_back("--model");
            args.push_back(model);
            SetStatus(model, AssetImportStatus::Processing);
        }
        for (const auto &material : mRunning.materials)
        {
            args.push_back("--material");
            args.push_back(material);
            SetStatus(material, AssetImportStatus::Processing);
        }
        for (const auto &changed : mRunning.changed)
        {
            args.push_back("--changed");
            args.push_back(changed.string());
        }
        for (const auto &model : mRunning.removals)
        {
            args.push_back(IsMaterial(model) ? "--remove-material" : "--remove-model");
            args.push_back(model);
        }
        for (const auto &[model, destination] : mRunning.moves)
            args.insert(args.end(), {"--move-model", model, "--move-to", destination.string()});
        if (mRunning.force)
            args.push_back("--force");
        if (!mRunning.importSource.empty())
        {
            args.insert(args.end(), {"--import", mRunning.importSource.string()});
            if (!mRunning.packageRoot.empty())
                args.insert(args.end(), {"--package-root", mRunning.packageRoot.string()});
            if (!mRunning.targetDirectory.empty())
                args.insert(args.end(), {"--destination", mRunning.targetDirectory.string()});
        }
        std::error_code error;
        std::filesystem::remove(mResultPath, error);
        if (!mProcess.Start(mCooker, args, ExecutableDir()))
        {
            for (const auto &model : mRunning.models)
                SetStatus(model, AssetImportStatus::Failed, "could not start AssetCooker");
            for (const auto &material : mRunning.materials)
                SetStatus(material, AssetImportStatus::Failed, "could not start AssetCooker");
            mRunning.Clear();
            return;
        }
        mView.mProcessRunning = true;
        mCancelRequested = false;
        mRunningStale = false;
        MIR_LOG_INFO("Started AssetCooker for {} queued model(s) and {} material(s)", mRunning.models.size(),
            mRunning.materials.size());
    }

    void AssetImportManager::FinishBatch(int exitCode, bool editing)
    {
        mView.mProcessRunning = false;
        if (mCancelRequested)
        {
            for (const auto &model : mRunning.models)
                SetStatus(model, AssetImportStatus::Canceled);
            for (const auto &material : mRunning.materials)
                SetStatus(material, AssetImportStatus::Canceled);
            for (const auto &model : mRunning.removals)
                SetStatus(model, AssetImportStatus::Canceled);
            for (const auto &[model, destination] : mRunning.moves)
                SetStatus(model, AssetImportStatus::Canceled);
        }
        else
        {
            try
            {
                const Json result = ReadJson(mResultPath);
                if (exitCode != 0 || result.value("status", "failed") == "failed")
                {
                    const std::string message =
                        result.value("diagnostic", Json::object()).value("message", "AssetCooker failed");
                    for (const auto &model : mRunning.models)
                        SetStatus(model, AssetImportStatus::Failed, message);
                    for (const auto &material : mRunning.materials)
                    {
                        SetStatus(material, AssetImportStatus::Failed, message);
                        const auto sequence = mRunning.materialSequences.find(material);
                        if (sequence != mRunning.materialSequences.end())
                            mMaterialFailureRevision[material] = sequence->second;
                    }
                    mView.mLastError = message;
                    MIR_LOG_ERROR("Asset import failed: {}", message);
                }
                else
                {
                    mView.mLastError.clear();
                    bool cooked = result.value("assetsChanged", false);
                    const Json resultModels = result.value("models", Json::array());
                    for (const auto &model : resultModels)
                    {
                        const std::string path = model.value("path", "");
                        const std::string status = model.value("status", "");
                        if (status == "cooked" || status == "removed")
                            cooked = true;
                        if (status == "cooked" && !mRunningStale)
                            mView.mLastImported = path;
                        SetStatus(path, mRunningStale           ? AssetImportStatus::Queued
                                        : status == "missing"   ? AssetImportStatus::Missing
                                        : status == "unchanged" ? AssetImportStatus::UpToDate
                                                                : (editing ? AssetImportStatus::WaitingForApply
                                                                           : AssetImportStatus::WaitingForStop));
                    }
                    // a full cook replaces a missing manifest, so it applies even when no model changed
                    mPendingApply |= cooked || mRunning.fullCook;
                }

                if (!mRunningStale)
                {
                    for (const std::string &material : mRunning.materials)
                    {
                        const auto sequence = mRunning.materialSequences.find(material);
                        const uint64_t revision = sequence != mRunning.materialSequences.end() ? sequence->second : 0;
                        const auto status = mStatus.find(material);
                        if (status == mStatus.end())
                            continue;
                        if (status->second == AssetImportStatus::WaitingForApply ||
                            status->second == AssetImportStatus::WaitingForStop)
                        {
                            mPendingApplyRevision[material] = revision;
                        }
                        else if (status->second == AssetImportStatus::UpToDate)
                        {
                            if (mMaterialFailureRevision.contains(material))
                            {
                                SetStatus(material, AssetImportStatus::WaitingForApply);
                                mPendingApplyRevision[material] = revision;
                                mPendingApply = true;
                            }
                            else
                                mMaterialCookRevision[material] = revision;
                        }
                        else if (status->second == AssetImportStatus::Failed ||
                                 status->second == AssetImportStatus::Missing ||
                                 status->second == AssetImportStatus::Canceled)
                            mMaterialFailureRevision[material] = revision;
                    }
                }
            }
            catch (const std::exception &error)
            {
                for (const auto &model : mRunning.models)
                    SetStatus(model, AssetImportStatus::Failed, error.what());
                for (const auto &material : mRunning.materials)
                {
                    SetStatus(material, AssetImportStatus::Failed, error.what());
                    const auto sequence = mRunning.materialSequences.find(material);
                    if (sequence != mRunning.materialSequences.end())
                        mMaterialFailureRevision[material] = sequence->second;
                }
            }
        }
        if (mRunningStale)
        {
            mQueued.models.insert(mRunning.models.begin(), mRunning.models.end());
            mQueued.materials.insert(mRunning.materials.begin(), mRunning.materials.end());
            mQueued.changed.insert(mRunning.changed.begin(), mRunning.changed.end());
            for (const auto &[material, revision] : mRunning.materialSequences)
                mQueued.materialSequences[material] = std::max(mQueued.materialSequences[material], revision);
            mQueued.fullCook |= mRunning.fullCook;
        }
        if (!mRunning.importSource.empty() || !mRunning.moves.empty())
        {
            mWatched.clear();
            mWatchInitialized = false;
            mLastWatchPoll = {};
        }
        mRunning.Clear();
        RefreshView();
    }

    void AssetImportManager::PollWatcher()
    {
        const auto now = std::chrono::steady_clock::now();
        if (mLastWatchPoll != std::chrono::steady_clock::time_point{} &&
            now - mLastWatchPoll < std::chrono::milliseconds(250))
            return;
        mLastWatchPoll = now;
        // External cooker runs can replace or remove the cache without changing
        // source files. Check metadata at the watcher cadence, not every frame.
        std::error_code cacheError;
        const std::filesystem::directory_entry cacheFile(mCookedRoot / "import-cache.json", cacheError);
        const std::optional<uint64_t> cacheSignature = !cacheError && cacheFile.is_regular_file(cacheError)
            ? std::optional<uint64_t>(Signature(cacheFile)) : std::nullopt;
        if (cacheError == std::errc::no_such_file_or_directory)
            cacheError.clear();
        if (cacheSignature != mImportCacheSignature || cacheError)
            mViewDirty = true;
        mImportCacheSignature = cacheSignature;
        std::set<std::filesystem::path> present;
        const bool cookerChangesSource =
            mView.mProcessRunning && (!mRunning.importSource.empty() || !mRunning.moves.empty());
        for (const auto &entry : std::filesystem::recursive_directory_iterator(mSourceAssetRoot))
        {
            if (!entry.is_regular_file() || !IsWatched(entry.path()))
                continue;
            const auto path = std::filesystem::weakly_canonical(entry.path());
            present.insert(path);
            const uint64_t signature = Signature(entry);
            auto [it, inserted] = mWatched.try_emplace(path, WatchedFile{signature, now, false});
            mViewDirty |= inserted;
            if (inserted && mWatchInitialized)
            {
                it->second.changedAt = now;
                it->second.pending = true;
                mRunningStale |= mView.mProcessRunning && !cookerChangesSource;
            }
            if (!inserted && it->second.signature != signature)
            {
                mViewDirty = true;
                it->second.signature = signature;
                it->second.changedAt = now;
                it->second.pending = true;
                mRunningStale |= mView.mProcessRunning && !cookerChangesSource;
            }
            if (mView.mAutoImport && it->second.pending && now - it->second.changedAt >= std::chrono::milliseconds(500))
            {
                if (IsModel(path) || IsMaterial(path))
                {
                    const auto relative = std::filesystem::relative(path, mSourceAssetRoot);
                    if (const AssetPathResult logical = LogicalModelPath(mProject.mAssetIdRoot, relative); logical.Ok())
                    {
                        if (IsMaterial(path))
                            mQueued.materials.insert(logical.path);
                        else
                            mQueued.models.insert(logical.path);
                        SetStatus(logical.path, AssetImportStatus::Queued);
                    }
                    else
                        MIR_LOG_ERROR("Asset import: '{}': {}", path.string(), Describe(logical.error));
                }
                else
                    mQueued.changed.insert(path);
                it->second.pending = false;
            }
        }
        for (auto it = mWatched.begin(); it != mWatched.end();)
        {
            if (!present.contains(it->first))
            {
                mViewDirty = true;
                mRunningStale |= mView.mProcessRunning && !cookerChangesSource;
                if (mView.mAutoImport)
                    mQueued.changed.insert(it->first);
                it = mWatched.erase(it);
            }
            else
                ++it;
        }
        mWatchInitialized = true;
    }

    void AssetImportManager::Update(bool editing)
    {
        PollWatcher();
        if (mView.mProcessRunning)
            if (auto code = mProcess.Poll())
                FinishBatch(*code, editing);
        if (!mView.mProcessRunning)
            StartBatch();
        if (mViewDirty)
            RefreshView();
    }

    void AssetImportManager::RefreshView()
    {
        std::map<std::string, AssetImportModelView> models;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(mSourceAssetRoot))
            if (entry.is_regular_file() && (IsModel(entry.path()) || IsMaterial(entry.path())))
            {
                // An unusable name is logged once, when the watcher sees it.
                const auto relative = std::filesystem::relative(entry.path(), mSourceAssetRoot);
                if (const AssetPathResult logical = LogicalModelPath(mProject.mAssetIdRoot, relative); logical.Ok())
                    models[logical.path].mPath = logical.path;
            }
        try
        {
            const Json cache = ReadJson(mCookedRoot / "import-cache.json");
            auto applyRecords = [&](const Json &records, const char *idKey) {
                for (const auto &[path, record] : records.items())
                {
                    auto &model = models[path];
                    model.mPath = path;
                    model.mAssetId = record.value(idKey, 0ull);
                    model.mLastSuccess = record.value("lastSuccess", 0ll);
                    model.mStatus =
                        record.value("missing", false) ? AssetImportStatus::Missing : AssetImportStatus::UpToDate;
                    for (const auto &dependency : record.value("dependencies", Json::array()))
                        model.mDependencies.push_back(dependency.get<std::string>());
                    for (const auto &entry : record.value("contents", Json::array()))
                        model.mContents.push_back({entry.value("kind", std::string{}),
                                                   entry.value("name", std::string{}), entry.value("id", 0ull),
                                                   entry.value("index", 0u)});
                    model.mDiagnostic = record.value("lastError", "");
                }
            };
            applyRecords(cache.value("models", Json::object()), "modelId");
            applyRecords(cache.value("materials", Json::object()), "materialId");
        }
        catch (const std::exception &error)
        {
            MIR_LOG_WARN("Could not read import cache: {}", error.what());
        }
        for (auto &[path, model] : models)
        {
            if (auto status = mStatus.find(path); status != mStatus.end())
                model.mStatus = status->second;
            if (auto diagnostic = mDiagnostics.find(path); diagnostic != mDiagnostics.end())
                model.mDiagnostic = diagnostic->second;
        }
        mView.mModels.clear();
        for (auto &[path, model] : models)
            mView.mModels.push_back(std::move(model));
        mViewDirty = false;
    }

    void AssetImportManager::SaveAutoImport()
    {
        try
        {
            Json settings = ReadJson(mSourceSettingsPath);
            settings["assets"]["autoImport"] = mView.mAutoImport;
            WriteJson(mSourceSettingsPath, settings);
        }
        catch (const std::exception &error)
        {
            MIR_LOG_ERROR("Could not save auto import setting: {}", error.what());
        }
    }

    void AssetImportManager::MarkApplied()
    {
        mPendingApply = false;
        for (auto &[path, status] : mStatus)
            if (status == AssetImportStatus::WaitingForApply || status == AssetImportStatus::WaitingForStop)
                status = AssetImportStatus::Applied;
        for (const auto &[path, revision] : mPendingApplyRevision)
        {
            mMaterialCookRevision[path] = revision;
            mMaterialFailureRevision.erase(path);
        }
        if (!mPendingApplyRevision.empty())
            ++mApplySequence;
        mPendingApplyRevision.clear();
        RefreshView();
    }
    void AssetImportManager::MarkApplyFailed(std::string diagnostic)
    {
        mPendingApply = false;
        for (auto &[path, status] : mStatus)
            if (status == AssetImportStatus::WaitingForApply || status == AssetImportStatus::WaitingForStop)
                SetStatus(path, AssetImportStatus::Failed, diagnostic);
        for (const auto &[path, revision] : mPendingApplyRevision)
            mMaterialFailureRevision[path] = revision;
        mPendingApplyRevision.clear();
        RefreshView();
    }

    void AssetImportManager::Reject(const std::string &model, std::string diagnostic)
    {
        SetStatus(model, AssetImportStatus::Failed, std::move(diagnostic));
        RefreshView();
    }

    uint64_t AssetImportManager::ReimportMaterial(std::string_view logicalPath)
    {
        const uint64_t revision = ++mMaterialRequestSequence;
        mQueued.materialSequences[std::string(logicalPath)] = revision;

        AssetImportAction action;
        action.mKind = AssetImportActionKind::Reimport;
        action.mModelPath = std::string(logicalPath);
        Handle(action);
        return revision;
    }

    std::optional<uint64_t> AssetImportManager::MaterialCookRevision(std::string_view logicalPath) const
    {
        const auto found = mMaterialCookRevision.find(std::string(logicalPath));
        return found != mMaterialCookRevision.end() ? std::optional<uint64_t>(found->second) : std::nullopt;
    }

    std::optional<uint64_t> AssetImportManager::MaterialCookFailureRevision(std::string_view logicalPath) const
    {
        const auto found = mMaterialFailureRevision.find(std::string(logicalPath));
        return found != mMaterialFailureRevision.end() ? std::optional<uint64_t>(found->second) : std::nullopt;
    }
} // namespace mir

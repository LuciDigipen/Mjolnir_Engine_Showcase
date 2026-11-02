/**
 * @file main.cpp
 * @author Rahul Nair
 * @brief Cooks a game's raw assets into the manifest + blob files the
 *        runtime AssetCache loads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "AssetReferences.h"
#include "GltfImporter.h"
#include "IdMigration.h"
#include "ImageImporter.h"
#include "MaterialCooker.h"
#include <algorithm>
#include <assets/AssetBlob.h>
#include <core/fs/FileIO.h>
#include <assets/AssetId.h>
#include <assets/AssetMeta.h>
#include <assets/AssetManifest.h>
#include <assets/AssetPath.h>
#include <assets/SourceAssetIds.h>
#include <assets/ModelAsset.h>
#include <cctype>
#include <chrono>
#include <core/log/Log.h>
#include <core/platform/Process.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <prefab/PrefabAsset.h>
#include <prefab/PrefabRegistry.h>
#include <particles/ParticleEffect.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <terrain/TerrainAssetPath.h>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace
{
    using Json = nlohmann::json;
    constexpr uint32_t kCacheVersion = 6;
    constexpr std::string_view kImporterFingerprint = "fastgltf-v2-stable-identity";
    constexpr std::string_view kMaterialCookerFingerprint = "surface-material-v1-packed-2048";
    // One generation back, kept so a crashed cook can be rolled back.
    constexpr std::string_view kPreviousManifestFileName = "manifest.previous.blob";
    struct SourceRoot
    {
        std::filesystem::path physical;
        std::string logical;
    };
    struct Options
    {
        std::vector<SourceRoot> roots;
        std::filesystem::path out, result;
        std::vector<std::string> models, materials;
        std::vector<std::string> removals, materialRemovals;
        std::vector<std::pair<std::string, std::filesystem::path>> moves;
        std::vector<std::filesystem::path> changed;
        std::filesystem::path importSource, packageRoot, targetDirectory;
        bool force = false;
        // --migrate-ids reports, or with --apply performs, the move to .meta guids and rewrites the JSON
        // under each --rewrite-dir
        bool migrateIds = false;
        bool applyMigration = false;
        std::vector<std::filesystem::path> rewriteDirs;
    };
    struct Output
    {
        mir::AssetId id{};
        uint32_t tag = 0, version = 0;
        std::string file;
        std::string logicalPath;
        std::vector<mir::AssetId> deps;
    };

    std::string Hex(uint64_t value)
    {
        std::ostringstream s;
        s << std::hex << std::setfill('0') << std::setw(16) << value;
        return s.str();
    }
    std::string LowerExtension(const std::filesystem::path &path)
    {
        std::string value = path.extension().string();
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }
    std::string LowerFilename(const std::filesystem::path &path)
    {
        std::string value = path.filename().string();
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }
    bool IsMaterialPath(const std::filesystem::path &path) { return LowerFilename(path).ends_with(".material.json"); }
    void Publish(const std::filesystem::path &path, std::span<const std::byte> bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path pending = path.string() + ".pending";
        if (!mir::WriteBytes(pending, bytes))
            throw std::runtime_error("cannot write " + pending.string());
#ifdef _WIN32
        if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot publish " + path.string());
#else
        std::filesystem::rename(pending, path);
#endif
    }
    void PublishText(const std::filesystem::path &path, const std::string &value)
    {
        Publish(path, std::as_bytes(std::span(value)));
    }
    std::vector<std::byte> CompileParticleScript(const std::string &source, const std::filesystem::path &out)
    {
        // The output lock serializes compiler scratch files with manifest publication.
        const auto path = std::filesystem::absolute(out / ("particle-" + Hex(mir::Fnv1a64(source)) + ".slang"));
        const auto spirv = std::filesystem::path(path.string() + ".spv");
        struct Scratch
        {
            std::filesystem::path source, spirv;
            ~Scratch()
            {
                std::error_code error;
                std::filesystem::remove(source, error);
                std::filesystem::remove(spirv, error);
            }
        } scratch{path, spirv};
        PublishText(path, source);
        // Remove an interrupted cook's output so a failed compile cannot reuse it.
        std::filesystem::remove(spirv);
        const std::vector<std::string> args{path.string(), "-target", "spirv", "-profile", "spirv_1_5",
            "-entry", "computeMain", "-stage", "compute", "-fvk-use-entrypoint-name",
            "-matrix-layout-column-major", "-O0", "-o", spirv.string()};
        mir::ChildProcess compiler;
        if (!compiler.Start(PARTICLE_SLANGC_EXECUTABLE, args))
            throw std::runtime_error("cannot start particle shader compiler: " + std::string(PARTICLE_SLANGC_EXECUTABLE));
        const auto exit = compiler.Wait();
        if (!exit || *exit != 0)
            throw std::runtime_error("particle shader compilation failed, slangc exit " + std::to_string(exit.value_or(-1)));
        const auto bytes = mir::ReadBytes(spirv);
        if (!bytes || bytes->size() < 20 || bytes->size() % 4 != 0 ||
            std::to_integer<uint8_t>((*bytes)[0]) != 0x03 || std::to_integer<uint8_t>((*bytes)[1]) != 0x02 ||
            std::to_integer<uint8_t>((*bytes)[2]) != 0x23 || std::to_integer<uint8_t>((*bytes)[3]) != 0x07)
            throw std::runtime_error("particle compiler emitted missing or invalid SPIR-V");
        return *bytes;
    }
    bool ProcessExists(uint64_t pid)
    {
#ifdef _WIN32
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (!process)
            return false;
        const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        return running;
#else
        return pid > 0 && kill(static_cast<pid_t>(pid), 0) == 0;
#endif
    }
    uint64_t CurrentPid()
    {
#ifdef _WIN32
        return GetCurrentProcessId();
#else
        return static_cast<uint64_t>(getpid());
#endif
    }
    class OutputLock
    {
    public:
        explicit OutputLock(const std::filesystem::path &out) : mPath(out / ".asset-cooker.lock")
        {
            std::error_code error;
            if (!std::filesystem::create_directory(mPath, error))
            {
                uint64_t owner = 0;
                std::ifstream(mPath / "owner") >> owner;
                if (owner && ProcessExists(owner))
                    throw std::runtime_error("another AssetCooker is writing this output directory");
                std::filesystem::remove_all(mPath, error);
                error.clear();
                if (!std::filesystem::create_directory(mPath, error))
                    throw std::runtime_error("cannot acquire AssetCooker output lock");
            }
            std::ofstream(mPath / "owner") << CurrentPid();
            mHeld = true;
        }
        ~OutputLock()
        {
            if (mHeld)
            {
                std::error_code e;
                std::filesystem::remove_all(mPath, e);
            }
        }
        OutputLock(const OutputLock &) = delete;
        OutputLock &operator=(const OutputLock &) = delete;

    private:
        std::filesystem::path mPath;
        bool mHeld = false;
    };

    std::string RequireAssetPath(const mir::AssetPathResult &result, std::string_view what)
    {
        if (!result.Ok())
            throw std::runtime_error(std::string(what) + ": " + std::string(mir::Describe(result.error)));
        return result.path;
    }

    Options ParseOptions(int argc, char **argv)
    {
        Options options;
        std::optional<std::filesystem::path> pendingRoot;
        std::optional<std::string> pendingMove;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            auto value = [&]() {
                if (i + 1 >= argc)
                    throw std::runtime_error("missing value after " + arg);
                return std::string(argv[++i]);
            };
            if (arg == "--source")
            {
                // The path doubles as the id root, so it must be relative like any id root
                const auto path = std::filesystem::path(value());
                options.roots.push_back({path, RequireAssetPath(mir::NormalizeAssetPath(path.generic_string()), arg)});
            }
            else if (arg == "--source-dir")
            {
                if (pendingRoot)
                    throw std::runtime_error("--source-dir requires a following --id-root");
                pendingRoot = value();
            }
            else if (arg == "--id-root")
            {
                if (!pendingRoot)
                    throw std::runtime_error("--id-root must follow --source-dir");
                options.roots.push_back({*pendingRoot, RequireAssetPath(mir::NormalizeAssetPath(value()), arg)});
                pendingRoot.reset();
            }
            else if (arg == "--out")
                options.out = value();
            else if (arg == "--result")
                options.result = value();
            else if (arg == "--model")
                options.models.push_back(RequireAssetPath(mir::NormalizeAssetPath(value()), arg));
            else if (arg == "--material")
                options.materials.push_back(RequireAssetPath(mir::NormalizeAssetPath(value()), arg));
            else if (arg == "--remove-model")
                options.removals.push_back(RequireAssetPath(mir::NormalizeAssetPath(value()), arg));
            else if (arg == "--remove-material")
                options.materialRemovals.push_back(RequireAssetPath(mir::NormalizeAssetPath(value()), arg));
            else if (arg == "--move-model")
            {
                if (pendingMove)
                    throw std::runtime_error("--move-model requires a following --move-to");
                pendingMove = RequireAssetPath(mir::NormalizeAssetPath(value()), arg);
            }
            else if (arg == "--move-to")
            {
                if (!pendingMove)
                    throw std::runtime_error("--move-to must follow --move-model");
                options.moves.emplace_back(*pendingMove, std::filesystem::path(value()).lexically_normal());
                pendingMove.reset();
            }
            else if (arg == "--changed")
                options.changed.emplace_back(value());
            else if (arg == "--import")
                options.importSource = value();
            else if (arg == "--package-root")
                options.packageRoot = value();
            else if (arg == "--destination")
                options.targetDirectory = value();
            else if (arg == "--force")
                options.force = true;
            else if (arg == "--migrate-ids")
                options.migrateIds = true;
            else if (arg == "--apply")
                options.applyMigration = true;
            else if (arg == "--rewrite-dir")
                options.rewriteDirs.emplace_back(value());
            else
                throw std::runtime_error("unknown argument: " + arg);
        }
        if (pendingRoot)
            throw std::runtime_error("--source-dir requires a following --id-root");
        if (pendingMove)
            throw std::runtime_error("--move-model requires a following --move-to");
        if (options.roots.empty() || (options.out.empty() && !options.migrateIds))
            throw std::runtime_error("source roots and output are required");
        return options;
    }
    std::string LogicalPath(const SourceRoot &root, const std::filesystem::path &path)
    {
        const auto relative = std::filesystem::relative(path, root.physical);
        return RequireAssetPath(mir::MakeLogicalAssetPath(root.logical, relative.generic_string()), path.string());
    }
    std::optional<std::pair<const SourceRoot *, std::filesystem::path>> ResolveModel(const Options &options,
                                                                                     const std::string &requested)
    {
        const mir::AssetPathResult logical = mir::NormalizeAssetPath(requested);
        if (!logical.Ok())
            return std::nullopt;
        const auto normalized = std::filesystem::path(logical.path);
        for (const auto &root : options.roots)
        {
            const auto relative = normalized.lexically_relative(std::filesystem::path(root.logical));
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
                return std::pair{&root, root.physical / relative};
            const auto direct = root.physical / normalized;
            if (std::filesystem::exists(direct))
                return std::pair{&root, direct};
        }
        return std::nullopt;
    }
    Json LoadJson(const std::filesystem::path &path, Json fallback)
    {
        if (!std::filesystem::exists(path))
            return fallback;
        std::ifstream input(path);
        Json value;
        input >> value;
        return value;
    }
    uint64_t Fingerprint(const std::vector<std::filesystem::path> &dependencies)
    {
        uint64_t hash = 14695981039346656037ull;
        auto append = [&](std::span<const std::byte> bytes) {
            for (const std::byte byte : bytes)
            {
                hash ^= std::to_integer<uint8_t>(byte);
                hash *= 1099511628211ull;
            }
        };
        append(std::as_bytes(std::span(kImporterFingerprint)));
        append(std::as_bytes(std::span(kMaterialCookerFingerprint)));
        const uint32_t payloadVersions[]{mir::kModelAssetContentVersion,      mir::kMeshAssetContentVersion,
                                         mir::kSkeletonAssetContentVersion,   mir::kAnimationAssetContentVersion,
                                         mir::kMaterialAssetContentVersion,   mir::kTextureAssetContentVersion,
                                         mir::kSurfaceMaterialAssetContentVersion};
        append(std::as_bytes(std::span(payloadVersions)));
        for (const auto &path : dependencies)
        {
            const std::string name = path.lexically_normal().generic_string();
            append(std::as_bytes(std::span(name)));
            const auto bytes = mir::ReadBytes(path);
            if (!bytes)
                throw std::runtime_error("missing dependency: " + path.string());
            constexpr std::string_view lfs = "version https://git-lfs.github.com/spec/v1";
            if (bytes->size() >= lfs.size() &&
                std::string_view(reinterpret_cast<const char *>(bytes->data()), lfs.size()) == lfs)
                throw std::runtime_error("dependency is a Git LFS pointer, not its content; run git lfs pull: " +
                                         path.string());
            append(*bytes);
        }
        return hash;
    }
    Output OutputFromJson(const Json &v)
    {
        Output output{{v.at("id").get<uint64_t>()},
                      v.at("tag").get<uint32_t>(),
                      v.at("version").get<uint32_t>(),
                      v.at("file").get<std::string>()};
        output.logicalPath = v.value("logicalPath", std::string());
        for (const auto &dep : v.value("deps", Json::array()))
            output.deps.push_back({dep.get<uint64_t>()});
        return output;
    }
    Json OutputToJson(const Output &v)
    {
        Json deps = Json::array();
        for (const mir::AssetId dep : v.deps)
            deps.push_back(dep.value);
        return {{"id", v.id.value}, {"tag", v.tag}, {"version", v.version}, {"file", v.file},
                {"logicalPath", v.logicalPath}, {"deps", deps}};
    }
    bool CachedOutputsValid(const Json &record, const std::filesystem::path &out)
    {
        if (!record.contains("outputs"))
            return false;
        for (const auto &encoded : record.at("outputs"))
        {
            const Output value = OutputFromJson(encoded);
            auto bytes = mir::ReadBytes(out / value.file);
            auto blob = bytes ? mir::ParseAssetBlob(*bytes) : std::nullopt;
            if (!blob || blob->header.typeTag != value.tag || blob->header.contentVersion != value.version)
                return false;
        }
        return true;
    }
    Json BuildResult(std::string_view status, const Json &models = Json::array())
    {
        return {{"version", 1}, {"status", status}, {"models", models}};
    }
    void WriteResult(const Options &options, const Json &result)
    {
        if (!options.result.empty())
            PublishText(options.result, result.dump(2) + "\n");
    }

    void StageExternalImport(Options &options)
    {
        if (options.importSource.empty())
            return;
        if (options.roots.size() != 1)
            throw std::runtime_error("external import requires exactly one source root");
        const auto source = std::filesystem::weakly_canonical(options.importSource);
        const auto package =
            std::filesystem::weakly_canonical(options.packageRoot.empty() ? source.parent_path() : options.packageRoot);
        const auto sourceRelative = source.lexically_relative(package);
        if (sourceRelative.empty() || sourceRelative.is_absolute() || *sourceRelative.begin() == "..")
            throw std::runtime_error("the glTF entry is outside its package root");
        auto target = options.targetDirectory;
        if (target.empty())
            target = std::filesystem::path("models") / source.stem();
        target = target.lexically_normal();
        if (target.empty() || target.is_absolute() || *target.begin() == "..")
            throw std::runtime_error("import target must be relative to the project's asset root");
        const auto destination = options.roots.front().physical / target;
        if (std::filesystem::exists(destination))
            throw std::runtime_error("import target already exists: " + destination.string());
        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);
        const std::string pendingPrefix = destination.filename().string() + ".importing-";
        for (const auto &entry : std::filesystem::directory_iterator(destination.parent_path()))
            if (entry.is_directory() && entry.path().filename().string().starts_with(pendingPrefix))
                std::filesystem::remove_all(entry.path(), error);
        const auto dependencies = mir::cook::InspectGltfDependencies(source, {package});
        const auto pending = destination.parent_path() /
                             (destination.filename().string() + ".importing-" + std::to_string(CurrentPid()));
        std::filesystem::remove_all(pending, error);
        try
        {
            for (const auto &dependency : dependencies)
            {
                const auto relative = dependency.lexically_relative(package);
                if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                    throw std::runtime_error("dependency is outside the selected package root: " + dependency.string());
                std::filesystem::create_directories((pending / relative).parent_path());
                std::filesystem::copy_file(dependency, pending / relative, std::filesystem::copy_options::none);
            }
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::rename(pending, destination);
        }
        catch (...)
        {
            std::filesystem::remove_all(pending, error);
            throw;
        }
        const auto imported = destination / sourceRelative;
        options.models.push_back(LogicalPath(options.roots.front(), imported));
    }

    std::string StageMove(Options &options, Json &cache, const std::string &logical,
                          const std::filesystem::path &targetRelative)
    {
        if (IsMaterialPath(std::filesystem::path(logical)))
            throw std::runtime_error("Surface materials cannot be moved as glTF model packages");
        auto resolved = ResolveModel(options, logical);
        if (!resolved || !std::filesystem::is_regular_file(resolved->second))
            throw std::runtime_error("model to move does not exist: " + logical);
        const SourceRoot &root = *resolved->first;
        const auto source = std::filesystem::weakly_canonical(resolved->second);
        auto target = targetRelative.lexically_normal();
        if (target.empty() || target.is_absolute() || *target.begin() == "..")
            throw std::runtime_error("move target must be relative to the project's asset root");
        const auto destination = root.physical / target;
        if (std::filesystem::exists(destination))
            throw std::runtime_error("move target already exists: " + destination.string());

        std::map<std::filesystem::path, uint32_t> uses;
        for (const auto &[path, record] : cache["models"].items())
            for (const auto &dependency : record.value("dependencies", Json::array()))
                ++uses[std::filesystem::weakly_canonical(dependency.get<std::string>())];
        std::vector<std::filesystem::path> dependencies{source};
        if (cache["models"].contains(logical))
            for (const auto &dependency : cache["models"][logical].value("dependencies", Json::array()))
                dependencies.push_back(std::filesystem::weakly_canonical(dependency.get<std::string>()));
        std::sort(dependencies.begin(), dependencies.end());
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());

        const auto pending =
            destination.parent_path() / (destination.filename().string() + ".moving-" + std::to_string(CurrentPid()));
        std::error_code error;
        std::filesystem::remove_all(pending, error);
        std::map<std::filesystem::path, std::filesystem::path> relocated;
        const auto newEntry = pending / source.filename();
        try
        {
            std::filesystem::create_directories(pending);
            for (const auto &dependency : dependencies)
            {
                const auto withinPackage = dependency.lexically_relative(source.parent_path());
                const bool privateDependency =
                    dependency == source || (!withinPackage.empty() && !withinPackage.is_absolute() &&
                                             *withinPackage.begin() != ".." && uses[dependency] <= 1);
                if (!privateDependency)
                    continue;
                const auto relative = dependency == source ? source.filename() : withinPackage;
                const auto staged = pending / relative;
                std::filesystem::create_directories(staged.parent_path());
                std::filesystem::copy_file(dependency, staged, std::filesystem::copy_options::none);
                relocated[dependency] = destination / relative;
            }
            // the .meta goes along, so the moved model keeps its guid
            const auto sidecar = mir::AssetMetaPath(source);
            if (std::filesystem::exists(sidecar))
                std::filesystem::copy_file(sidecar, mir::AssetMetaPath(newEntry));

            if (LowerExtension(source) == ".gltf")
            {
                Json gltf = LoadJson(newEntry, Json::object());
                auto rewrite = [&](Json &objects) {
                    for (auto &object : objects)
                        if (object.contains("uri") && object["uri"].is_string() &&
                            !object["uri"].get_ref<const std::string &>().starts_with("data:"))
                        {
                            const auto oldDependency = std::filesystem::weakly_canonical(
                                source.parent_path() / object["uri"].get<std::string>());
                            const auto finalDependency =
                                relocated.contains(oldDependency) ? relocated.at(oldDependency) : oldDependency;
                            object["uri"] = std::filesystem::relative(finalDependency, destination).generic_string();
                        }
                };
                if (gltf.contains("buffers"))
                    rewrite(gltf["buffers"]);
                if (gltf.contains("images"))
                    rewrite(gltf["images"]);
                PublishText(newEntry, gltf.dump(2) + "\n");
            }
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::rename(pending, destination);
            for (const auto &[oldPath, newPath] : relocated)
                std::filesystem::remove(oldPath, error);
            std::filesystem::remove(mir::AssetMetaPath(source), error);
        }
        catch (...)
        {
            std::filesystem::remove_all(pending, error);
            throw;
        }
        const auto movedEntry = destination / source.filename();
        return LogicalPath(root, movedEntry);
    }

    int Cook(Options options)
    {
        const auto outputPath = std::filesystem::weakly_canonical(options.out);
        for (const auto &root : options.roots)
        {
            if (!std::filesystem::is_directory(root.physical))
                throw std::runtime_error("source root does not exist: " + root.physical.string());
            const auto relative = outputPath.lexically_relative(std::filesystem::weakly_canonical(root.physical));
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
                throw std::runtime_error("output directory must be outside source roots");
        }
        std::filesystem::create_directories(options.out);
        OutputLock lock(options.out);
        StageExternalImport(options);
        const auto cachePath = options.out / "import-cache.json";
        Json cache = LoadJson(cachePath, Json::object());
        const bool resetCache = !cache.is_object() || cache.value("version", 0u) != kCacheVersion ||
                                !cache.contains("models") || !cache["models"].is_object() ||
                                !cache.contains("materials") || !cache["materials"].is_object();
        if (resetCache)
            cache = {{"version", kCacheVersion}, {"models", Json::object()}, {"materials", Json::object()},
                     {"requiresFullCook", true}};
        const bool requiresFullCook = cache.value("requiresFullCook", false);
        std::vector<std::pair<std::string, std::string>> completedMoves;
        for (const auto &[model, destination] : options.moves)
        {
            const std::string moved = StageMove(options, cache, model, destination);
            options.models.push_back(moved);
            completedMoves.emplace_back(model, moved);
        }
        std::vector<mir::AssetSourceRoot> sourceRoots;
        for (const auto &root : options.roots)
            sourceRoots.push_back({root.physical, root.logical});
        mir::SourceAssetIds ids(std::move(sourceRoots));
        ids.Scan();
        // Particle dependencies form a small source graph, a changed member recooks
        // the complete graph before publishing its replacement manifest.
        bool particleChange = std::any_of(options.changed.begin(), options.changed.end(), [](const auto &path)
            {
                const auto name = path.filename().string();
                return name.ends_with(".particle.json") || name.ends_with(".emitter.json");
            });
        if (!particleChange && !options.changed.empty())
            for (const auto &root : options.roots)
                for (const auto &entry : std::filesystem::recursive_directory_iterator(root.physical))
                {
                    if (!entry.is_regular_file() || !entry.path().filename().string().ends_with(".emitter.json"))
                        continue;
                    const auto bytes = mir::ReadBytes(entry.path());
                    if (!bytes) continue;
                    const auto emitter = mir::ReadParticleEmitterSource(
                        {reinterpret_cast<const char *>(bytes->data()), bytes->size()}, root.logical);
                    if (!emitter.mEmitter) continue;
                    const auto texture = std::filesystem::weakly_canonical(root.physical / emitter.mTexturePath);
                    particleChange = particleChange || std::any_of(options.changed.begin(), options.changed.end(),
                        [&](const auto &changed) { return std::filesystem::weakly_canonical(changed) == texture; });
                }
        const bool partial = !requiresFullCook &&
                             (!options.models.empty() || !options.materials.empty() || !options.changed.empty() ||
                              !options.removals.empty() || !options.materialRemovals.empty() || !options.moves.empty());
        std::unordered_map<uint64_t, Output> outputs;
        std::unordered_map<uint64_t, Output> previousOutputs;
        if (auto previous = mir::AssetManifest::LoadFile(options.out / mir::kAssetManifestFileName))
            for (const auto &entry : previous->Entries())
            {
                const auto deps = previous->Dependencies(entry);
                previousOutputs.emplace(entry.id.value,
                    Output{entry.id, entry.typeTag, entry.contentVersion, std::string(previous->PathOf(entry)),
                           std::string(previous->LogicalPathOf(entry)),
                           std::vector<mir::AssetId>(deps.begin(), deps.end())});
            }
        if (partial)
            outputs = previousOutputs;

        std::set<std::string> requested(options.models.begin(), options.models.end());
        std::set<std::string> requestedMaterials(options.materials.begin(), options.materials.end());
        std::set<std::string> changedCanonical;
        for (const auto &path : options.changed)
            changedCanonical.insert(std::filesystem::weakly_canonical(path).generic_string());
        if (!changedCanonical.empty())
        {
            for (auto &[model, record] : cache["models"].items())
                for (const auto &dependency : record.value("dependencies", Json::array()))
                    if (changedCanonical.contains(
                            std::filesystem::weakly_canonical(dependency.get<std::string>()).generic_string()))
                        requested.insert(model);
            for (auto &[material, record] : cache["materials"].items())
                for (const auto &dependency : record.value("dependencies", Json::array()))
                    if (changedCanonical.contains(
                            std::filesystem::weakly_canonical(dependency.get<std::string>()).generic_string()))
                        requestedMaterials.insert(material);
        }

        std::vector<std::pair<const SourceRoot *, std::filesystem::path>> models;
        std::vector<std::pair<const SourceRoot *, std::filesystem::path>> materials;
        if (partial)
        {
            for (const auto &model : requested)
            {
                auto resolved = ResolveModel(options, model);
                if (!resolved)
                    throw std::runtime_error("model is outside configured source roots: " + model);
                models.push_back(*resolved);
            }
            for (const auto &material : requestedMaterials)
            {
                auto resolved = ResolveModel(options, material);
                if (!resolved)
                    throw std::runtime_error("material is outside configured source roots: " + material);
                materials.push_back(*resolved);
            }
        }
        else
        {
            for (const auto &root : options.roots)
                for (const auto &entry : std::filesystem::recursive_directory_iterator(root.physical))
                    if (entry.is_regular_file() &&
                        (LowerExtension(entry.path()) == ".gltf" || LowerExtension(entry.path()) == ".glb"))
                        models.emplace_back(&root, entry.path());
                    else if (entry.is_regular_file() && IsMaterialPath(entry.path()))
                        materials.emplace_back(&root, entry.path());
            std::sort(models.begin(), models.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
            std::sort(materials.begin(), materials.end(),
                      [](const auto &a, const auto &b) { return a.second < b.second; });
        }

        // each glTF source and the importer state its .meta keeps once the cook has published
        std::vector<std::pair<std::filesystem::path, std::string>> importerStates;
        Json resultModels = Json::array();
        std::unordered_set<std::string> seenModels;
        std::unordered_map<uint64_t, std::string> prefabAssetOwners;
        bool assetsChanged = false;
        auto emit = [&](mir::AssetId id, const std::vector<std::byte> &bytes, std::string_view logicalPath = {}) {
            const auto blob = mir::ParseAssetBlob(bytes);
            if (!blob)
                throw std::runtime_error("importer emitted invalid blob");
            if (const auto owner = prefabAssetOwners.find(id.value);
                owner != prefabAssetOwners.end() && blob->header.typeTag != mir::kPrefabAssetTypeTag)
                throw std::runtime_error("an asset hashes to the AssetId of prefab '" + owner->second + "'");
            const std::string file = Hex(id.value) + "-" + Hex(mir::Fnv1a64(bytes)) + ".blob";
            const auto path = options.out / file;
            bool same = false;
            if (auto previous = mir::ReadBytes(path))
                same = *previous == bytes;
            if (!same)
                Publish(path, bytes);
            Output output{id, blob->header.typeTag, blob->header.contentVersion, file};
            output.logicalPath = std::string(logicalPath);
            // the same asset can be emitted again by a dependent, which has no name to give
            if (output.logicalPath.empty())
                if (const auto known = outputs.find(id.value); known != outputs.end())
                    output.logicalPath = known->second.logicalPath;
            try
            {
                output.deps = mir::cook::CollectAssetReferences(*blob);
            }
            catch (const std::exception &error)
            {
                throw std::runtime_error("cannot read the references of asset " + Hex(id.value) + ": " + error.what());
            }
            const auto baseline = previousOutputs.find(id.value);
            assetsChanged = assetsChanged || baseline == previousOutputs.end() || baseline->second.file != file;
            outputs[id.value] = output;
            return output;
        };

        // a file a particle source names, by the guid in its .meta
        const mir::AssetIdOfPath particleIdOf = [&ids](std::string_view logical) { return ids.IdAt(logical); };

        std::unordered_map<mir::PrefabId, std::string> prefabOwners;
        auto cookPrefab = [&](const std::string &logical, const std::filesystem::path &path) {
            const mir::PrefabParseResult parsed = mir::ReadPrefabFile(path);
            if (!parsed.mDocument)
            {
                std::string message = "invalid prefab " + logical;
                for (const mir::PrefabDiagnostic &diagnostic : parsed.mDiagnostics)
                    if (diagnostic.mSeverity == mir::PrefabDiagnosticSeverity::Error)
                        message += "\n  " + diagnostic.mMessage;
                throw std::runtime_error(message);
            }

            const mir::PrefabId uuid = parsed.mDocument->mId;
            if (const auto owner = prefabOwners.find(uuid); owner != prefabOwners.end())
                throw std::runtime_error("prefab UUID " + mir::ToString(uuid) + " is used by both '" +
                    owner->second + "' and '" + logical + "'");
            prefabOwners.emplace(uuid, logical);

            const mir::AssetId id = mir::PrefabAssetId(uuid);
            if (const auto existing = outputs.find(id.value); existing != outputs.end())
                throw std::runtime_error("prefab '" + logical + "' hashes to the AssetId of cooked blob '" +
                    existing->second.file + "'; regenerate the prefab UUID");
            prefabAssetOwners.emplace(id.value, logical);
            emit(id, mir::BuildPrefabAsset(*parsed.mDocument));
        };

        std::unordered_map<uint64_t, std::string> particleScripts;
        auto cookParticleEmitter = [&](const SourceRoot &root, const std::filesystem::path &path)
        {
            const auto bytes = mir::ReadBytes(path);
            if (!bytes) throw std::runtime_error("missing particle emitter: " + path.string());
            auto emitter = mir::ReadParticleEmitterSource(
                {reinterpret_cast<const char *>(bytes->data()), bytes->size()}, root.logical, particleIdOf);
            if (!emitter.mEmitter)
                throw std::runtime_error(LogicalPath(root, path) + ": " + emitter.mError);
            const auto texturePath = root.physical / emitter.mTexturePath;
            const auto textureBytes = mir::ReadBytes(texturePath);
            if (!textureBytes || !mir::cook::IsImagePath(texturePath))
                throw std::runtime_error("missing or unsupported particle texture: " + texturePath.string());
            if (emitter.mEmitter->simulationTarget == mir::ParticleSimulationTarget::GPU)
            {
                const auto source = mir::GenerateParticleScript(*emitter.mEmitter);
                const auto layout = mir::ParticleScriptLayout();
                const auto scriptId = mir::MakeAssetId("particles/generated/" + Hex(mir::Fnv1a64(source)) + ".spv");
                const auto layoutId = mir::MakeAssetId("particles/generated/" + Hex(mir::Fnv1a64(layout)) + ".layout");
                const auto found = particleScripts.find(scriptId.value);
                if (found != particleScripts.end() && found->second != source)
                    throw std::runtime_error("particle script identity collision");
                if (found == particleScripts.end())
                {
                    const auto code = CompileParticleScript(source, options.out);
                    emit(scriptId, mir::BuildAssetBlob(mir::kParticleScriptTypeTag, mir::kParticleScriptContentVersion, code));
                    emit(layoutId, mir::BuildAssetBlob(mir::kParticleScriptLayoutTypeTag,
                        mir::kParticleScriptLayoutContentVersion, std::as_bytes(std::span(layout))));
                    particleScripts.emplace(scriptId.value, source);
                }
                emitter.mEmitter->script = scriptId;
                emitter.mEmitter->scriptLayout = layoutId;
            }
            emit(emitter.mEmitter->texture, mir::BuildTypedAsset(mir::cook::DecodeImage(*textureBytes)));
            emit(ids.Meta(path).guid, mir::BuildParticleEmitterAsset(*emitter.mEmitter), LogicalPath(root, path));
        };

        for (const auto &logical : options.removals)
        {
            if (auto resolved = ResolveModel(options, logical); resolved && std::filesystem::exists(resolved->second))
                throw std::runtime_error("refusing to remove an asset whose source still exists: " + logical);
            if (!cache["models"].contains(logical))
                continue;
            for (const auto &old : cache["models"][logical].value("outputs", Json::array()))
                outputs.erase(old.at("id").get<uint64_t>());
            cache["models"].erase(logical);
            resultModels.push_back({{"path", logical}, {"status", "removed"}});
        }
        for (const auto &logical : options.materialRemovals)
        {
            if (auto resolved = ResolveModel(options, logical); resolved && std::filesystem::exists(resolved->second))
                throw std::runtime_error("refusing to remove an asset whose source still exists: " + logical);
            if (!cache["materials"].contains(logical))
                continue;
            for (const auto &old : cache["materials"][logical].value("outputs", Json::array()))
                outputs.erase(old.at("id").get<uint64_t>());
            cache["materials"].erase(logical);
            resultModels.push_back({{"path", logical}, {"status", "removed"}});
        }

        std::unordered_set<std::string> seenMaterials;
        std::unordered_set<std::string> materialSourceImages;
        std::vector<std::filesystem::path> physicalRoots;
        for (const auto &candidate : options.roots)
            physicalRoots.push_back(candidate.physical);
        // A source image that a material packs is not a standalone asset. Drop
        // an older standalone entry from the manifest on a partial cook.
        auto eraseStandaloneSource = [&](const std::filesystem::path &image) {
            outputs.erase(ids.KnownId(image).value);
        };
        for (const auto &[root, path] : materials)
        {
            const std::string logical = LogicalPath(*root, path);
            if (!std::filesystem::exists(path))
            {
                if (cache["materials"].contains(logical))
                {
                    cache["materials"][logical]["missing"] = true;
                    resultModels.push_back({{"path", logical}, {"status", "missing"}});
                    continue;
                }
                throw std::runtime_error("material does not exist: " + path.string());
            }
            seenMaterials.insert(logical);
            auto &record = cache["materials"][logical];
            if (!record.is_object())
                record = Json::object();
            if (record.contains("outputs"))
                for (const auto &old : record["outputs"])
                    outputs.erase(old.at("id").get<uint64_t>());
            std::vector<std::filesystem::path> dependencies;
            uint64_t fingerprint = 0;
            try
            {
                mir::cook::MaterialDependencies material =
                    mir::cook::InspectMaterialDependencies(path, logical, root->physical, physicalRoots);
                for (const auto &image : material.images)
                {
                    materialSourceImages.insert(std::filesystem::weakly_canonical(image).generic_string());
                    eraseStandaloneSource(image);
                }
                dependencies = std::move(material.files);
                fingerprint = Fingerprint(dependencies);
            }
            catch (const std::exception &error)
            {
                record["lastError"] = error.what();
                record["missing"] = false;
                PublishText(options.out / "import-cache.json", cache.dump(2) + "\n");
                throw std::runtime_error(logical + ": " + error.what());
            }
            if (!options.force && record.value("fingerprint", 0ull) == fingerprint &&
                record.value("materialId", 0ull) == ids.Meta(path).guid.value && CachedOutputsValid(record, options.out))
            {
                for (const auto &cached : record["outputs"])
                {
                    Output output = OutputFromJson(cached);
                    outputs[output.id.value] = output;
                }
                record["missing"] = false;
                resultModels.push_back({{"path", logical},
                                        {"status", "unchanged"},
                                        {"materialId", record.value("materialId", 0ull)}});
                continue;
            }

            mir::cook::MaterialImport imported;
            try
            {
                // The paths were checked by the inspection pass.
                imported = mir::cook::ImportMaterial(path, logical, root->physical, physicalRoots, ids.Meta(path).guid);
            }
            catch (const std::exception &error)
            {
                record["lastError"] = error.what();
                record["missing"] = false;
                PublishText(options.out / "import-cache.json", cache.dump(2) + "\n");
                throw std::runtime_error(logical + ": " + error.what());
            }
            Json cachedOutputs = Json::array();
            for (const auto &blob : imported.blobs)
                cachedOutputs.push_back(OutputToJson(emit(blob.id, blob.bytes,
                    blob.id == imported.materialId ? std::string_view(logical) : std::string_view())));
            Json dependencyNames = Json::array();
            for (const auto &dependency : imported.dependencies)
                dependencyNames.push_back(dependency.generic_string());
            Json contents = Json::array();
            for (const auto &entry : imported.contents)
                contents.push_back({{"kind", entry.kind},
                                    {"name", entry.name},
                                    {"id", entry.id.value},
                                    {"index", entry.index}});
            Json sizes = Json::array();
            for (const auto &size : imported.textureSizes)
                sizes.push_back({{"role", size.role}, {"width", size.width}, {"height", size.height}});
            record = {{"materialId", imported.materialId.value},
                      {"fingerprint", fingerprint},
                      {"dependencies", dependencyNames},
                      {"outputs", cachedOutputs},
                      {"contents", contents},
                      {"textureSizes", sizes},
                      {"missing", false},
                      {"lastSuccess", std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count()}};
            resultModels.push_back({{"path", logical},
                                    {"status", "cooked"},
                                    {"materialId", imported.materialId.value},
                                    {"dependencies", dependencyNames},
                                    {"sizes", sizes}});
        }

        for (const auto &[root, path] : models)
        {
            const std::string logical = LogicalPath(*root, path);
            if (!std::filesystem::exists(path))
            {
                if (cache["models"].contains(logical))
                {
                    cache["models"][logical]["missing"] = true;
                    resultModels.push_back({{"path", logical}, {"status", "missing"}});
                    continue;
                }
                throw std::runtime_error("model does not exist: " + path.string());
            }
            seenModels.insert(logical);
            auto &record = cache["models"][logical];
            if (!record.is_object())
                record = Json::object();
            if (record.contains("outputs"))
                for (const auto &old : record["outputs"])
                    outputs.erase(old.at("id").get<uint64_t>());
            std::vector<std::filesystem::path> physicalRoots;
            for (const auto &candidate : options.roots)
                physicalRoots.push_back(candidate.physical);
            std::vector<std::filesystem::path> dependencies;
            uint64_t fingerprint = 0;
            try
            {
                dependencies = mir::cook::InspectGltfDependencies(path, physicalRoots);
                fingerprint = Fingerprint(dependencies);
            }
            catch (const std::exception &error)
            {
                Json names = Json::array();
                for (const auto &dependency : dependencies)
                    names.push_back(dependency.generic_string());
                record["dependencies"] = names;
                record["lastError"] = error.what();
                record["missing"] = false;
                PublishText(options.out / "import-cache.json", cache.dump(2) + "\n");
                throw std::runtime_error(logical + ": " + error.what());
            }
            if (!options.force && record.value("fingerprint", 0ull) == fingerprint &&
                record.value("modelId", 0ull) == ids.Meta(path).guid.value && CachedOutputsValid(record, options.out))
            {
                for (const auto &cached : record["outputs"])
                {
                    Output output = OutputFromJson(cached);
                    outputs[output.id.value] = output;
                }
                record["missing"] = false;
                resultModels.push_back(
                    {{"path", logical}, {"status", "unchanged"}, {"modelId", record.value("modelId", 0ull)}});
                continue;
            }

            mir::cook::GltfImport imported;
            try
            {
                imported = mir::cook::ImportGltf(path, logical, physicalRoots, ids.Meta(path));
            }
            catch (const std::exception &error)
            {
                record["lastError"] = error.what();
                record["missing"] = false;
                PublishText(options.out / "import-cache.json", cache.dump(2) + "\n");
                throw std::runtime_error(logical + ": " + error.what());
            }
            Json cachedOutputs = Json::array();
            std::optional<Output> modelOutput;
            for (const auto &blob : imported.blobs)
            {
                Output output = emit(blob.id, blob.bytes,
                    blob.id == imported.modelId ? std::string_view(logical) : std::string_view());
                cachedOutputs.push_back(OutputToJson(output));
                if (blob.id == imported.modelId)
                    modelOutput = output;
            }
            if (!modelOutput)
                throw std::runtime_error("importer did not emit its model payload");
            Json dependencyNames = Json::array();
            for (const auto &dependency : imported.dependencies)
                dependencyNames.push_back(dependency.generic_string());
            // What the package contains, so the editor can list sub-assets
            // and their ids without loading any cooked blob.
            Json contents = Json::array();
            for (const auto &entry : imported.contents)
                contents.push_back({{"kind", entry.kind},
                                    {"name", entry.name},
                                    {"id", entry.id.value},
                                    {"index", entry.index}});
            record = {{"modelId", imported.modelId.value},
                      {"fingerprint", fingerprint},
                      {"dependencies", dependencyNames},
                      {"contents", contents},
                      {"outputs", cachedOutputs},
                      {"missing", false},
                      {"lastSuccess", std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count()}};
            importerStates.emplace_back(path, std::move(imported.importer));
            resultModels.push_back({{"path", logical},
                                    {"status", "cooked"},
                                    {"modelId", record["modelId"]},
                                    {"dependencies", dependencyNames}});
        }
        for (const auto &[oldPath, newPath] : completedMoves)
            if (oldPath != newPath)
                cache["models"].erase(oldPath);

        if (!partial)
        {
            // A record whose source is gone keeps its outputs, unless this cook emitted them all again.
            // Then the source moved, and its new record replaces the old one.
            auto keepMissing = [&](Json &records, const std::unordered_set<std::string> &seen)
            {
                std::vector<std::string> moved;
                for (auto &[logical, record] : records.items())
                {
                    if (seen.contains(logical))
                        continue;
                    const Json cachedOutputs = record.value("outputs", Json::array());
                    const bool emittedAgain = !cachedOutputs.empty() &&
                        std::all_of(cachedOutputs.begin(), cachedOutputs.end(), [&](const Json &cached)
                            { return outputs.contains(cached.at("id").get<uint64_t>()); });
                    if (emittedAgain)
                    {
                        moved.push_back(logical);
                        continue;
                    }
                    record["missing"] = true;
                    for (const auto &cached : cachedOutputs)
                    {
                        Output output = OutputFromJson(cached);
                        if (!outputs.contains(output.id.value) && std::filesystem::exists(options.out / output.file))
                            outputs[output.id.value] = output;
                    }
                }
                for (const std::string &logical : moved)
                    records.erase(logical);
            };
            keepMissing(cache["models"], seenModels);
            keepMissing(cache["materials"], seenMaterials);
        }
        // Standalone assets are rooted in the configured source roots. Particle changes
        // recook this graph while retaining manifest entries from roots outside the request.
        if (!partial || particleChange)
        {
            for (const auto &[logical, record] : cache["materials"].items())
                for (const auto &dependency : record.value("dependencies", Json::array()))
                {
                    const auto path = std::filesystem::path(dependency.get<std::string>());
                    if (mir::cook::IsImagePath(path))
                        materialSourceImages.insert(std::filesystem::weakly_canonical(path).generic_string());
                }
            // The recook below re-emits every live particle source, so entries for deleted
            // or renamed sources must not survive from the previous manifest.
            std::erase_if(outputs, [](const auto &entry)
                {
                    return entry.second.tag == mir::kParticleEffectTypeTag ||
                           entry.second.tag == mir::kParticleEmitterTypeTag;
                });
            //a gltf's images are cooked by its import, under its own ids
            std::unordered_set<std::string> modelImages;
            for (const auto &[logical, record] : cache["models"].items())
                for (const auto &dependency : record.value("dependencies", Json::array()))
                    modelImages.insert(
                        std::filesystem::weakly_canonical(dependency.get<std::string>()).generic_string());
            for (const auto &root : options.roots)
            {
                std::vector<std::filesystem::path> files;
                for (const auto &entry : std::filesystem::recursive_directory_iterator(root.physical))
                    if (entry.is_regular_file())
                        files.push_back(entry.path());
                std::sort(files.begin(), files.end());
                for (const auto &path : files)
                {
                    const std::string ext = LowerExtension(path), filename = path.filename().string();
                    // a bank's Wwise metadata is read loose, beside the bank
                    const bool bankMetadata =
                        ext == ".json" && std::filesystem::exists(std::filesystem::path(path).replace_extension(".bnk"));
                    if (ext == ".gltf" || ext == ".glb" || IsMaterialPath(path) || ext == ".slang" ||
                        ext == ".bnk" || ext == ".wem" ||
                        bankMetadata || mir::IsAssetToolingFile(path))
                        continue;
                    // TerrainTool owns recipes and releases.
                    const std::string logical = LogicalPath(root, path);
                    if (mir::IsTerrainFileExcludedFromCook(logical))
                        continue;
                    if (ext == ".prefab")
                    {
                        cookPrefab(logical, path);
                        continue;
                    }
                    if (filename.ends_with(".emitter.json"))
                    {
                        cookParticleEmitter(root, path);
                        continue;
                    }
                    if (filename.ends_with(".particle.json"))
                    {
                        const auto effectBytes = mir::ReadBytes(path);
                        if (!effectBytes)
                            throw std::runtime_error("cannot read " + path.string());
                        const auto effect = mir::ReadParticleEffectSource(
                            {reinterpret_cast<const char *>(effectBytes->data()), effectBytes->size()}, root.logical,
                            particleIdOf);
                        if (!effect.mEffect)
                            throw std::runtime_error(logical + ": " + effect.mError);
                        for (const auto &emitterPath : effect.mEmitterPaths)
                            cookParticleEmitter(root, root.physical / emitterPath);
                        emit(ids.Meta(path).guid, mir::BuildParticleEffectAsset(*effect.mEffect), logical);
                        continue;
                    }
                    auto bytes = mir::ReadBytes(path);
                    if (!bytes)
                        throw std::runtime_error("cannot read " + path.string());
                    //any other image is a texture of its own, decoded here so
                    //runtime never parses an image format
                    const std::string canonicalPath = std::filesystem::weakly_canonical(path).generic_string();
                    if (mir::cook::IsImagePath(path) && materialSourceImages.contains(canonicalPath))
                        continue;
                    if (mir::cook::IsImagePath(path) && !modelImages.contains(canonicalPath))
                    {
                        std::vector<std::byte> texture;
                        try
                        {
                            texture = mir::BuildTypedAsset(mir::cook::DecodeImage(*bytes));
                        }
                        catch (const std::exception &error)
                        {
                            throw std::runtime_error(logical + ": " + error.what());
                        }
                        emit(ids.Meta(path).guid, texture, logical);
                        continue;
                    }
                    emit(ids.Meta(path).guid,
                         mir::BuildAssetBlob(mir::kRawAssetTypeTag, mir::kRawAssetContentVersion, *bytes), logical);
                }
            }
        }
        for (auto &[source, state] : importerStates)
        {
            ids.Meta(source).importer = std::move(state);
            ids.Save(source);
        }
        // Generated scripts may be shared across source roots, so one stays while any
        // asset in the replacement manifest still points at it.
        std::unordered_set<uint64_t> referenced;
        for (const auto &[id, output] : outputs)
            for (const mir::AssetId dep : output.deps)
                referenced.insert(dep.value);
        std::erase_if(outputs, [&](const auto &entry)
            {
                const auto &output = entry.second;
                return (output.tag == mir::kParticleScriptTypeTag || output.tag == mir::kParticleScriptLayoutTypeTag) &&
                    !referenced.contains(output.id.value);
            });
        for (const auto &[id, output] : outputs)
            for (const mir::AssetId dep : output.deps)
                if (!outputs.contains(dep.value))
                    throw std::runtime_error("asset " + (output.logicalPath.empty() ? Hex(id) : output.logicalPath) +
                                             " depends on " + Hex(dep.value) + ", which is not cooked");
        std::vector<Output> ordered;
        for (const auto &[id, output] : outputs)
            ordered.push_back(output);
        std::sort(ordered.begin(), ordered.end(),
                  [](const Output &a, const Output &b) { return a.id.value < b.id.value; });
        std::vector<mir::AssetManifestSourceEntry> entries;
        for (const auto &output : ordered)
            entries.push_back({{output.id, output.tag, output.version}, output.file, output.logicalPath, output.deps});
        const auto manifestPath = options.out / mir::kAssetManifestFileName;
        const auto manifestBytes = mir::BuildAssetManifestBlob(entries);
        const auto previousManifestBytes = mir::ReadBytes(manifestPath);
        const bool manifestChanged = !previousManifestBytes || *previousManifestBytes != manifestBytes;
        if (auto bytes = mir::ReadBytes(manifestPath))
            Publish(options.out / kPreviousManifestFileName, *bytes);
        Publish(manifestPath, manifestBytes);
        cache.erase("requiresFullCook");
        PublishText(cachePath, cache.dump(2) + "\n");

        // Background loads may still read older cooked files. Delete them only during a clean with all
        // engine and editor readers stopped.
        const bool changed = std::any_of(resultModels.begin(), resultModels.end(), [](const Json &v) {
            const std::string status = v.value("status", "");
            return status == "cooked" || status == "removed";
        });
        assetsChanged = assetsChanged || manifestChanged;
        auto result = BuildResult(changed || assetsChanged ? "succeeded" : "unchanged", resultModels);
        result["assetsChanged"] = assetsChanged;
        WriteResult(options, result);
        MIR_LOG_INFO("AssetCooker: {} assets in manifest ({} model, {} material requests)", ordered.size(),
                     models.size(), materials.size());
        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    mir::InitLog();
    Options options;
    try
    {
        options = ParseOptions(argc, argv);
        if (options.migrateIds)
        {
            mir::cook::IdMigrationOptions migration;
            for (const auto &root : options.roots)
                migration.roots.push_back({root.physical, root.logical});
            migration.dataDirs = options.rewriteDirs;
            migration.apply = options.applyMigration;
            mir::cook::MigrateIds(migration);
            mir::FlushLog();
            return 0;
        }
        const int result = Cook(options);
        mir::FlushLog();
        return result;
    }
    catch (const std::exception &error)
    {
        MIR_LOG_ERROR("AssetCooker: {}", error.what());
        if (!options.result.empty())
            try
            {
                Json result = BuildResult("failed");
                result["diagnostic"] = {{"stage", "cook"}, {"message", error.what()}};
                WriteResult(options, result);
            }
            catch (...)
            {
            }
        mir::FlushLog();
        return 1;
    }
}

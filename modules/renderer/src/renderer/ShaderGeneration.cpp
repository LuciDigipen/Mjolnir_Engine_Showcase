/**
 * @file ShaderGeneration.cpp
 * @author Jackey Cen
 * @brief Immutable shader generations and cross-process publication.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/ShaderGeneration.h>

#include <core/fs/FileIO.h>
#include <core/platform/Process.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mir
{
    namespace
    {
        void SetError(std::string *error, std::string message)
        {
            if (error != nullptr)
                *error = std::move(message);
        }

        std::string Utf8(const std::filesystem::path &path)
        {
            const auto bytes = path.generic_u8string();
            return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
        }

        bool Exists(const std::filesystem::path &path)
        {
            std::error_code error;
            return std::filesystem::is_regular_file(path, error);
        }

        std::string GenerationName(uint64_t id)
        {
            std::string value = std::to_string(id);
            return std::string(16 - std::min<std::size_t>(16, value.size()), '0') + value;
        }

        bool HashFile(const std::filesystem::path &path, uint64_t *size, uint64_t *hash)
        {
            const std::optional<std::vector<std::byte>> bytes = ReadBytes(path);
            if (!bytes)
                return false;
            *size = bytes->size();
            *hash = HashShaderContent(*bytes);
            return true;
        }

        nlohmann::json ToJson(const std::vector<ShaderGenerationFile> &files)
        {
            nlohmann::json array = nlohmann::json::array();
            for (const ShaderGenerationFile &file : files)
                array.push_back({ { "path", file.mPath }, { "size", file.mSize }, { "hash", file.mContentHash } });
            return array;
        }

        std::vector<ShaderGenerationFile> FromJson(const nlohmann::json &array)
        {
            std::vector<ShaderGenerationFile> files;
            if (!array.is_array())
                throw std::runtime_error("shader snapshot must be an array");
            for (const nlohmann::json &entry : array)
            {
                ShaderGenerationFile file;
                if (!entry.is_object() || !entry.contains("path") || !entry.at("path").is_string() ||
                    !entry.contains("size") || !entry.at("size").is_number_unsigned() ||
                    !entry.contains("hash") || !entry.at("hash").is_number_unsigned())
                    throw std::runtime_error("shader snapshot entry is incomplete");
                file.mPath = entry.at("path").get<std::string>();
                file.mSize = entry.at("size").get<uint64_t>();
                file.mContentHash = entry.at("hash").get<uint64_t>();
                if (file.mPath.empty() || file.mPath.find('\0') != std::string::npos)
                    throw std::runtime_error("shader snapshot path is invalid");
                files.push_back(std::move(file));
            }
            return files;
        }

        std::optional<nlohmann::json> ReadMetadata(const std::filesystem::path &generation, std::string *error)
        {
            const auto path = generation / "metadata.json";
            const std::optional<std::string> text = Exists(path) ? ReadText(path) : std::nullopt;
            if (!text)
            {
                SetError(error, "shader generation metadata is missing in " + generation.string());
                return std::nullopt;
            }
            try
            {
                return nlohmann::json::parse(*text);
            }
            catch (const std::exception &exception)
            {
                SetError(error, std::string("shader generation metadata is malformed: ") + exception.what());
                return std::nullopt;
            }
        }
    }

    uint64_t HashShaderContent(std::span<const std::byte> bytes)
    {
        uint64_t hash = 1469598103934665603ull;
        for (const std::byte value : bytes)
        {
            hash ^= static_cast<uint8_t>(value);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::optional<ShaderGenerationFile> SnapshotShaderFile(const std::filesystem::path &path,
        std::string_view logicalPath)
    {
        ShaderGenerationFile file;
        file.mPath = std::string(logicalPath);
        if (!HashFile(path, &file.mSize, &file.mContentHash))
            return std::nullopt;
        return file;
    }

    bool VerifyShaderSnapshot(const std::vector<ShaderGenerationFile> &files, std::string *error)
    {
        for (const ShaderGenerationFile &file : files)
        {
            const auto path = std::filesystem::u8path(file.mPath);
            const auto bytes = Exists(path) ? ReadBytes(path) : std::nullopt;
            if (!bytes || bytes->size() != file.mSize ||
                HashShaderContent(*bytes) != file.mContentHash)
            {
                SetError(error, "shader input changed or disappeared: " + file.mPath);
                return false;
            }
        }
        return true;
    }

    ShaderGenerationStore::ShaderGenerationStore(std::filesystem::path root) : mRoot(std::move(root))
    {
        std::error_code ignored;
        std::filesystem::create_directories(mRoot / "generations", ignored);
    }

    ShaderGenerationStore::~ShaderGenerationStore()
    {
        if (mPublishing)
            Abort();
        Unlock();
    }

    std::filesystem::path ShaderGenerationStore::GenerationPath(uint64_t id) const
    {
        return mRoot / "generations" / GenerationName(id);
    }

    std::optional<uint64_t> ShaderGenerationStore::CurrentId() const
    {
        const auto pointer = mRoot / "current.json";
        const std::optional<std::string> text = Exists(pointer) ? ReadText(pointer) : std::nullopt;
        if (!text)
            return std::nullopt;
        try
        {
            const nlohmann::json root = nlohmann::json::parse(*text);
            if (root.value("version", 0u) != 1u || !root.contains("generation") ||
                !root.at("generation").is_number_unsigned() || root.at("generation").get<uint64_t>() == 0)
                return std::nullopt;
            return root.at("generation").get<uint64_t>();
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }

    std::filesystem::path ShaderGenerationStore::CurrentPath() const
    {
        if (const std::optional<uint64_t> id = CurrentId())
            return GenerationPath(*id);
        return mRoot;
    }

    bool ShaderGenerationStore::Lock(std::string *error, bool wait)
    {
        if (mLocked)
            return true;
        std::error_code ignored;
        std::filesystem::create_directories(mRoot, ignored);
        const std::filesystem::path lockPath = mRoot / ".publication.lock";
#if defined(_WIN32)
        HANDLE handle = CreateFileW(lockPath.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            SetError(error, "could not open the shader publication lock");
            return false;
        }
        OVERLAPPED overlapped{};
        const DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
        if (!LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &overlapped))
        {
            CloseHandle(handle);
            SetError(error, "could not acquire the shader publication lock");
            return false;
        }
        mLockHandle = handle;
#else
        const int handle = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        struct flock lock{};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        if (handle < 0 || fcntl(handle, wait ? F_SETLKW : F_SETLK, &lock) != 0)
        {
            if (handle >= 0)
                close(handle);
            SetError(error, "could not acquire the shader publication lock");
            return false;
        }
        mLockHandle = handle;
#endif
        mLocked = true;
        return true;
    }

    void ShaderGenerationStore::Unlock()
    {
        if (!mLocked)
            return;
#if defined(_WIN32)
        OVERLAPPED overlapped{};
        UnlockFileEx(static_cast<HANDLE>(mLockHandle), 0, MAXDWORD, MAXDWORD, &overlapped);
        CloseHandle(static_cast<HANDLE>(mLockHandle));
        mLockHandle = nullptr;
#else
        close(mLockHandle);
        mLockHandle = -1;
#endif
        mLocked = false;
    }

    bool ShaderGenerationStore::BeginPublish(uint64_t *outId, std::string *error, bool wait)
    {
        if (mPublishing)
            return false;
        if (!Lock(error, wait))
            return false;

        mExpectedCurrent = CurrentId().value_or(0);
        const auto base = ReadCurrent(mRoot);
        mParent = base ? base->mId : 0;
        mId = std::max(mParent, mExpectedCurrent);
        std::error_code scanError;
        for (const auto &entry : std::filesystem::directory_iterator(mRoot / "generations", scanError))
        {
            const std::string name = Utf8(entry.path().filename());
            uint64_t id = 0;
            const auto parsed = std::from_chars(name.data(), name.data() + name.size(), id);
            if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size())
                mId = std::max(mId, id);
        }
        if (scanError || mId == std::numeric_limits<uint64_t>::max())
        {
            SetError(error, "could not allocate a unique shader generation id");
            Unlock();
            return false;
        }
        ++mId;
        mInputs.clear();
        mOutputs.clear();
        mFinished = false;
        static std::atomic_uint64_t stagingSequence{0};
        mStaging = mRoot / "generations" / (".staging-" + std::to_string(mId) + "-" +
            std::to_string(CurrentProcessId()) + "-" + std::to_string(stagingSequence.fetch_add(1)));

        std::error_code status;
        std::filesystem::remove_all(mStaging, status);
        std::filesystem::create_directories(mStaging, status);
        if (status)
        {
            SetError(error, "could not create the shader staging directory: " + status.message());
            Unlock();
            return false;
        }
        mPublishing = true;
        if (!CloneBase(error))
        {
            std::filesystem::remove_all(mStaging, status);
            mPublishing = false;
            Unlock();
            return false;
        }
        if (outId != nullptr)
            *outId = mId;
        return true;
    }

    bool ShaderGenerationStore::CloneBase(std::string *error)
    {
        const std::filesystem::path base = mParent != 0 ? GenerationPath(mParent) : mRoot;
        if (mParent != 0)
        {
            std::string readError;
            if (const auto snapshot = ReadGeneration(mRoot, mParent, &readError))
            {
                mOutputs = snapshot->mOutputs;
            }
            else if (std::filesystem::exists(base / "metadata.json"))
            {
                SetError(error, "current shader generation is not readable: " + readError);
                return false;
            }
        }

        std::error_code status;
        if (!std::filesystem::is_directory(base, status))
            return true;
        for (const std::filesystem::directory_entry &entry :
            std::filesystem::directory_iterator(base, std::filesystem::directory_options::skip_permission_denied,
                status))
        {
            if (status)
                break;
            if (!entry.is_regular_file(status))
                continue;
            const std::string name = Utf8(entry.path().filename());
            if (name == "current.json" || name == "previous.json" || name == ".publication.lock" ||
                name == "metadata.json")
                continue;
            if (!std::filesystem::copy_file(entry.path(), mStaging / std::filesystem::u8path(name),
                    std::filesystem::copy_options::overwrite_existing, status))
            {
                if (status)
                {
                    SetError(error, "could not clone the base shader generation: " + status.message());
                    return false;
                }
            }
        }
        if (mParent == 0)
        {
            mOutputs.clear();
            for (const std::filesystem::directory_entry &entry :
                std::filesystem::directory_iterator(mStaging, status))
            {
                if (!entry.is_regular_file(status))
                    continue;
                if (const auto file = SnapshotShaderFile(entry.path(), Utf8(entry.path().filename())))
                    mOutputs.push_back(*file);
            }
        }
        return true;
    }

    bool ShaderGenerationStore::StageFile(std::string_view name, std::span<const std::byte> bytes,
        std::string *error)
    {
        if (!mPublishing || mFinished || name.empty() || std::filesystem::u8path(name).has_parent_path() ||
            name == "." || name == ".." || name.find('\0') != std::string_view::npos ||
            name == "metadata.json" || name == "current.json" || name == "previous.json" || name == ".publication.lock")
        {
            SetError(error, "invalid staged shader name");
            return false;
        }
        if (!WriteBytes(mStaging / std::filesystem::u8path(name), bytes))
        {
            SetError(error, "could not stage shader output '" + std::string(name) + "'");
            return false;
        }
        ShaderGenerationFile file{ std::string(name), bytes.size(), HashShaderContent(bytes) };
        mOutputs.erase(std::remove_if(mOutputs.begin(), mOutputs.end(),
            [&](const ShaderGenerationFile &existing) { return existing.mPath == file.mPath; }), mOutputs.end());
        mOutputs.push_back(std::move(file));
        return true;
    }

    bool ShaderGenerationStore::StageFromFile(std::string_view name, const std::filesystem::path &source,
        std::string *error)
    {
        const std::optional<std::vector<std::byte>> bytes = ReadBytes(source);
        if (!bytes)
        {
            SetError(error, "shader output is missing: " + source.string());
            return false;
        }
        return StageFile(name, *bytes, error);
    }

    void ShaderGenerationStore::SetInputs(std::vector<ShaderGenerationFile> inputs)
    {
        mInputs = std::move(inputs);
    }

    bool ShaderGenerationStore::ValidateInputs(std::string *error) const
    {
        return VerifyShaderSnapshot(mInputs, error);
    }

    bool ShaderGenerationStore::VerifyOutputs(const std::filesystem::path &directory, std::string *error) const
    {
        for (const ShaderGenerationFile &file : mOutputs)
        {
            const std::optional<std::vector<std::byte>> bytes = ReadBytes(directory / std::filesystem::u8path(file.mPath));
            if (!bytes || bytes->size() != file.mSize || HashShaderContent(*bytes) != file.mContentHash)
            {
                SetError(error, "staged shader output is incomplete: " + file.mPath);
                return false;
            }
        }
        return true;
    }

    bool ShaderGenerationStore::WriteMetadata(std::string *error)
    {
        const nlohmann::json metadata{
            { "version", 1 },
            { "generation", mId },
            { "parent", mParent },
            { "inputs", ToJson(mInputs) },
            { "outputs", ToJson(mOutputs) } };
        if (!WriteTextAtomic(mStaging / "metadata.json", metadata.dump(2) + "\n"))
        {
            SetError(error, "could not write shader generation metadata");
            return false;
        }
        return true;
    }

    bool ShaderGenerationStore::Finish(std::string *error)
    {
        if (!mPublishing || mFinished || !mLocked)
            return false;
        if (!ValidateInputs(error))
            return false;
        if (!VerifyOutputs(mStaging, error))
            return false;
        if (!WriteMetadata(error))
            return false;

        std::error_code status;
        const std::filesystem::path target = GenerationPath(mId);
        if (std::filesystem::exists(target, status))
        {
            SetError(error, "shader generation id collides with a sealed generation");
            return false;
        }
        std::filesystem::rename(mStaging, target, status);
        if (status)
        {
            SetError(error, "could not seal shader generation: " + status.message());
            return false;
        }
        mFinished = true;
        return true;
    }

    bool ShaderGenerationStore::PublishPointer(std::string *error)
    {
        if (!mLocked || !mPublishing || !mFinished)
        {
            SetError(error, "shader generation cannot be promoted without the writer lock");
            return false;
        }
        if (CurrentId().value_or(0) != mExpectedCurrent)
        {
            SetError(error, "shader generation parent changed under the writer lock");
            return false;
        }
        if (!ValidateInputs(error))
            return false;
        if (!VerifyOutputs(GenerationPath(mId), error))
            return false;

        const std::filesystem::path pointer = mRoot / "current.json";
        const std::filesystem::path previous = mRoot / "previous.json";
        if (mParent != 0)
        {
            const nlohmann::json prior{{"version", 1}, {"generation", mParent}};
            if (!WriteTextAtomic(previous, prior.dump()))
            {
                SetError(error, "could not preserve the previous shader pointer");
                return false;
            }
        }
        const std::filesystem::path temporary = mRoot / ("current.json.tmp." + std::to_string(mId));
        const nlohmann::json root{ { "version", 1 }, { "generation", mId } };
        if (!WriteTextAtomic(temporary, root.dump(2) + "\n"))
        {
            SetError(error, "could not write the shader generation pointer");
            return false;
        }
#if defined(_WIN32)
        if (!MoveFileExW(temporary.wstring().c_str(), pointer.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            SetError(error, "could not promote the shader generation pointer");
            return false;
        }
#else
        std::error_code status;
        std::filesystem::rename(temporary, pointer, status);
        if (status)
        {
            SetError(error, "could not promote the shader generation pointer: " + status.message());
            return false;
        }
#endif
        return true;
    }

    void ShaderGenerationStore::Abort()
    {
        if (!mPublishing)
            return;
        std::error_code ignored;
        std::filesystem::remove_all(mStaging, ignored);
        mPublishing = false;
        mFinished = false;
        mInputs.clear();
        mOutputs.clear();
    }

    std::optional<ShaderGenerationSnapshot> ShaderGenerationStore::ReadGeneration(
        const std::filesystem::path &root, uint64_t id, std::string *error)
    {
        const std::filesystem::path generation = root / "generations" / GenerationName(id);
        const auto metadata = ReadMetadata(generation, error);
        if (!metadata)
            return std::nullopt;
        try
        {
            if (metadata->value("version", 0u) != 1u)
            {
                SetError(error, "shader generation metadata has an unsupported version");
                return std::nullopt;
            }
            ShaderGenerationSnapshot snapshot;
            if (!metadata->contains("generation") || !metadata->at("generation").is_number_unsigned() ||
                !metadata->contains("parent") || !metadata->at("parent").is_number_unsigned())
                throw std::runtime_error("shader generation lineage is incomplete");
            snapshot.mId = metadata->at("generation").get<uint64_t>();
            snapshot.mParent = metadata->at("parent").get<uint64_t>();
            if (id == 0 || snapshot.mId != id)
            {
                SetError(error, "shader generation metadata id does not match its directory");
                return std::nullopt;
            }
            snapshot.mOutputs = FromJson(metadata->at("outputs"));
            snapshot.mInputs = FromJson(metadata->at("inputs"));
            if (snapshot.mOutputs.empty())
            {
                SetError(error, "shader generation metadata has no outputs");
                return std::nullopt;
            }
            std::set<std::string> names;
            for (const ShaderGenerationFile &file : snapshot.mOutputs)
            {
                const auto relative = std::filesystem::u8path(file.mPath);
                if (relative.is_absolute() || relative.has_parent_path() || file.mPath == "." ||
                    file.mPath == ".." || !names.insert(file.mPath).second)
                {
                    SetError(error, "shader generation output path is not a bare file name");
                    return std::nullopt;
                }
                const std::optional<std::vector<std::byte>> bytes = ReadBytes(generation / relative);
                if (!bytes || bytes->size() != file.mSize || HashShaderContent(*bytes) != file.mContentHash)
                {
                    SetError(error, "shader generation output is corrupt: " + file.mPath);
                    return std::nullopt;
                }
            }
            return snapshot;
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("shader generation metadata is malformed: ") + exception.what());
            return std::nullopt;
        }
    }

    std::optional<ShaderGenerationSnapshot> ShaderGenerationStore::ReadCurrent(
        const std::filesystem::path &root, std::string *error)
    {
        const auto readPointer = [&](const std::filesystem::path &path, uint64_t &outId) -> bool
            {
                const std::optional<std::string> text = Exists(path) ? ReadText(path) : std::nullopt;
                if (!text)
                    return false;
                try
                {
                    const nlohmann::json pointer = nlohmann::json::parse(*text);
                    if (pointer.value("version", 0u) != 1u || !pointer.contains("generation") ||
                        !pointer.at("generation").is_number_unsigned())
                        return false;
                    outId = pointer.at("generation").get<uint64_t>();
                    return true;
                }
                catch (const std::exception &)
                {
                    return false;
                }
            };

        uint64_t id = 0;
        std::string currentError;
        if (readPointer(root / "current.json", id))
        {
            if (const auto current = ReadGeneration(root, id, &currentError))
                return current;
        }
        else
            SetError(&currentError, "shader generation pointer is missing or malformed");

        uint64_t previousId = 0;
        if (readPointer(root / "previous.json", previousId))
        {
            std::string previousError;
            if (const auto previous = ReadGeneration(root, previousId, &previousError))
                return previous;
        }
        if (error != nullptr)
            *error = currentError;
        return std::nullopt;
    }
}

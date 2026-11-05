/**
 * @file ShaderCompiler.cpp
 * @author Jackey Cen
 * @brief Bounded compiler jobs and source-closure stabilization.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */
#include <renderer/ShaderCompiler.h>

#include <core/fs/FileIO.h>
#include <core/platform/Process.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mir
{
    namespace
    {
        constexpr size_t kOutputLimit = 64 * 1024;

        std::string Utf8(const std::filesystem::path &path)
        {
            const auto bytes = path.generic_u8string();
            return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
        }

        std::filesystem::path Resolve(std::string_view name, const std::filesystem::path &cwd)
        {
            auto path = std::filesystem::u8path(name);
            if (path.is_relative())
                path = cwd / path;
            std::error_code error;
            const auto canonical = std::filesystem::weakly_canonical(path, error);
            return error ? path.lexically_normal() : canonical;
        }

        std::optional<std::string> OptionalText(const std::filesystem::path &path)
        {
            std::error_code error;
            return std::filesystem::is_regular_file(path, error) ? ReadText(path) : std::nullopt;
        }

        nlohmann::json FilesJson(const std::vector<ShaderGenerationFile> &files)
        {
            auto json = nlohmann::json::array();
            for (const auto &file : files)
                json.push_back({{"path", file.mPath}, {"size", file.mSize}, {"hash", file.mContentHash}});
            return json;
        }

        std::vector<ShaderGenerationFile> ReadFiles(const nlohmann::json &json)
        {
            std::vector<ShaderGenerationFile> files;
            for (const auto &entry : json)
                files.push_back({entry.at("path").get<std::string>(), entry.at("size").get<uint64_t>(),
                    entry.at("hash").get<uint64_t>()});
            return files;
        }

        bool Cancelled(const std::atomic_bool *cancel)
        {
            return cancel != nullptr && cancel->load(std::memory_order_relaxed);
        }

        std::string DepfileToken(std::string_view value)
        {
            std::string escaped;
            for (const char character : value)
            {
                if (character == '$') escaped += '$';
                if (character == ' ' || character == '#' || character == ':' || character == '\\')
                    escaped += '\\';
                escaped += character;
            }
            return escaped;
        }
    }

    ShaderCompilerResult RunShaderCompiler(const std::filesystem::path &program,
        std::span<const std::string> arguments, const std::filesystem::path &workingDirectory,
        const std::atomic_bool *cancel, std::chrono::milliseconds timeout)
    {
        ShaderCompilerResult result;
        if (Cancelled(cancel))
        {
            result.cancelled = true;
            return result;
        }
#if defined(_WIN32)
        struct Handle
        {
            HANDLE value = nullptr;
            ~Handle() { if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        } readPipe, writePipe, process, thread, job, input;
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&readPipe.value, &writePipe.value, &attributes, 0) ||
            !SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0))
        {
            result.diagnostic = "could not create the compiler output pipe";
            return result;
        }
        std::string command = QuoteCommandLineArgument(Utf8(program));
        for (const auto &argument : arguments)
            command += " " + QuoteCommandLineArgument(argument);
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.data(),
            static_cast<int>(command.size()), nullptr, 0);
        if (length <= 0)
        {
            result.diagnostic = "compiler arguments are not valid UTF-8";
            return result;
        }
        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.data(), static_cast<int>(command.size()),
            wide.data(), length);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = writePipe.value;
        startup.hStdError = writePipe.value;
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        startup.hStdInput = input.value;
        PROCESS_INFORMATION info{};
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job.value == nullptr || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits)) ||
            !CreateProcessW(program.wstring().c_str(), wide.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                workingDirectory.empty() ? nullptr : workingDirectory.wstring().c_str(), &startup, &info))
        {
            result.diagnostic = "could not start the shader compiler";
            return result;
        }
        process.value = info.hProcess;
        thread.value = info.hThread;
        CloseHandle(writePipe.value);
        writePipe.value = nullptr;
        if (!AssignProcessToJobObject(job.value, process.value))
        {
            TerminateProcess(process.value, 1);
            WaitForSingleObject(process.value, 5000);
            result.diagnostic = "could not own the compiler process";
            return result;
        }
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
        {
            TerminateJobObject(job.value, 1);
            result.diagnostic = "could not resume the shader compiler";
            return result;
        }
        const auto started = std::chrono::steady_clock::now();
        const auto drain = [&]
            {
                DWORD available = 0;
                std::array<char, 4096> bytes{};
                for (int chunk = 0; chunk < 32 &&
                    PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr) && available != 0; ++chunk)
                {
                    DWORD read = 0;
                    if (!ReadFile(readPipe.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
                        read == 0)
                        break;
                    if (result.diagnostic.size() < kOutputLimit)
                        result.diagnostic.append(bytes.data(),
                            std::min<size_t>(read, kOutputLimit - result.diagnostic.size()));
                }
            };
        for (;;)
        {
            drain();
            if (WaitForSingleObject(process.value, 10) == WAIT_OBJECT_0)
                break;
            result.cancelled = Cancelled(cancel);
            if (result.cancelled || std::chrono::steady_clock::now() - started >= timeout)
            {
                TerminateJobObject(job.value, 1);
                WaitForSingleObject(process.value, 5000);
                const std::string suffix = result.cancelled ? "\ncompiler job cancelled" : "\ncompiler job timed out";
                if (result.diagnostic.size() + suffix.size() > kOutputLimit)
                    result.diagnostic.resize(kOutputLimit - suffix.size());
                result.diagnostic += suffix;
                break;
            }
        }
        drain();
        DWORD exitCode = 1;
        GetExitCodeProcess(process.value, &exitCode);
        result.exitCode = static_cast<int>(exitCode);
        result.success = exitCode == 0 && !result.cancelled &&
            std::chrono::steady_clock::now() - started < timeout;
#else
        ChildProcess child;
        if (!child.Start(program, arguments, workingDirectory))
        {
            result.diagnostic = "could not start the shader compiler";
            return result;
        }
        const auto started = std::chrono::steady_clock::now();
        while (child.Running())
        {
            if (Cancelled(cancel) || std::chrono::steady_clock::now() - started >= timeout)
            {
                result.cancelled = Cancelled(cancel);
                child.Terminate();
                result.diagnostic = result.cancelled ? "compiler job cancelled" : "compiler job timed out";
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        result.exitCode = child.Poll().value_or(-1);
        result.success = result.exitCode == 0;
#endif
        return result;
    }

    std::filesystem::path UniqueShaderJobDirectory(const std::filesystem::path &parent)
    {
        static std::atomic_uint64_t sequence{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto name = std::to_string(CurrentProcessId()) + "-" + std::to_string(tick) + "-" +
            std::to_string(sequence.fetch_add(1));
        const auto directory = parent / name;
        std::filesystem::create_directories(directory);
        return directory;
    }

    ShaderArtifact CompileShaderArtifact(const ShaderManifest &manifest, const ShaderManifestOutput &definition,
        const std::filesystem::path &directory, const std::filesystem::path &cacheDirectory,
        const std::atomic_bool *cancel)
    {
        ShaderArtifact artifact;
        artifact.output = definition;
        const auto name = Utf8(std::filesystem::u8path(definition.output).filename());
        artifact.output.output = Utf8(directory / std::filesystem::u8path(name));
        artifact.output.reflection = Utf8(directory / std::filesystem::u8path(definition.reflection).filename());
        artifact.output.depfile = Utf8(directory / std::filesystem::u8path(definition.depfile).filename());
        const std::array<std::string, 3> names{name,
            Utf8(std::filesystem::u8path(definition.reflection).filename()),
            Utf8(std::filesystem::u8path(definition.depfile).filename())};
        const std::set<std::string> distinct(names.begin(), names.end());
        if (distinct.size() != names.size() || distinct.contains("") || distinct.contains(".") || distinct.contains(".."))
        { artifact.diagnostic = "invalid compiler artifact names"; return artifact; }
        std::filesystem::create_directories(directory);
        const std::string key = nlohmann::json{{"compiler", Utf8(manifest.slangc)}, {"argv", definition.arguments},
            {"cwd", Utf8(manifest.workingDirectory)}, {"sources", definition.sources}}.dump();
        const auto cacheMetadata = cacheDirectory / std::filesystem::u8path(name + ".compile.json");
        std::set<std::string> known;
        for (const auto &source : definition.sources)
            known.insert(Utf8(Resolve(source, manifest.workingDirectory)));
        try
        {
            if (!cacheDirectory.empty())
                if (const auto text = OptionalText(cacheMetadata))
                {
                    const auto cached = nlohmann::json::parse(*text);
                    const auto inputs = ReadFiles(cached.at("inputs"));
                    for (const auto &input : inputs)
                        known.insert(input.mPath);
                    if (cached.at("version") == 1 && cached.at("key") == key && VerifyShaderSnapshot(inputs))
                    {
                        bool valid = true;
                        const auto files = ReadFiles(cached.at("outputs"));
                        std::set<std::string> cachedNames;
                        for (const auto &file : files) cachedNames.insert(file.mPath);
                        if (files.size() != names.size() || cachedNames != distinct)
                            valid = false;
                        for (const auto &file : files)
                        {
                            if (std::find(names.begin(), names.end(), file.mPath) == names.end())
                            { valid = false; break; }
                            const auto bytes = ReadBytes(cacheDirectory / std::filesystem::u8path(file.mPath));
                            if (!bytes || bytes->size() != file.mSize || HashShaderContent(*bytes) != file.mContentHash ||
                                !WriteBytes(directory / std::filesystem::u8path(file.mPath), *bytes))
                            { valid = false; break; }
                        }
                        if (valid && VerifyShaderSnapshot(inputs))
                        {
                            artifact.inputs = inputs;
                            artifact.cached = true;
                            artifact.success = true;
                            return artifact;
                        }
                    }
                }
        }
        catch (const nlohmann::json::exception &) {}
        for (int pass = 0; pass < 3; ++pass)
        {
            artifact.inputs.clear();
            for (const auto &path : known)
                if (std::error_code error; std::filesystem::is_regular_file(std::filesystem::u8path(path), error))
                    if (const auto snapshot = SnapshotShaderFile(std::filesystem::u8path(path), path))
                    artifact.inputs.push_back(*snapshot);
            for (const auto &file : names)
            {
                std::error_code error;
                std::filesystem::remove(directory / std::filesystem::u8path(file), error);
                if (error)
                { artifact.diagnostic = "could not clear an old compiler artifact"; return artifact; }
            }
            std::vector<std::string> arguments;
            for (size_t i = 1; i < definition.arguments.size(); ++i)
            {
                const auto &argument = definition.arguments[i];
                arguments.push_back(argument);
                if (argument == "-o" || argument == "-reflection-json" || argument == "-depfile")
                {
                    if (++i >= definition.arguments.size())
                    { artifact.diagnostic = "compiler output flag has no argument"; return artifact; }
                    arguments.push_back(argument == "-o" ? artifact.output.output
                        : argument == "-depfile" ? artifact.output.depfile : artifact.output.reflection);
                }
            }
            const auto process = RunShaderCompiler(manifest.slangc, arguments, manifest.workingDirectory, cancel);
            artifact.cancelled = process.cancelled;
            artifact.diagnostic = process.diagnostic;
            if (!process.success)
            {
                artifact.diagnostic += "\nshader compiler exited with code " + std::to_string(process.exitCode);
                return artifact;
            }
            const auto spirv = ReadBytes(std::filesystem::u8path(artifact.output.output));
            const auto reflection = ReadText(std::filesystem::u8path(artifact.output.reflection));
            const auto depfile = ReadText(std::filesystem::u8path(artifact.output.depfile));
            if (!spirv || !reflection || reflection->empty() || !depfile ||
                SpirvEntryPointStage(*spirv, definition.entry) != definition.stage)
            {
                artifact.diagnostic += "\ncompiler did not produce a complete shader artifact: " +
                    artifact.output.output + " (SPIR-V=" + std::to_string(spirv ? spirv->size() : 0) +
                    ", reflection=" + std::to_string(reflection ? reflection->size() : 0) +
                    ", depfile=" + std::to_string(depfile ? depfile->size() : 0) + ")";
                if (spirv)
                    artifact.diagnostic += " entry stage=" + SpirvEntryPointStage(*spirv, definition.entry).value_or("missing") +
                        ", expected=" + definition.stage;
                return artifact;
            }
            const auto dependencies = ParseShaderDepfile(*depfile, &artifact.diagnostic);
            if (!dependencies)
                return artifact;
            bool expanded = false;
            for (const auto &dependency : *dependencies)
                expanded |= known.insert(Utf8(Resolve(dependency, manifest.workingDirectory))).second;
            if (!VerifyShaderSnapshot(artifact.inputs, &artifact.diagnostic))
            { artifact.superseded = true; return artifact; }
            if (expanded)
                continue;
            std::string normalizedDepfile = DepfileToken(definition.output) + ":";
            for (const auto &input : artifact.inputs)
                normalizedDepfile += " " + DepfileToken(input.mPath);
            normalizedDepfile += '\n';
            if (!WriteText(std::filesystem::u8path(artifact.output.depfile), normalizedDepfile))
            { artifact.diagnostic = "could not normalize the shader depfile"; return artifact; }
            if (!cacheDirectory.empty())
            {
                std::filesystem::create_directories(cacheDirectory);
                std::vector<ShaderGenerationFile> outputs;
                for (const auto &file : names)
                {
                    const auto bytes = ReadBytes(directory / std::filesystem::u8path(file));
                    if (!bytes || !WriteBytesAtomic(cacheDirectory / std::filesystem::u8path(file), *bytes))
                    { artifact.diagnostic = "could not write the shader compiler cache"; return artifact; }
                    outputs.push_back({file, bytes->size(), HashShaderContent(*bytes)});
                }
                if (!WriteTextAtomic(cacheMetadata, nlohmann::json{{"version", 1}, {"key", key},
                        {"inputs", FilesJson(artifact.inputs)}, {"outputs", FilesJson(outputs)}}.dump()))
                { artifact.diagnostic = "could not write the shader compiler cache metadata"; return artifact; }
            }
            artifact.success = true;
            return artifact;
        }
        artifact.superseded = true;
        artifact.diagnostic = "shader dependency closure did not stabilize";
        return artifact;
    }
}

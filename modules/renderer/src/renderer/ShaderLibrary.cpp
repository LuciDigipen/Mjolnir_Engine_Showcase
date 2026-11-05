/**
 * @file ShaderLibrary.cpp
 * @author Jackey Cen
 * @brief Shader manifest, dependency, ABI and staged-publication implementation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/ShaderLibrary.h>
#include <renderer/ShaderGeneration.h>

#include <core/fs/FileIO.h>

#include <nlohmann/json.hpp>
#include <spirv_reflect.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <system_error>

namespace mir
{
    namespace
    {
        void SetError(std::string *error, std::string message)
        {
            if (error != nullptr)
                *error = std::move(message);
        }

        std::optional<std::vector<std::string>> ReadStringArray(const nlohmann::json &value,
            std::string_view key, std::string *error, std::string_view owner)
        {
            if (!value.contains(key) || !value.at(key).is_array())
            {
                SetError(error, std::string(owner) + " is missing array '" + std::string(key) + "'");
                return std::nullopt;
            }
            std::vector<std::string> result;
            for (const nlohmann::json &item : value.at(key))
            {
                if (!item.is_string())
                {
                    SetError(error, std::string(owner) + " has a non-string member in '" + std::string(key) + "'");
                    return std::nullopt;
                }
                result.push_back(item.get<std::string>());
            }
            return result;
        }

        std::optional<std::string> ReadString(const nlohmann::json &value, std::string_view key,
            std::string *error, std::string_view owner, bool required = true)
        {
            if (!value.contains(key) || !value.at(key).is_string())
            {
                if (!required)
                    return std::string{};
                SetError(error, std::string(owner) + " is missing string '" + std::string(key) + "'");
                return std::nullopt;
            }
            return value.at(key).get<std::string>();
        }

        // Make-style escaping: a backslash quotes a space, '#', ':' or another
        // backslash. Any other backslash stays literal so "C:\src\file" survives.
        std::vector<std::string> SplitMakeWords(std::string_view text, std::size_t begin, std::size_t end)
        {
            std::vector<std::string> words;
            std::string current;
            bool quoted = false;
            for (std::size_t i = begin; i < end; ++i)
            {
                const char character = text[i];
                if (character == '"')
                {
                    quoted = !quoted;
                    continue;
                }
                if (character == '\\' && i + 1 < end)
                {
                    const char next = text[i + 1];
                    if (next == ' ' || next == '#' || next == ':' || next == '"' || next == '\\' ||
                        next == '\t')
                    {
                        current.push_back(next);
                        ++i;
                        continue;
                    }
                    current.push_back(character);
                    continue;
                }
                if ((character == ' ' || character == '\t' || character == '\r') && !quoted)
                {
                    if (!current.empty())
                    {
                        words.push_back(current);
                        current.clear();
                    }
                    continue;
                }
                current.push_back(character);
            }
            if (!current.empty())
                words.push_back(current);
            return words;
        }

        std::string BindingSignature(const nlohmann::json &binding)
        {
            if (!binding.is_object())
                return "";
            std::string result;
            if (binding.contains("offset") && binding.at("offset").is_number_unsigned())
                result += "@" + std::to_string(binding.at("offset").get<uint64_t>());
            if (binding.contains("size") && binding.at("size").is_number_unsigned())
                result += "+" + std::to_string(binding.at("size").get<uint64_t>());
            for (const char *key : { "elementStride", "stride" })
            {
                if (binding.contains(key) && binding.at(key).is_number_unsigned() &&
                    binding.at(key).get<uint64_t>() != 0)
                    result += "/" + std::to_string(binding.at(key).get<uint64_t>());
            }
            return result;
        }

        std::string TypeSignature(const nlohmann::json &type)
        {
            if (!type.is_object())
                return "?";
            const std::string kind = type.value("kind", "?");
            std::string result = kind;
            if (type.contains("scalarType") && type.at("scalarType").is_string())
                result += ":" + type.at("scalarType").get<std::string>();
            if (type.contains("baseShape") && type.at("baseShape").is_string())
                result += ":" + type.at("baseShape").get<std::string>();
            if (type.contains("access") && type.at("access").is_string())
                result += ":" + type.at("access").get<std::string>();
            if (type.contains("elementCount") && type.at("elementCount").is_number_unsigned())
                result += "[" + std::to_string(type.at("elementCount").get<uint64_t>()) + "]";
            if (type.contains("rowCount") && type.contains("columnCount"))
                result += "x" + std::to_string(type.at("rowCount").get<uint64_t>()) + "x" +
                    std::to_string(type.at("columnCount").get<uint64_t>());
            if (type.contains("elementType"))
                result += "<" + TypeSignature(type.at("elementType")) + ">";
            if (type.contains("resultType"))
                result += "->" + TypeSignature(type.at("resultType"));
            if (type.contains("fields") && type.at("fields").is_array())
            {
                result += "{";
                for (const nlohmann::json &field : type.at("fields"))
                {
                    result += field.value("name", "?") + "=";
                    if (field.contains("type"))
                        result += TypeSignature(field.at("type"));
                    if (field.contains("binding"))
                        result += BindingSignature(field.at("binding"));
                    result += ";";
                }
                result += "}";
            }
            for (const char *key : { "containerVarLayout", "elementVarLayout" })
            {
                if (type.contains(key) && type.at(key).is_object() && type.at(key).contains("binding"))
                    result += std::string("|") + key + BindingSignature(type.at(key).at("binding"));
            }
            return result;
        }

        std::string IoType(const nlohmann::json &value)
        {
            return value.contains("type") ? TypeSignature(value.at("type")) : std::string("?");
        }

        void CollectVaryings(const nlohmann::json &value, std::vector<ShaderStageIo> &out, bool outputs)
        {
            if (!value.is_object())
                return;
            const nlohmann::json &binding = value.contains("binding") ? value.at("binding") : nlohmann::json{};
            const std::string kind = binding.is_object() ? binding.value("kind", "") : "";
            const bool hasFields = value.contains("type") && value.at("type").is_object() &&
                value.at("type").contains("fields") && value.at("type").at("fields").is_array() &&
                !value.at("type").at("fields").empty();
            if (kind == "varyingInput" || kind == "varyingOutput")
            {
                const std::string semantic = value.value("semanticName", "");
                if (!semantic.empty() && semantic.rfind("SV", 0) == 0 &&
                    !(outputs && semantic == "SV_TARGET"))
                    return;
                if (!hasFields)
                {
                    ShaderStageIo io;
                    io.location = binding.value("index", uint32_t{ 0 });
                    io.semantic = semantic;
                    if (value.contains("semanticIndex") && value.at("semanticIndex").is_number_unsigned())
                        io.semantic += std::to_string(value.at("semanticIndex").get<uint64_t>());
                    io.type = IoType(value);
                    out.push_back(std::move(io));
                }
            }
            else if (!kind.empty() && kind != "uniform" && kind != "descriptorTableSlot" &&
                kind != "pushConstantBuffer")
            {
                ShaderStageIo io;
                io.location = binding.value("index", uint32_t{ 0 });
                io.semantic = kind + ":" + value.value("name", "");
                io.type = IoType(value);
                out.push_back(std::move(io));
            }

            if (hasFields && (kind == "varyingInput" || kind == "varyingOutput"))
            {
                for (const nlohmann::json &field : value.at("type").at("fields"))
                    CollectVaryings(field, out, outputs);
            }
        }

        uint64_t LargestSize(const nlohmann::json &value)
        {
            if (!value.is_object())
                return 0;
            uint64_t size = 0;
            if (value.contains("binding") && value.at("binding").is_object() &&
                value.at("binding").contains("size") && value.at("binding").at("size").is_number_unsigned())
            {
                size = std::max(size, value.at("binding").at("size").get<uint64_t>());
            }
            for (const auto &item : value.items())
                size = std::max(size, LargestSize(item.value()));
            return size;
        }

        uint64_t HashFingerprint(const ShaderSourceFingerprint &fingerprint)
        {
            uint64_t hash = 1469598103934665603ull;
            hash ^= fingerprint.size;
            hash *= 1099511628211ull;
            hash ^= fingerprint.writeTime;
            hash *= 1099511628211ull;
            hash ^= fingerprint.contentHash;
            hash *= 1099511628211ull;
            return hash;
        }
    }

    const ShaderManifestOutput *ShaderManifest::Find(std::string_view outputPath) const
    {
        for (const ShaderManifestOutput &output : outputs)
            if (output.output == outputPath)
                return &output;

        const std::filesystem::path wanted(outputPath);
        for (const ShaderManifestOutput &output : outputs)
            if (std::filesystem::path(output.output).filename() == wanted.filename() &&
                wanted.filename() == wanted)
                return &output;
        return nullptr;
    }

    std::optional<ShaderManifest> LoadShaderManifest(const std::filesystem::path &path, std::string *error)
    {
        const std::optional<std::string> text = ReadText(path);
        if (!text)
        {
            SetError(error, "shader manifest is missing: " + path.string());
            return std::nullopt;
        }

        try
        {
            const nlohmann::json root = nlohmann::json::parse(*text);
            if (!root.is_object() || !root.contains("version") || !root.at("version").is_number_unsigned() ||
                root.at("version").get<uint32_t>() != 1)
            {
                SetError(error, "shader manifest has an unsupported shape or version");
                return std::nullopt;
            }

            ShaderManifest manifest;
            manifest.slangc = std::filesystem::u8path(root.value("slangc", ""));
            manifest.workingDirectory = std::filesystem::u8path(root.value("workingDirectory", ""));
            const auto common = ReadStringArray(root, "commonArguments", error, "shader manifest");
            if (!common || manifest.slangc.empty() || manifest.workingDirectory.empty())
            {
                if (common)
                    SetError(error, "shader manifest has no slangc or working directory");
                return std::nullopt;
            }
            manifest.commonArguments = *common;

            if (!root.contains("outputs") || !root.at("outputs").is_array())
            {
                SetError(error, "shader manifest has no outputs array");
                return std::nullopt;
            }
            for (const nlohmann::json &entry : root.at("outputs"))
            {
                if (!entry.is_object())
                {
                    SetError(error, "shader manifest output is not an object");
                    return std::nullopt;
                }
                ShaderManifestOutput output;
                const auto type = ReadString(entry, "type", error, "shader output", false);
                const auto frontend = ReadString(entry, "frontend", error, "shader output", false);
                const auto sources = ReadStringArray(entry, "sources", error, "shader output");
                const auto entryName = ReadString(entry, "entry", error, "shader output");
                const auto stage = ReadString(entry, "stage", error, "shader output");
                const auto outputPath = ReadString(entry, "output", error, "shader output");
                const auto reflection = ReadString(entry, "reflection", error, "shader output");
                const auto depfile = ReadString(entry, "depfile", error, "shader output");
                const auto arguments = ReadStringArray(entry, "arguments", error, "shader output");
                if (!type || !frontend || !sources || !entryName || !stage || !outputPath || !reflection ||
                    !depfile || !arguments)
                    return std::nullopt;
                output.type = *type;
                output.frontend = *frontend;
                output.sources = *sources;
                output.entry = *entryName;
                output.stage = *stage;
                output.specialize = entry.value("specialize", "");
                output.output = *outputPath;
                output.reflection = *reflection;
                output.depfile = *depfile;
                output.arguments = *arguments;
                if (output.sources.empty() || output.entry.empty() || output.stage.empty() ||
                    output.output.empty() || output.arguments.empty())
                {
                    SetError(error, "shader manifest output is incomplete");
                    return std::nullopt;
                }
                manifest.outputs.push_back(std::move(output));
            }

            if (root.contains("assetRoots") && root.at("assetRoots").is_array())
            {
                for (const nlohmann::json &entry : root.at("assetRoots"))
                {
                    if (!entry.is_object() || !entry.contains("root") || !entry.contains("namespace") ||
                        !entry.at("root").is_string() || !entry.at("namespace").is_string())
                        continue;
                    manifest.assetRoots.push_back(ShaderAssetRoot{ std::filesystem::u8path(entry.at("root").get<std::string>()),
                        entry.at("namespace").get<std::string>() });
                }
            }

            if (root.contains("frontendTemplates"))
            {
                if (!root.at("frontendTemplates").is_array())
                {
                    SetError(error, "shader manifest frontendTemplates is not an array");
                    return std::nullopt;
                }
                for (const nlohmann::json &entry : root.at("frontendTemplates"))
                {
                    ShaderFrontendTemplate frontend;
                    const auto name = ReadString(entry, "frontend", error, "frontend template");
                    const auto placeholders = ReadStringArray(entry, "placeholders", error, "frontend template");
                    const auto vertex = ReadStringArray(entry, "vertex", error, "frontend template");
                    const auto fragment = ReadStringArray(entry, "fragment", error, "frontend template");
                    std::optional<std::vector<std::string>> surfaceFragment{std::in_place};
                    if (entry.contains("surfaceFragment"))
                        surfaceFragment = ReadStringArray(entry, "surfaceFragment", error, "frontend template");
                    const auto raytracing = ReadStringArray(entry, "raytracing", error, "frontend template");
                    const auto layout = ReadStringArray(entry, "layout", error, "frontend template");
                    if (!name || !placeholders || !vertex || !fragment || !raytracing || !layout || !surfaceFragment)
                        return std::nullopt;
                    frontend.frontend = *name;
                    frontend.placeholders = *placeholders;
                    frontend.vertex = *vertex;
                    frontend.fragment = *fragment;
                    frontend.raytracing = *raytracing;
                    frontend.layout = *layout;
                    frontend.surfaceFragment = std::move(*surfaceFragment);
                    for (auto [key, target] : { std::pair{"giRaytracing", &frontend.giRaytracing},
                        std::pair{"probeRaytracing", &frontend.probeRaytracing} })
                    {
                        if (!entry.contains(key)) continue;
                        const auto args = ReadStringArray(entry, key, error, "frontend template");
                        if (!args) return std::nullopt;
                        *target = *args;
                    }
                    manifest.frontendTemplates.push_back(std::move(frontend));
                }
            }
            return manifest;
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("shader manifest is malformed: ") + exception.what());
            return std::nullopt;
        }
    }

    std::string SerializeShaderManifest(const ShaderManifest &manifest)
    {
        const auto utf8 = [](const std::filesystem::path &path)
            {
                const auto bytes = path.generic_u8string();
                return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            };
        nlohmann::json root;
        root["version"] = 1;
        root["slangc"] = utf8(manifest.slangc);
        root["workingDirectory"] = utf8(manifest.workingDirectory);
        root["commonArguments"] = manifest.commonArguments;
        nlohmann::json outputs = nlohmann::json::array();
        for (const ShaderManifestOutput &output : manifest.outputs)
        {
            outputs.push_back({ { "type", output.type }, { "frontend", output.frontend },
                { "sources", output.sources }, { "entry", output.entry }, { "stage", output.stage },
                { "specialize", output.specialize }, { "output", output.output },
                { "reflection", output.reflection }, { "depfile", output.depfile },
                { "arguments", output.arguments } });
        }
        root["outputs"] = std::move(outputs);
        nlohmann::json roots = nlohmann::json::array();
        for (const ShaderAssetRoot &assetRoot : manifest.assetRoots)
            roots.push_back({ { "root", utf8(assetRoot.root) }, { "namespace", assetRoot.nameSpace } });
        root["assetRoots"] = std::move(roots);
        nlohmann::json templates = nlohmann::json::array();
        for (const ShaderFrontendTemplate &frontend : manifest.frontendTemplates)
        {
            templates.push_back({ { "frontend", frontend.frontend }, { "placeholders", frontend.placeholders },
                { "vertex", frontend.vertex }, { "fragment", frontend.fragment },
                { "raytracing", frontend.raytracing }, { "layout", frontend.layout },
                { "surfaceFragment", frontend.surfaceFragment },
                { "giRaytracing", frontend.giRaytracing }, { "probeRaytracing", frontend.probeRaytracing } });
        }
        root["frontendTemplates"] = std::move(templates);
        return root.dump(2) + "\n";
    }
    std::optional<std::vector<std::string>> ParseShaderDepfile(std::string_view text, std::string *error)
    {
        std::string joined;
        joined.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\\' && i + 1 < text.size() &&
                (text[i + 1] == '\n' || (text[i + 1] == '\r' && i + 2 < text.size() && text[i + 2] == '\n')))
            {
                i += text[i + 1] == '\r' ? 2 : 1;
                joined.push_back(' ');
                continue;
            }
            joined.push_back(text[i]);
        }

        std::vector<std::string> unique;
        std::set<std::string> seen;
        bool sawRule = false;
        std::size_t lineBegin = 0;
        while (lineBegin <= joined.size())
        {
            std::size_t lineEnd = joined.find('\n', lineBegin);
            if (lineEnd == std::string::npos)
                lineEnd = joined.size();
            std::size_t trimmed = lineBegin;
            while (trimmed < lineEnd && (joined[trimmed] == ' ' || joined[trimmed] == '\t' || joined[trimmed] == '\r'))
                ++trimmed;
            if (trimmed < lineEnd && joined[trimmed] != '#')
            {
                std::size_t colon = std::string::npos;
                bool quoted = false;
                for (std::size_t i = trimmed; i < lineEnd; ++i)
                {
                    if (joined[i] == '"')
                    {
                        quoted = !quoted;
                        continue;
                    }
                    if (quoted || joined[i] != ':' || (i > trimmed && joined[i - 1] == '\\'))
                        continue;
                    if (i == trimmed + 1 && std::isalpha(static_cast<unsigned char>(joined[trimmed])) &&
                        i + 1 < lineEnd && (joined[i + 1] == '\\' || joined[i + 1] == '/'))
                        continue;
                    colon = i;
                    break;
                }
                if (colon != std::string::npos)
                {
                    sawRule = true;
                    for (std::string &dependency : SplitMakeWords(joined, colon + 1, lineEnd))
                    {
                        if (dependency.empty() || !seen.insert(dependency).second)
                            continue;
                        unique.push_back(std::move(dependency));
                    }
                }
            }
            if (lineEnd == joined.size())
                break;
            lineBegin = lineEnd + 1;
        }

        if (!sawRule)
        {
            SetError(error, "shader depfile has no rule");
            return std::nullopt;
        }
        if (unique.empty())
        {
            SetError(error, "shader depfile lists no usable dependencies");
            return std::nullopt;
        }
        return unique;
    }

    bool ValidateSpirvModule(std::span<const std::byte> bytes, std::string *error)
    {
        if (bytes.size() < 20 || bytes.size() % sizeof(uint32_t) != 0)
        {
            SetError(error, "SPIR-V module has an invalid byte length");
            return false;
        }
        uint32_t magic = 0;
        std::memcpy(&magic, bytes.data(), sizeof(magic));
        if (magic != 0x07230203u)
        {
            SetError(error, "SPIR-V module has an invalid magic number");
            return false;
        }
        return true;
    }

    std::optional<std::string> SpirvEntryPointStage(std::span<const std::byte> bytes,
        std::string_view entryPoint, std::string *error)
    {
        if (!ValidateSpirvModule(bytes, error))
            return std::nullopt;

        const auto word = [&](std::size_t index)
            {
                uint32_t value = 0;
                std::memcpy(&value, bytes.data() + index * sizeof(uint32_t), sizeof(value));
                return value;
            };
        const std::size_t count = bytes.size() / sizeof(uint32_t);
        constexpr uint32_t kOpEntryPoint = 15;
        for (std::size_t i = 5; i + 2 < count;)
        {
            const uint32_t instruction = word(i);
            const uint16_t opcode = static_cast<uint16_t>(instruction & 0xffff);
            const uint16_t length = static_cast<uint16_t>(instruction >> 16);
            if (length == 0)
                break;
            if (opcode == kOpEntryPoint && length >= 4 && i + length <= count)
            {
                const uint32_t model = word(i + 1);
                std::string name;
                bool terminated = false;
                for (std::size_t j = i + 3; j < i + length && !terminated; ++j)
                {
                    const uint32_t value = word(j);
                    for (unsigned shift = 0; shift < 32; shift += 8)
                    {
                        const char character = static_cast<char>((value >> shift) & 0xff);
                        if (character == '\0')
                        {
                            terminated = true;
                            break;
                        }
                        name.push_back(character);
                    }
                }
                if (terminated && name == entryPoint)
                {
                    switch (model)
                    {
                    case 0: return "vertex";
                    case 1: return "tesscontrol";
                    case 2: return "tesseval";
                    case 3: return "geometry";
                    case 4: return "fragment";
                    case 5: return "compute";
                    case 5313: return "raygeneration";
                    case 5314: return "intersection";
                    case 5315: return "anyhit";
                    case 5316: return "closesthit";
                    case 5317: return "miss";
                    case 5318: return "callable";
                    default:
                        SetError(error, "SPIR-V entry point has an unsupported execution model");
                        return std::nullopt;
                    }
                }
            }
            i += length;
        }
        SetError(error, "SPIR-V module has no entry point '" + std::string(entryPoint) + "'");
        return std::nullopt;
    }

    std::optional<ShaderReflectionSignature> ParseShaderReflection(std::string_view json,
        std::string_view expectedStage, std::string *error)
    {
        nlohmann::json root;
        try
        {
            root = nlohmann::json::parse(json);
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("shader reflection is not JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!root.is_object())
        {
            SetError(error, "shader reflection must be an object");
            return std::nullopt;
        }

        try
        {
            if (!root.contains("entryPoints") || !root.at("entryPoints").is_array() ||
                root.at("entryPoints").empty() || !root.at("entryPoints")[0].is_object())
            {
                SetError(error, "shader reflection has no entry point");
                return std::nullopt;
            }
            const nlohmann::json &entry = root.at("entryPoints")[0];
            ShaderReflectionSignature signature;
            signature.entryPoint = entry.value("name", "");
            signature.stage = entry.value("stage", std::string(expectedStage));
            if (signature.entryPoint.empty())
            {
                SetError(error, "shader reflection entry point has no name");
                return std::nullopt;
            }

            std::map<std::string, const nlohmann::json *> parameters;
            if (root.contains("parameters") && root.at("parameters").is_array())
            {
                for (const nlohmann::json &parameter : root.at("parameters"))
                    if (parameter.is_object())
                        parameters[parameter.value("name", "")] = &parameter;
            }

            std::set<std::string> used;
            if (entry.contains("bindings") && entry.at("bindings").is_array())
            {
                for (const nlohmann::json &binding : entry.at("bindings"))
                {
                    if (!binding.is_object() || !binding.contains("binding") ||
                        !binding.at("binding").is_object())
                        continue;
                    const nlohmann::json &slot = binding.at("binding");
                    if (slot.value("kind", "") == "descriptorTableSlot" && slot.value("used", 1) != 0)
                        used.insert(binding.value("name", ""));
                }
            }

            for (const auto &[name, parameter] : parameters)
            {
                const nlohmann::json &binding = parameter->contains("binding") ? parameter->at("binding") :
                                                                                  nlohmann::json{};
                if (!binding.is_object())
                    continue;
                const std::string kind = binding.value("kind", "");
                if (kind == "descriptorTableSlot" && used.contains(name))
                {
                    ShaderDescriptorBinding descriptor;
                    descriptor.set = binding.value("space", uint32_t{ 0 });
                    descriptor.binding = binding.value("index", uint32_t{ 0 });
                    descriptor.count = 1;
                    if (parameter->contains("type") && parameter->at("type").is_object())
                    {
                        const nlohmann::json &type = parameter->at("type");
                        if (type.value("kind", "") == "array")
                            descriptor.count = type.value("elementCount", uint32_t{ 1 });
                        descriptor.type = TypeSignature(type);
                    }
                    signature.descriptors.push_back(std::move(descriptor));
                }
            }
            std::sort(signature.descriptors.begin(), signature.descriptors.end());

            const auto addPush = [&](const nlohmann::json &parameter)
                {
                    if (signature.pushConstants.size() != 0)
                        return;
                    const nlohmann::json &binding = parameter.contains("binding") ? parameter.at("binding") :
                                                                                       nlohmann::json{};
                    if (!binding.is_object() || binding.value("kind", "") != "pushConstantBuffer")
                        return;
                    ShaderPushConstantRange range;
                    range.offset = binding.value("offset", uint32_t{ 0 });
                    if (parameter.contains("type") && parameter.at("type").is_object() &&
                        parameter.at("type").contains("elementVarLayout") &&
                        parameter.at("type").at("elementVarLayout").contains("binding"))
                    {
                        const nlohmann::json &layoutBinding =
                            parameter.at("type").at("elementVarLayout").at("binding");
                        range.offset = layoutBinding.value("offset", range.offset);
                        range.size = layoutBinding.value("size", uint32_t{ 0 });
                    }
                    if (range.size == 0)
                        range.size = static_cast<uint32_t>(LargestSize(parameter));
                    range.stages = signature.stage;
                    range.abi = parameter.contains("type") ? TypeSignature(parameter.at("type")) : "";
                    signature.pushConstants.push_back(std::move(range));
                };
            for (const auto &[name, parameter] : parameters)
                addPush(*parameter);

            if (entry.contains("parameters") && entry.at("parameters").is_array())
            {
                for (const nlohmann::json &parameter : entry.at("parameters"))
                {
                    if (!parameter.is_object())
                        continue;
                    addPush(parameter);
                    CollectVaryings(parameter, signature.inputs, false);
                }
            }
            if (entry.contains("result"))
                CollectVaryings(entry.at("result"), signature.outputs, true);

            const auto sortIo = [](std::vector<ShaderStageIo> &io)
                {
                    std::sort(io.begin(), io.end(), [](const ShaderStageIo &left, const ShaderStageIo &right)
                        {
                            if (left.location != right.location)
                                return left.location < right.location;
                            return left.semantic < right.semantic;
                        });
                };
            sortIo(signature.inputs);
            sortIo(signature.outputs);
            std::sort(signature.pushConstants.begin(), signature.pushConstants.end());
            return signature;
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("shader reflection is malformed: ") + exception.what());
            return std::nullopt;
        }
    }

    ShaderCompatibility CompareShaderReflection(const ShaderReflectionSignature &accepted,
        const ShaderReflectionSignature &candidate)
    {        ShaderCompatibility result;
        const auto compare = [&](const char *kind, const auto &before, const auto &after)
            {
                if (before == after)
                    return;
                result.restartRequired = true;
                result.differences.push_back(std::string(kind) + " changed");
            };

        compare("entry stage", accepted.stage, candidate.stage);
        compare("descriptor layout", accepted.descriptors, candidate.descriptors);
        compare("push constant range", accepted.pushConstants, candidate.pushConstants);
        compare("stage inputs", accepted.inputs, candidate.inputs);
        compare("stage outputs", accepted.outputs, candidate.outputs);
        result.compatible = !result.restartRequired;
        return result;
    }

    namespace
    {
        std::string ScalarCategory(const SpvReflectTypeDescription *type)
        {
            if (type == nullptr)
                return "?";
            if (type->op == SpvOpTypeFloat)
                return "float";
            if (type->op == SpvOpTypeInt)
                return "int";
            if (type->op == SpvOpTypeBool)
                return "bool";
            return "op" + std::to_string(static_cast<uint32_t>(type->op)) + "f" +
                std::to_string(static_cast<uint32_t>(type->type_flags));
        }

        std::string NumericAbi(const SpvReflectNumericTraits &numeric,
            const SpvReflectTypeDescription *type)
        {
            std::string value = ScalarCategory(type) + "s" + std::to_string(numeric.scalar.width) + "/" +
                std::to_string(numeric.scalar.signedness) + "v" + std::to_string(numeric.vector.component_count);
            value += "m" + std::to_string(numeric.matrix.column_count) + "x" +
                std::to_string(numeric.matrix.row_count) + "/" + std::to_string(numeric.matrix.stride);
            return value;
        }

        std::string ArrayAbi(const SpvReflectArrayTraits &array)
        {
            std::string value = "a" + std::to_string(array.dims_count) + "/" +
                std::to_string(array.stride) + "[";
            for (uint32_t i = 0; i < array.dims_count && i < SPV_REFLECT_MAX_ARRAY_DIMS; ++i)
            {
                if (i != 0)
                    value += ",";
                value += std::to_string(array.dims[i]);
            }
            return value + "]";
        }

        std::string BindingArrayAbi(const SpvReflectBindingArrayTraits &array)
        {
            std::string value = "a" + std::to_string(array.dims_count) + "[";
            for (uint32_t i = 0; i < array.dims_count && i < SPV_REFLECT_MAX_ARRAY_DIMS; ++i)
            {
                if (i != 0)
                    value += ",";
                value += std::to_string(array.dims[i]);
            }
            return value + "]";
        }

        std::string BlockAbi(const SpvReflectBlockVariable &block)
        {
            std::string value = "off=" + std::to_string(block.absolute_offset) + ",size=" +
                std::to_string(block.size) + ",pad=" + std::to_string(block.padded_size) + ",row=" +
                ((block.decoration_flags & SPV_REFLECT_DECORATION_ROW_MAJOR) != 0 ? "1" : "0") + "," +
                NumericAbi(block.numeric, block.type_description) + "," + ArrayAbi(block.array) + "{";
            for (uint32_t i = 0; i < block.member_count; ++i)
                value += BlockAbi(block.members[i]) + ";";
            return value + "}";
        }

        std::string InterfaceAbi(const SpvReflectInterfaceVariable &variable)
        {
            std::string value = "loc=" + std::to_string(variable.location) + ",fmt=" +
                std::to_string(static_cast<uint32_t>(variable.format)) + "," +
                NumericAbi(variable.numeric, variable.type_description) + "," + ArrayAbi(variable.array);
            for (uint32_t i = 0; i < variable.member_count; ++i)
            {
                const SpvReflectInterfaceVariable &member = variable.members[i];
                value += ";m" + std::to_string(i) + "=" + std::to_string(member.location) + ":" +
                    std::to_string(static_cast<uint32_t>(member.format)) + ":" +
                    NumericAbi(member.numeric, member.type_description) + ":" + ArrayAbi(member.array);
            }
            return value;
        }

        std::string ExecutionStage(SpvExecutionModel model)
        {
            switch (model)
            {
            case SpvExecutionModelVertex: return "vertex";
            case SpvExecutionModelTessellationControl: return "tesscontrol";
            case SpvExecutionModelTessellationEvaluation: return "tesseval";
            case SpvExecutionModelGeometry: return "geometry";
            case SpvExecutionModelFragment: return "fragment";
            case SpvExecutionModelGLCompute: return "compute";
            case SpvExecutionModelRayGenerationKHR: return "raygeneration";
            case SpvExecutionModelIntersectionKHR: return "intersection";
            case SpvExecutionModelAnyHitKHR: return "anyhit";
            case SpvExecutionModelClosestHitKHR: return "closesthit";
            case SpvExecutionModelMissKHR: return "miss";
            case SpvExecutionModelCallableKHR: return "callable";
            default: return "unknown";
            }
        }
    }

    std::optional<ShaderSpirvAbi> ParseSpirvAbi(std::span<const std::byte> bytes,
        std::string_view entryPoint, std::string *error)
    {
        if (!ValidateSpirvModule(bytes, error))
            return std::nullopt;

        SpvReflectShaderModule module{};
        if (spvReflectCreateShaderModule(bytes.size(), bytes.data(), &module) != SPV_REFLECT_RESULT_SUCCESS)
        {
            SetError(error, "SPIRV-Reflect could not parse the module");
            return std::nullopt;
        }

        ShaderSpirvAbi abi;
        bool found = false;
        for (uint32_t i = 0; i < module.entry_point_count; ++i)
        {
            const SpvReflectEntryPoint &entry = module.entry_points[i];
            if (entryPoint != entry.name)
                continue;
            found = true;
            abi.entryPoint = entry.name;
            abi.stage = ExecutionStage(entry.spirv_execution_model);

            for (uint32_t setIndex = 0; setIndex < entry.descriptor_set_count; ++setIndex)
            {
                const SpvReflectDescriptorSet &set = entry.descriptor_sets[setIndex];
                for (uint32_t bindingIndex = 0; bindingIndex < set.binding_count; ++bindingIndex)
                {
                    const SpvReflectDescriptorBinding &binding = *set.bindings[bindingIndex];
                    std::string value = "set=" + std::to_string(binding.set) + ",binding=" +
                        std::to_string(binding.binding) + ",type=" +
                        std::to_string(static_cast<uint32_t>(binding.descriptor_type)) + ",count=" +
                        std::to_string(binding.count) + ",array=" + BindingArrayAbi(binding.array);
                    if (binding.block.member_count != 0)
                        value += ",block=" + BlockAbi(binding.block);
                    if (binding.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                        binding.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                    {
                        value += ",image=" + std::to_string(static_cast<uint32_t>(binding.image.dim)) + "/" +
                            std::to_string(binding.image.arrayed) + "/" +
                            std::to_string(binding.image.depth) + "/" +
                            std::to_string(binding.image.ms) + "/" +
                            std::to_string(binding.image.sampled) + "/" +
                            std::to_string(static_cast<uint32_t>(binding.image.image_format));
                    }
                    abi.descriptors.push_back(std::move(value));
                }
            }

            for (uint32_t blockIndex = 0; blockIndex < module.push_constant_block_count; ++blockIndex)
                abi.pushConstants.push_back(BlockAbi(module.push_constant_blocks[blockIndex]));

            for (uint32_t inputIndex = 0; inputIndex < entry.input_variable_count; ++inputIndex)
            {
                const SpvReflectInterfaceVariable &variable = *entry.input_variables[inputIndex];
                if (variable.built_in >= 0)
                    continue;
                abi.inputs.push_back(InterfaceAbi(variable));
            }
            for (uint32_t outputIndex = 0; outputIndex < entry.output_variable_count; ++outputIndex)
            {
                const SpvReflectInterfaceVariable &variable = *entry.output_variables[outputIndex];
                if (variable.built_in >= 0)
                    continue;
                abi.outputs.push_back(InterfaceAbi(variable));
            }
            break;
        }

        spvReflectDestroyShaderModule(&module);
        if (!found)
        {
            SetError(error, "SPIRV-Reflect has no entry point '" + std::string(entryPoint) + "'");
            return std::nullopt;
        }

        std::sort(abi.descriptors.begin(), abi.descriptors.end());
        std::sort(abi.inputs.begin(), abi.inputs.end());
        std::sort(abi.outputs.begin(), abi.outputs.end());
        std::sort(abi.pushConstants.begin(), abi.pushConstants.end());
        return abi;
    }

    ShaderCompatibility CompareSpirvAbi(const ShaderSpirvAbi &accepted, const ShaderSpirvAbi &candidate)
    {
        ShaderCompatibility result;
        const auto compare = [&](const char *kind, const auto &before, const auto &after)
            {
                if (before == after)
                    return;
                result.restartRequired = true;
                result.differences.push_back(std::string(kind) + " changed");
            };
        compare("entry stage", accepted.stage, candidate.stage);
        compare("descriptor ABI", accepted.descriptors, candidate.descriptors);
        compare("push constant ABI", accepted.pushConstants, candidate.pushConstants);
        compare("stage inputs", accepted.inputs, candidate.inputs);
        compare("stage outputs", accepted.outputs, candidate.outputs);
        result.compatible = !result.restartRequired;
        return result;
    }

    std::optional<ShaderSourceFingerprint> FingerprintShaderSource(const std::filesystem::path &path)
    {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error)
            return std::nullopt;
        const auto writeTime = std::filesystem::last_write_time(path, error);
        if (error)
            return std::nullopt;
        const auto bytes = ReadBytes(path);
        if (!bytes) return std::nullopt;
        return ShaderSourceFingerprint{ static_cast<uint64_t>(size),
            static_cast<uint64_t>(writeTime.time_since_epoch().count()), true, HashShaderContent(*bytes) };
    }

    void ShaderDependencyIndex::Clear()
    {
        mSources.clear();
        mOutputs.clear();
        mEdges.clear();
        mRevision = 0;
    }

    void ShaderDependencyIndex::AddDependency(std::string source, std::string output)
    {
        for (const Edge &edge : mEdges)
            if (edge.source == source && edge.output == output)
                return;
        mEdges.push_back(Edge{ std::move(source), std::move(output) });
    }

    void ShaderDependencyIndex::AddOutput(std::string output)
    {
        if (std::find(mOutputs.begin(), mOutputs.end(), output) == mOutputs.end())
            mOutputs.push_back(std::move(output));
    }

    bool ShaderDependencyIndex::RebuildFromManifest(const ShaderManifest &manifest, std::string *error)
    {
        Clear();
        for (const ShaderManifestOutput &output : manifest.outputs)
        {
            AddOutput(output.output);
            for (const std::string &source : output.sources)
                AddDependency(source, output.output);

            std::optional<std::string> depfileText;
            if (!output.depfile.empty())
            {
                std::filesystem::path depfilePath = std::filesystem::u8path(output.depfile);
                if (depfilePath.is_relative())
                    depfilePath = manifest.workingDirectory / depfilePath;
                depfileText = ReadText(depfilePath);
            }
            if (!depfileText)
                continue;

            const auto dependencies = ParseShaderDepfile(*depfileText, nullptr);
            if (!dependencies)
                continue;
            for (const std::string &dependency : *dependencies)
            {
                std::filesystem::path resolved = std::filesystem::u8path(dependency);
                if (resolved.is_relative())
                    resolved = manifest.workingDirectory / resolved;
                const auto bytes = resolved.lexically_normal().generic_u8string();
                AddDependency(std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()), output.output);
            }
        }
        if (mEdges.empty())
        {
            SetError(error, "shader dependency graph is empty");
            return false;
        }
        return true;
    }

    bool ShaderDependencyIndex::Observe(std::string_view source, const ShaderSourceFingerprint &fingerprint)
    {
        for (SourceState &state : mSources)
        {
            if (state.path != source)
                continue;
            if (state.fingerprint == fingerprint)
                return false;
            state.fingerprint = fingerprint;
            ++mRevision;
            return true;
        }
        mSources.push_back(SourceState{ std::string(source), fingerprint });
        ++mRevision;
        return true;
    }

    std::vector<std::string> ShaderDependencyIndex::Poll()
    {
        std::set<std::string> paths;
        for (const Edge &edge : mEdges)
            paths.insert(edge.source);

        std::vector<std::string> changed;
        for (const std::string &path : paths)
        {
            std::optional<ShaderSourceFingerprint> fingerprint = FingerprintShaderSource(std::filesystem::u8path(path));
            if (!fingerprint)
                fingerprint = ShaderSourceFingerprint{};
            if (Observe(path, *fingerprint))
                changed.push_back(path);
        }
        return changed;
    }

    std::vector<std::string> ShaderDependencyIndex::AffectedOutputs(
        std::span<const std::string> changedSources) const
    {
        std::set<std::string> changed(changedSources.begin(), changedSources.end());
        std::vector<std::string> result;
        for (const std::string &output : mOutputs)
        {
            bool affected = false;
            for (const Edge &edge : mEdges)
            {
                if (edge.output == output && changed.contains(edge.source))
                {
                    affected = true;
                    break;
                }
            }
            if (affected)
                result.push_back(output);
        }
        return result;
    }

    std::vector<std::string> ShaderDependencyIndex::DependenciesOf(std::string_view output) const
    {
        std::vector<std::string> result;
        for (const Edge &edge : mEdges)
            if (edge.output == output && std::find(result.begin(), result.end(), edge.source) == result.end())
                result.push_back(edge.source);
        return result;
    }

    std::vector<std::string> ShaderDependencyIndex::Sources() const
    {
        std::vector<std::string> result;
        for (const Edge &edge : mEdges)
            if (std::find(result.begin(), result.end(), edge.source) == result.end())
                result.push_back(edge.source);
        return result;
    }

    const ShaderSourceFingerprint *ShaderDependencyIndex::FingerprintOf(std::string_view source) const
    {
        for (const SourceState &state : mSources)
            if (state.path == source)
                return &state.fingerprint;
        return nullptr;
    }

    MaterialMigration MigrateMaterialRecord(const MaterialLayout &acceptedLayout,
        const MaterialLayout &candidateLayout, std::span<const std::byte> acceptedBytes,
        std::span<const std::byte> defaultBytes)
    {
        MaterialMigration result;
        if (candidateLayout.size == 0 || candidateLayout.size > (1u << 20))
        {
            result.error = "candidate material layout has an invalid size";
            return result;
        }
        if (defaultBytes.size() < candidateLayout.size)
        {
            result.error = "candidate material defaults do not cover the layout";
            return result;
        }
        if (!acceptedBytes.empty() && acceptedBytes.size() < acceptedLayout.size)
        {
            result.error = "accepted material record does not cover its layout";
            return result;
        }

        const bool hasAccepted = !acceptedBytes.empty() && acceptedLayout.size != 0;
        result.bytes.assign(candidateLayout.size, std::byte{ 0 });

        for (const MaterialFieldLayout &candidate : candidateLayout.fields)
        {
            if (candidate.offset > candidateLayout.size || candidate.size > candidateLayout.size - candidate.offset)
            {
                result.error = "candidate material field '" + candidate.name + "' does not fit the record";
                result.bytes.clear();
                return result;
            }

            const MaterialFieldLayout *accepted = nullptr;
            if (hasAccepted)
            {
                for (const MaterialFieldLayout &field : acceptedLayout.fields)
                {
                    if (field.name != candidate.name)
                        continue;
                    accepted = &field;
                    break;
                }
            }

            if (accepted != nullptr)
            {
                const bool textureRef = accepted->kind == MaterialFieldKind::TextureRef &&
                    candidate.kind == MaterialFieldKind::TextureRef;
                if (accepted->kind != candidate.kind || (!textureRef && accepted->size != candidate.size))
                {
                    result.error = "material field '" + candidate.name + "' changed type";
                    result.bytes.clear();
                    return result;
                }
                if (accepted->offset > acceptedBytes.size() || accepted->size > acceptedBytes.size() - accepted->offset)
                {
                    result.error = "accepted material field '" + candidate.name + "' does not fit its record";
                    result.bytes.clear();
                    return result;
                }
                if (textureRef)
                {
                    const uint32_t imageSize = std::min<uint32_t>(accepted->size, 4u);
                    std::memcpy(result.bytes.data() + candidate.offset, acceptedBytes.data() + accepted->offset,
                        imageSize);
                    if (candidate.size == 8 && accepted->size == 8)
                        std::memcpy(result.bytes.data() + candidate.offset + 4,
                            acceptedBytes.data() + accepted->offset + 4, 4);
                }
                else
                {
                    std::memcpy(result.bytes.data() + candidate.offset, acceptedBytes.data() + accepted->offset,
                        candidate.size);
                }
                if (candidate.offset != accepted->offset || candidate.size != accepted->size)
                    result.recordChanged = true;
                continue;
            }

            std::memcpy(result.bytes.data() + candidate.offset, defaultBytes.data() + candidate.offset,
                candidate.size);
            result.defaultedFields.push_back(candidate.name);
            result.recordChanged = true;
        }

        if (hasAccepted)
        {
            for (const MaterialFieldLayout &accepted : acceptedLayout.fields)
            {
                const bool present = std::any_of(candidateLayout.fields.begin(), candidateLayout.fields.end(),
                    [&](const MaterialFieldLayout &candidate) { return candidate.name == accepted.name; });
                if (!present)
                    result.recordChanged = true;
            }
        }

        result.ok = true;
        return result;
    }

    StagedShaderPublisher::StagedShaderPublisher(std::filesystem::path directory, uint64_t revision,
        uint64_t sourceFingerprint)
        : mDirectory(std::move(directory)), mRevision(revision), mSourceFingerprint(sourceFingerprint)
    {
    }

    bool StagedShaderPublisher::Stage(std::string_view name, std::span<const std::byte> bytes, std::string *error)
    {
        if (name.empty() || std::filesystem::path(name).has_parent_path())
        {
            SetError(error, "staged shader name must be a bare file name");
            return false;
        }
        std::error_code status;
        std::filesystem::create_directories(mDirectory, status);
        if (status)
        {
            SetError(error, "could not create shader directory: " + status.message());
            return false;
        }

        const std::filesystem::path target = mDirectory / std::string(name);
        const std::filesystem::path temporary =
            mDirectory / (std::string(name) + ".tmp." + std::to_string(mRevision));
        if (!WriteBytes(temporary, bytes))
        {
            SetError(error, "could not stage shader output '" + std::string(name) + "'");
            return false;
        }
        for (StagedFile &staged : mFiles)
        {
            if (staged.name == name)
            {
                staged.temporary = temporary;
                return true;
            }
        }
        mFiles.push_back(StagedFile{ std::string(name), temporary, target, target.string() + ".bak." +
            std::to_string(mRevision) });
        mStaged.push_back(std::string(name));
        return true;
    }

    bool StagedShaderPublisher::StageText(std::string_view name, std::string_view text, std::string *error)
    {
        return Stage(name, std::as_bytes(std::span<const char>(text.data(), text.size())), error);
    }

    std::optional<ShaderOutputStamp> StagedShaderPublisher::ReadStamp(const std::filesystem::path &target)
    {
        const std::optional<std::string> text = ReadText(target.string() + ".stamp");
        if (!text)
            return std::nullopt;
        ShaderOutputStamp stamp;
        try
        {
            std::size_t consumed = 0;
            stamp.revision = std::stoull(*text, &consumed);
            const std::string rest = text->substr(consumed);
            stamp.sourceFingerprint = std::stoull(rest);
            stamp.valid = true;
            return stamp;
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }

    bool StagedShaderPublisher::Commit(std::string *error)
    {
        if (mFiles.empty())
            return true;

        for (const StagedFile &staged : mFiles)
        {
            const auto stamp = ReadStamp(staged.target);
            if (stamp && stamp->valid && stamp->revision > mRevision)
            {
                SetError(error, "refusing to replace a newer accepted shader output '" + staged.name + "'");
                return false;
            }
        }

        for (StagedFile &staged : mFiles)
        {
            std::error_code status;
            if (std::filesystem::exists(staged.target, status))
            {
                std::filesystem::copy_file(staged.target, staged.backup,
                    std::filesystem::copy_options::overwrite_existing, status);
                if (status)
                {
                    Rollback();
                    SetError(error, "could not back up shader output '" + staged.name + "': " + status.message());
                    return false;
                }
                staged.mHadOriginal = true;
                const std::optional<std::string> oldStamp = ReadText(staged.target.string() + ".stamp");
                if (oldStamp)
                {
                    staged.mOldStamp = *oldStamp;
                    staged.mHadStamp = true;
                }
            }

            // Windows cannot rename over an existing target, so the backup is
            // the only copy while the replacement is moved into place.
            std::filesystem::remove(staged.target, status);
            std::filesystem::rename(staged.temporary, staged.target, status);
            if (status)
            {
                std::filesystem::remove(staged.target, status);
                if (staged.mHadOriginal)
                {
                    status.clear();
                    std::filesystem::copy_file(staged.backup, staged.target,
                        std::filesystem::copy_options::overwrite_existing, status);
                }
                Rollback();
                SetError(error, "could not publish shader output '" + staged.name + "': " + status.message());
                return false;
            }
            staged.mPublished = true;

            const std::string stamp = std::to_string(mRevision) + " " + std::to_string(mSourceFingerprint);
            if (!WriteTextAtomic(staged.target.string() + ".stamp", stamp))
            {
                Rollback();
                SetError(error, "could not stamp shader output '" + staged.name + "'");
                return false;
            }
        }
        return true;
    }

    void StagedShaderPublisher::Finalize()
    {
        for (const StagedFile &staged : mFiles)
        {
            std::error_code ignored;
            std::filesystem::remove(staged.backup, ignored);
        }
        mFiles.clear();
        mStaged.clear();
    }

    void StagedShaderPublisher::Rollback()
    {
        for (StagedFile &staged : mFiles)
        {
            std::error_code ignored;
            if (!staged.mPublished)
            {
                std::filesystem::remove(staged.temporary, ignored);
                continue;
            }
            if (staged.mHadOriginal)
                std::filesystem::copy_file(staged.backup, staged.target,
                    std::filesystem::copy_options::overwrite_existing, ignored);
            else
                std::filesystem::remove(staged.target, ignored);

            const std::string stampPath = staged.target.string() + ".stamp";
            if (staged.mHadStamp)
                WriteTextAtomic(stampPath, staged.mOldStamp);
            else
                std::filesystem::remove(stampPath, ignored);
            std::filesystem::remove(staged.backup, ignored);
        }
        mFiles.clear();
        mStaged.clear();
    }

    void StagedShaderPublisher::Discard()
    {
        for (const StagedFile &staged : mFiles)
        {
            std::error_code ignored;
            std::filesystem::remove(staged.temporary, ignored);
        }
        mFiles.clear();
        mStaged.clear();
    }
}

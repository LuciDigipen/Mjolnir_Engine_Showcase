/**
 * @file MaterialCooker.cpp
 * @author Jackey Cen
 * @brief .material.json parsing and the packed surface texture cook.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "MaterialCooker.h"
#include "ImageImporter.h"
#include <assets/AssetPath.h>
#include <core/fs/FileIO.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace mir::cook
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr int64_t kMinResolution = 1;
        constexpr int64_t kMaxResolution = 16384;
        constexpr std::string_view kLfsPrefix = "version https://git-lfs.github.com/spec/v1";

        float ToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
        float ToSrgb(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }
        uint8_t Byte(float v) { return static_cast<uint8_t>(std::clamp<long>(std::lround(v * 255.0f), 0, 255)); }

        bool IsLfsPointer(const std::filesystem::path &path)
        {
            std::ifstream input(path, std::ios::binary);
            std::string head(96, '\0');
            input.read(head.data(), static_cast<std::streamsize>(head.size()));
            head.resize(static_cast<size_t>(input.gcount()));
            return head.starts_with(kLfsPrefix);
        }
        void RequireSourceFile(const std::filesystem::path &path, const std::string &reference)
        {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error))
                throw std::runtime_error("missing source file '" + reference + "' (" + path.string() + ")");
            if (IsLfsPointer(path))
                throw std::runtime_error("source file '" + reference +
                                         "' is a Git LFS pointer, not its content; run git lfs pull");
        }
        std::filesystem::path ResolveReference(const std::string &reference, const std::string &kind,
                                               const std::filesystem::path &ownerRoot,
                                               const std::vector<std::filesystem::path> &approvedRoots)
        {
            const AssetPathResult normalized = NormalizeAssetPath(reference);
            if (!normalized.Ok())
                throw std::runtime_error(kind + " '" + reference + "': " + std::string(Describe(normalized.error)));
            std::vector<std::filesystem::path> roots{ownerRoot};
            for (const auto &root : approvedRoots)
            {
                const auto canonical = std::filesystem::weakly_canonical(root);
                if (std::none_of(roots.begin(), roots.end(),
                                 [&](const auto &known) { return std::filesystem::weakly_canonical(known) == canonical; }))
                    roots.push_back(root);
            }
            for (const auto &root : roots)
            {
                const auto candidate = root / std::filesystem::path(normalized.path);
                std::error_code error;
                if (!std::filesystem::exists(candidate, error))
                    continue;
                const auto rootCanonical = std::filesystem::weakly_canonical(root);
                const auto fileCanonical = std::filesystem::weakly_canonical(candidate);
                const auto relative = fileCanonical.lexically_relative(rootCanonical);
                if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                    throw std::runtime_error(kind + " '" + reference + "' escapes its asset root");
                RequireSourceFile(fileCanonical, reference);
                return fileCanonical;
            }
            throw std::runtime_error("missing " + kind + " '" + reference + "' under " + ownerRoot.string());
        }
        void RejectUnknown(const Json &object, std::initializer_list<std::string_view> allowed,
                           const std::string &what)
        {
            if (!object.is_object())
                throw std::runtime_error(what + " must be an object");
            for (const auto &[key, value] : object.items())
                if (std::none_of(allowed.begin(), allowed.end(), [&](auto known) { return key == known; }))
                    throw std::runtime_error(what + " has unknown key '" + key + "'");
        }
        TextureColorSpace ParseColorSpace(const Json &value, const std::string &role)
        {
            if (!value.is_string())
                throw std::runtime_error("texture '" + role + "' colorSpace must be a string");
            std::string text = value.get<std::string>();
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (text == "srgb")
                return TextureColorSpace::Srgb;
            if (text == "linear")
                return TextureColorSpace::Linear;
            throw std::runtime_error("texture '" + role + "' colorSpace must be 'srgb' or 'linear'");
        }
        struct ParsedMaterial
        {
            SurfaceMaterialAsset asset;
            std::vector<std::filesystem::path> files, images;
        };
        ParsedMaterial Parse(const std::filesystem::path &path, const std::string &assetPath,
                             const std::filesystem::path &ownerRoot,
                             const std::vector<std::filesystem::path> &approvedRoots)
        {
            const auto bytes = ReadBytes(path);
            if (!bytes)
                throw std::runtime_error("cannot read " + path.string());
            RequireSourceFile(path, assetPath);
            Json document;
            try
            {
                document = Json::parse(std::string_view(reinterpret_cast<const char *>(bytes->data()), bytes->size()));
            }
            catch (const Json::exception &error)
            {
                throw std::runtime_error(assetPath + ": invalid JSON: " + error.what());
            }
            if (!document.is_object())
                throw std::runtime_error(assetPath + ": the material root must be an object");
            const auto version = document.find("version");
            if (version == document.end() || !version->is_number_integer() || version->get<int64_t>() != 1)
                throw std::runtime_error(assetPath + ": unsupported material version (expected 1)");

            ParsedMaterial parsed;
            parsed.asset.version = 1;
            const auto shader = document.find("shader");
            if (shader == document.end())
                throw std::runtime_error(assetPath + ": missing shader");
            RejectUnknown(*shader, {"file", "type"}, "shader");
            if (!shader->contains("file") || !shader->at("file").is_string() ||
                shader->at("file").get<std::string>().empty() || !shader->contains("type") ||
                !shader->at("type").is_string() || shader->at("type").get<std::string>().empty())
                throw std::runtime_error(assetPath + ": shader needs non-empty file and type strings");
            parsed.asset.shaderFile = shader->at("file").get<std::string>();
            parsed.asset.shaderType = shader->at("type").get<std::string>();
            const auto shaderPath = ResolveReference(parsed.asset.shaderFile, "shader", ownerRoot, approvedRoots);
            parsed.files.push_back(shaderPath);

            const auto frontends = document.find("frontends");
            if (frontends == document.end() || !frontends->is_array() || frontends->empty())
                throw std::runtime_error(assetPath + ": frontends must be a non-empty array");
            for (const auto &frontend : *frontends)
            {
                if (!frontend.is_string())
                    throw std::runtime_error(assetPath + ": frontends must contain strings");
                const std::string value = frontend.get<std::string>();
                if (value != "mesh" && value != "terrain")
                    throw std::runtime_error(assetPath + ": unknown frontend '" + value + "'");
                parsed.asset.frontends.push_back(value);
            }

            const auto parameters = document.find("parameters");
            parsed.asset.parametersJson = parameters == document.end() ? "{}" : parameters->dump();
            if (parameters != document.end() && !parameters->is_object())
                throw std::runtime_error(assetPath + ": parameters must be an object");

            Json unknown = Json::object();
            for (const auto &[key, value] : document.items())
                if (key != "version" && key != "shader" && key != "frontends" && key != "parameters" &&
                    key != "textures" && key != "resolution")
                    unknown[key] = value;
            parsed.asset.unknownJson = unknown.dump();

            const auto resolution = document.find("resolution");
            if (resolution != document.end())
            {
                if (!resolution->is_number_integer())
                    throw std::runtime_error(assetPath + ": resolution must be a positive integer");
                const int64_t value = resolution->get<int64_t>();
                if (value < kMinResolution || value > kMaxResolution)
                    throw std::runtime_error(assetPath + ": resolution is outside the supported range");
                parsed.asset.resolution = static_cast<uint32_t>(value);
            }

            const auto textures = document.find("textures");
            if (textures != document.end())
            {
                if (!textures->is_object())
                    throw std::runtime_error(assetPath + ": textures must be an object");
                static const std::set<std::string> roles{"baseColor",  "normal",     "displacement",
                                                         "roughness", "specular",   "ao"};
                for (const auto &[role, value] : textures->items())
                {
                    if (!roles.contains(role))
                        throw std::runtime_error(assetPath + ": unknown texture role '" + role + "'");
                    RejectUnknown(value, {"path", "colorSpace"}, "texture '" + role + "'");
                    if (!value.contains("path") || !value.at("path").is_string() ||
                        value.at("path").get<std::string>().empty())
                        throw std::runtime_error(assetPath + ": texture '" + role + "' needs a path");
                    if (!value.contains("colorSpace"))
                        throw std::runtime_error(assetPath + ": texture '" + role + "' needs a colorSpace");
                    SurfaceMaterialTexture texture;
                    texture.role = role;
                    texture.path = value.at("path").get<std::string>();
                    texture.colorSpace = ParseColorSpace(value.at("colorSpace"), role);
                    const auto image = ResolveReference(texture.path, "texture", ownerRoot, approvedRoots);
                    parsed.images.push_back(image);
                    parsed.asset.textures.push_back(std::move(texture));
                }
            }
            parsed.files.push_back(std::filesystem::weakly_canonical(path));
            parsed.files.insert(parsed.files.end(), parsed.images.begin(), parsed.images.end());
            return parsed;
        }

        struct Source
        {
            TextureAsset texture;
            bool present = false;
        };
        Source LoadSource(const ParsedMaterial &material, const std::string &role,
                          const std::filesystem::path &ownerRoot,
                          const std::vector<std::filesystem::path> &approvedRoots)
        {
            Source source;
            const auto found = std::find_if(material.asset.textures.begin(), material.asset.textures.end(),
                                            [&](const auto &texture) { return texture.role == role; });
            if (found == material.asset.textures.end())
                return source;
            const auto path = ResolveReference(found->path, "texture", ownerRoot, approvedRoots);
            const auto bytes = ReadBytes(path);
            if (!bytes)
                throw std::runtime_error("cannot read " + path.string());
            source.texture = DecodeImage(*bytes, found->colorSpace, false);
            source.present = true;
            return source;
        }
        float Sample(const Source &source, uint32_t x, uint32_t y, uint32_t channel, bool linearize)
        {
            if (!source.present)
                return 0;
            const float value = source.texture.rgba[(static_cast<size_t>(y) * source.texture.width + x) * 4 + channel] / 255.0f;
            return linearize && source.texture.colorSpace == TextureColorSpace::Srgb ? ToLinear(value) : value;
        }
        TextureAsset MakeOutput(uint32_t resolution, TextureFormat format, TextureColorSpace colorSpace)
        {
            TextureAsset texture;
            texture.width = texture.height = resolution;
            texture.format = format;
            texture.colorSpace = colorSpace;
            texture.rgba.assign(static_cast<size_t>(resolution) * resolution * 4, 0);
            return texture;
        }
        void EachTexel(const Source &source, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                       auto &&sample)
        {
            if (!source.present)
                return;
            const uint32_t x0 = x * source.texture.width / width;
            const uint32_t y0 = y * source.texture.height / height;
            const uint32_t x1 = std::max((x + 1) * source.texture.width / width, x0 + 1);
            const uint32_t y1 = std::max((y + 1) * source.texture.height / height, y0 + 1);
            for (uint32_t sy = y0; sy < y1; ++sy)
                for (uint32_t sx = x0; sx < x1; ++sx)
                    sample(sx, sy);
        }
        TextureAsset PackAlbedo(const ParsedMaterial &material, const std::filesystem::path &ownerRoot,
                                const std::vector<std::filesystem::path> &approvedRoots)
        {
            const Source base = LoadSource(material, "baseColor", ownerRoot, approvedRoots);
            const Source ao = LoadSource(material, "ao", ownerRoot, approvedRoots);
            TextureAsset out = MakeOutput(material.asset.resolution, TextureFormat::Rgba8Srgb,
                                          TextureColorSpace::Srgb);
            for (uint32_t y = 0; y < out.height; ++y)
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    glm::vec3 rgb(1);
                    float alpha = 1;
                    if (base.present)
                    {
                        glm::vec3 sum(0);
                        uint32_t samples = 0;
                        EachTexel(base, out.width, out.height, x, y, [&](uint32_t sx, uint32_t sy) {
                            sum += glm::vec3(Sample(base, sx, sy, 0, true), Sample(base, sx, sy, 1, true),
                                             Sample(base, sx, sy, 2, true));
                            ++samples;
                        });
                        rgb = sum / static_cast<float>(std::max(samples, 1u));
                    }
                    if (ao.present)
                    {
                        float sum = 0;
                        uint32_t samples = 0;
                        // AO is linear data even when the source file is tagged sRGB.
                        EachTexel(ao, out.width, out.height, x, y, [&](uint32_t sx, uint32_t sy) {
                            sum += Sample(ao, sx, sy, 0, false);
                            ++samples;
                        });
                        alpha = sum / static_cast<float>(std::max(samples, 1u));
                    }
                    uint8_t *pixel = out.rgba.data() + (static_cast<size_t>(y) * out.width + x) * 4;
                    pixel[0] = Byte(ToSrgb(rgb.r));
                    pixel[1] = Byte(ToSrgb(rgb.g));
                    pixel[2] = Byte(ToSrgb(rgb.b));
                    pixel[3] = Byte(alpha);
                }
            BuildTextureMipChain(out);
            return out;
        }
        glm::vec3 DecodeNormal(const uint8_t *pixel)
        {
            return glm::vec3(pixel[0] / 127.5f - 1.0f, pixel[1] / 127.5f - 1.0f, pixel[2] / 127.5f - 1.0f);
        }
        glm::vec3 Normalized(glm::vec3 value)
        {
            const float length = glm::length(value);
            return length > 1e-5f ? value / length : glm::vec3(0, 0, 1);
        }
        void EncodeNormal(glm::vec3 normal, uint8_t *pixel)
        {
            normal = Normalized(normal);
            pixel[0] = Byte(normal.x * 0.5f + 0.5f);
            pixel[1] = Byte(normal.y * 0.5f + 0.5f);
            pixel[2] = Byte(normal.z * 0.5f + 0.5f);
            pixel[3] = 255;
        }
        void BuildNormalMipChain(TextureAsset &texture)
        {
            for (size_t i = 0; i < texture.rgba.size(); i += 4)
            {
                const glm::vec3 normal = DecodeNormal(texture.rgba.data() + i);
                EncodeNormal(normal, texture.rgba.data() + i);
            }
            std::vector<uint8_t> level = texture.rgba;
            uint32_t width = texture.width, height = texture.height;
            while (width > 1 || height > 1)
            {
                const uint32_t nextWidth = std::max(width / 2, 1u);
                const uint32_t nextHeight = std::max(height / 2, 1u);
                std::vector<uint8_t> next(static_cast<size_t>(nextWidth) * nextHeight * 4);
                for (uint32_t y = 0; y < nextHeight; ++y)
                    for (uint32_t x = 0; x < nextWidth; ++x)
                    {
                        glm::vec3 sum(0);
                        uint32_t samples = 0;
                        for (uint32_t sy = y * height / nextHeight; sy < (y + 1) * height / nextHeight; ++sy)
                            for (uint32_t sx = x * width / nextWidth; sx < (x + 1) * width / nextWidth; ++sx)
                            {
                                sum += DecodeNormal(level.data() + (static_cast<size_t>(sy) * width + sx) * 4);
                                ++samples;
                            }
                        EncodeNormal(sum / static_cast<float>(std::max(samples, 1u)),
                                     next.data() + (static_cast<size_t>(y) * nextWidth + x) * 4);
                    }
                TextureMip mip;
                mip.width = nextWidth;
                mip.height = nextHeight;
                mip.offset = static_cast<uint32_t>(texture.mipData.size());
                mip.length = static_cast<uint32_t>(next.size());
                texture.mips.push_back(mip);
                texture.mipData.insert(texture.mipData.end(), next.begin(), next.end());
                level = std::move(next);
                width = nextWidth;
                height = nextHeight;
            }
        }
        TextureAsset PackNormal(const ParsedMaterial &material, const std::filesystem::path &ownerRoot,
                                const std::vector<std::filesystem::path> &approvedRoots)
        {
            const Source source = LoadSource(material, "normal", ownerRoot, approvedRoots);
            TextureAsset out = MakeOutput(material.asset.resolution, TextureFormat::Rgba8Unorm,
                                          TextureColorSpace::Linear);
            for (uint32_t y = 0; y < out.height; ++y)
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    glm::vec3 sum(0);
                    if (source.present)
                    {
                        uint32_t samples = 0;
                        EachTexel(source, out.width, out.height, x, y, [&](uint32_t sx, uint32_t sy) {
                            const uint8_t *pixel =
                                source.texture.rgba.data() + (static_cast<size_t>(sy) * source.texture.width + sx) * 4;
                            sum += DecodeNormal(pixel);
                            ++samples;
                        });
                        sum /= static_cast<float>(std::max(samples, 1u));
                    }
                    else
                        sum = glm::vec3(0, 0, 1);
                    EncodeNormal(sum, out.rgba.data() + (static_cast<size_t>(y) * out.width + x) * 4);
                }
            BuildNormalMipChain(out);
            return out;
        }
        TextureAsset PackSurface(const ParsedMaterial &material, const std::filesystem::path &ownerRoot,
                                 const std::vector<std::filesystem::path> &approvedRoots)
        {
            const Source roughness = LoadSource(material, "roughness", ownerRoot, approvedRoots);
            const Source displacement = LoadSource(material, "displacement", ownerRoot, approvedRoots);
            const Source specular = LoadSource(material, "specular", ownerRoot, approvedRoots);
            TextureAsset out = MakeOutput(material.asset.resolution, TextureFormat::Rgba8Unorm,
                                          TextureColorSpace::Linear);
            for (uint32_t y = 0; y < out.height; ++y)
                for (uint32_t x = 0; x < out.width; ++x)
                {
                    auto channel = [&](const Source &source, float fallback) {
                        if (!source.present)
                            return fallback;
                        float sum = 0;
                        uint32_t samples = 0;
                        EachTexel(source, out.width, out.height, x, y, [&](uint32_t sx, uint32_t sy) {
                            sum += Sample(source, sx, sy, 0, true);
                            ++samples;
                        });
                        return sum / static_cast<float>(std::max(samples, 1u));
                    };
                    uint8_t *pixel = out.rgba.data() + (static_cast<size_t>(y) * out.width + x) * 4;
                    pixel[0] = Byte(channel(roughness, 1.0f));
                    pixel[1] = Byte(channel(displacement, 0.5f));
                    pixel[2] = Byte(channel(specular, 0.04f));
                    pixel[3] = 255;
                }
            BuildTextureMipChain(out);
            return out;
        }
        AssetId DerivedId(const std::string &assetPath, std::string_view role)
        {
            return MakeAssetId(assetPath + "#" + std::string(role));
        }
        void Emit(MaterialImport &result, AssetId id, const auto &asset, std::string kind, std::string name)
        {
            result.blobs.push_back({id, BuildTypedAsset(asset)});
            result.contents.push_back({id, std::move(kind), std::move(name), 0});
        }
    } // namespace

    MaterialDependencies InspectMaterialDependencies(const std::filesystem::path &path, const std::string &assetPath,
                                                     const std::filesystem::path &ownerRoot,
                                                     const std::vector<std::filesystem::path> &approvedRoots)
    {
        ParsedMaterial parsed = Parse(path, assetPath, ownerRoot, approvedRoots);
        return {std::move(parsed.files), std::move(parsed.images)};
    }

    MaterialImport ImportMaterial(const std::filesystem::path &path, const std::string &assetPath,
                                  const std::filesystem::path &ownerRoot,
                                  const std::vector<std::filesystem::path> &approvedRoots)
    {
        ParsedMaterial parsed = Parse(path, assetPath, ownerRoot, approvedRoots);
        MaterialImport result;
        result.materialId = MakeAssetId(assetPath);
        result.dependencies = parsed.files;
        result.sourceImages = parsed.images;

        if (!parsed.asset.textures.empty())
        {
            const TextureAsset albedo = PackAlbedo(parsed, ownerRoot, approvedRoots);
            const TextureAsset normal = PackNormal(parsed, ownerRoot, approvedRoots);
            const TextureAsset surface = PackSurface(parsed, ownerRoot, approvedRoots);
            parsed.asset.albedoTexture = DerivedId(assetPath, "albedoTexture");
            parsed.asset.normalTexture = DerivedId(assetPath, "normalTexture");
            parsed.asset.surfaceTexture = DerivedId(assetPath, "surfaceTexture");
            Emit(result, parsed.asset.albedoTexture, albedo, "texture", "albedo");
            Emit(result, parsed.asset.normalTexture, normal, "texture", "normal");
            Emit(result, parsed.asset.surfaceTexture, surface, "texture", "surface");
            result.textureSizes = {{"albedoTexture", albedo.width, albedo.height},
                                   {"normalTexture", normal.width, normal.height},
                                   {"surfaceTexture", surface.width, surface.height}};
        }
        Emit(result, result.materialId, parsed.asset, "material", path.filename().string());
        return result;
    }
} // namespace mir::cook

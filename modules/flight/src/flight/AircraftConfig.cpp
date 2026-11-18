/**
 * @file AircraftConfig.cpp
 * @author Jackey Cen
 * @brief Parses and decodes the *.aircraft.json configuration asset.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfig.h>

#include <assets/AssetBlob.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <format>
#include <string>
#include <utility>

namespace mir
{
    namespace
    {
        using Json = nlohmann::ordered_json;

        bool IsKnownKey(std::string_view key)
        {
            return key == "version" || key == "mass" || key == "inertia" || key == "centerOfMass"
                || key == "reference" || key == "camera" || key == "nodes"
                // owned by later issues, accepted and ignored for now
                || key == "throttleRate" || key == "actuators" || key == "engines" || key == "gear"
                || key == "stallWarningMarginDeg" || key == "aero";
        }

        bool ReadNumber(const Json &value, const std::string &path, float &out, std::string &error)
        {
            if (!value.is_number())
            {
                error = std::format("{}: expected a number", path);
                return false;
            }

            const double number = value.get<double>();
            if (!std::isfinite(number) || !std::isfinite(static_cast<float>(number)))
            {
                error = std::format("{}: must be finite", path);
                return false;
            }

            out = static_cast<float>(number);
            return true;
        }

        bool ReadPositiveNumber(const Json &value, const std::string &path, float &out, std::string &error)
        {
            if (!ReadNumber(value, path, out, error))
                return false;
            if (out <= 0.0f)
            {
                error = std::format("{}: must be > 0", path);
                return false;
            }
            return true;
        }

        bool ReadNonNegativeNumber(const Json &value, const std::string &path, float &out, std::string &error)
        {
            if (!ReadNumber(value, path, out, error))
                return false;
            if (out < 0.0f)
            {
                error = std::format("{}: must be >= 0", path);
                return false;
            }
            return true;
        }

        bool ReadVector3(const Json &value, const std::string &path, glm::vec3 &out, std::string &error)
        {
            if (!value.is_array() || value.size() != 3)
            {
                error = std::format("{}: expected 3 numbers", path);
                return false;
            }

            glm::vec3 components{0.0f};
            for (std::size_t i = 0; i < 3; ++i)
            {
                if (!ReadNumber(value[i], std::format("{}[{}]", path, i), components[i], error))
                    return false;
            }

            out = components;
            return true;
        }

        bool ReadRequiredPositive(const Json &object, const char *key, const std::string &path, float &out,
            std::string &error)
        {
            const auto entry = object.find(key);
            if (entry == object.end())
            {
                error = std::format("{}: missing", path);
                return false;
            }
            return ReadPositiveNumber(*entry, path, out, error);
        }

        bool ReadRequiredVector3(const Json &object, const char *key, const std::string &path, glm::vec3 &out,
            std::string &error)
        {
            const auto entry = object.find(key);
            if (entry == object.end())
            {
                error = std::format("{}: missing", path);
                return false;
            }
            return ReadVector3(*entry, path, out, error);
        }

        bool ParseDocument(const Json &document, AircraftConfig &out, std::string &error)
        {
            AircraftConfig config;

            for (const auto &entry : document.items())
            {
                if (!IsKnownKey(entry.key()))
                {
                    error = std::format("{}: unknown key", entry.key());
                    return false;
                }
            }

            const auto version = document.find("version");
            if (version == document.end())
            {
                error = "version: missing";
                return false;
            }
            if (!version->is_number() || version->get<double>() != 1.0)
            {
                error = "version: must be 1";
                return false;
            }
            config.version = 1;

            const auto mass = document.find("mass");
            if (mass == document.end())
            {
                error = "mass: missing";
                return false;
            }
            if (!ReadPositiveNumber(*mass, "mass", config.mass, error))
                return false;

            const auto inertia = document.find("inertia");
            if (inertia == document.end())
            {
                error = "inertia: missing";
                return false;
            }
            if (!inertia->is_object())
            {
                error = "inertia: expected an object";
                return false;
            }
            if (!ReadRequiredVector3(*inertia, "diagonal", "inertia.diagonal", config.inertiaDiagonal, error))
                return false;
            for (std::size_t i = 0; i < 3; ++i)
            {
                if (config.inertiaDiagonal[i] <= 0.0f)
                {
                    error = std::format("inertia.diagonal[{}]: must be > 0", i);
                    return false;
                }
            }
            if (const auto products = inertia->find("products"); products != inertia->end()
                && !ReadVector3(*products, "inertia.products", config.inertiaProducts, error))
                return false;

            if (!ReadRequiredVector3(document, "centerOfMass", "centerOfMass", config.centerOfMass, error))
                return false;

            const auto reference = document.find("reference");
            if (reference == document.end())
            {
                error = "reference: missing";
                return false;
            }
            if (!reference->is_object())
            {
                error = "reference: expected an object";
                return false;
            }
            if (!ReadRequiredPositive(*reference, "area", "reference.area", config.reference.area, error)
                || !ReadRequiredPositive(*reference, "span", "reference.span", config.reference.span, error)
                || !ReadRequiredPositive(*reference, "chord", "reference.chord", config.reference.chord, error))
                return false;

            const auto camera = document.find("camera");
            if (camera == document.end())
            {
                error = "camera: missing";
                return false;
            }
            if (!camera->is_object())
            {
                error = "camera: expected an object";
                return false;
            }
            if (!ReadRequiredVector3(*camera, "eye", "camera.eye", config.camera.eye, error))
                return false;
            const auto chaseDistance = camera->find("chaseDistance");
            if (chaseDistance == camera->end())
            {
                error = "camera.chaseDistance: missing";
                return false;
            }
            if (!ReadNonNegativeNumber(*chaseDistance, "camera.chaseDistance", config.camera.chaseDistance, error))
                return false;
            const auto chaseHeight = camera->find("chaseHeight");
            if (chaseHeight == camera->end())
            {
                error = "camera.chaseHeight: missing";
                return false;
            }
            if (!ReadNonNegativeNumber(*chaseHeight, "camera.chaseHeight", config.camera.chaseHeight, error))
                return false;

            const auto nodes = document.find("nodes");
            if (nodes != document.end())
            {
                if (!nodes->is_object())
                {
                    error = "nodes: expected an object";
                    return false;
                }
                for (const auto &entry : nodes->items())
                {
                    const std::string &component = entry.key();
                    if (!entry.value().is_string())
                    {
                        error = std::format("nodes.{}: expected a string", component);
                        return false;
                    }
                    config.nodes.push_back(
                        AircraftConfig::Node{ component, entry.value().get<std::string>() });
                }
            }

            out = std::move(config);
            return true;
        }

        bool DecodeAircraftConfig(const AssetBlobView &root, const AssetDependencies &, AircraftConfig &out,
            std::string &error)
        {
            if (root.header.typeTag != kRawAssetTypeTag)
            {
                error = std::format("expected a raw blob, got type tag {:#010x}", root.header.typeTag);
                return false;
            }
            return ParseAircraftConfig(AsStringView(root), out, error);
        }
    }

    bool ParseAircraftConfig(std::string_view json, AircraftConfig &out, std::string &error)
    {
        Json document;
        try
        {
            document = Json::parse(json);
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            return false;
        }

        if (!document.is_object())
        {
            error = "$: expected an object";
            return false;
        }

        return ParseDocument(document, out, error);
    }

    const AssetDecoder<AircraftConfig> &AircraftConfigDecoder()
    {
        static constexpr AssetDecoder<AircraftConfig> kDecoder{&DecodeAircraftConfig, {}, nullptr};
        return kDecoder;
    }
}

/**
 * @file ComponentJson.cpp
 * @author Sumin Park
 * @brief Field <-> Json conversion for type-erased component fields.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "scene/ComponentJson.h"

#include "core/math/WorldReal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        double CleanFloat(float value)
        {
            if (!std::isfinite(value))
                return static_cast<double>(value);

            char buffer[32];
            const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), value);

            double clean = 0.0;
            std::from_chars(buffer, formatted.ptr, clean);
            return clean;
        }

        bool ReadInt32(const json &value, int32_t &out)
        {
            if (!value.is_number_integer())
                return false;

            if (value.is_number_unsigned())
            {
                const uint64_t raw = value.get<uint64_t>();
                if (raw > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
                    return false;
                out = static_cast<int32_t>(raw);
                return true;
            }

            const int64_t raw = value.get<int64_t>();
            if (raw < std::numeric_limits<int32_t>::min() || raw > std::numeric_limits<int32_t>::max())
                return false;
            out = static_cast<int32_t>(raw);
            return true;
        }

        bool ReadUint32(const json &value, uint32_t &out)
        {
            if (!value.is_number_integer())
                return false;

            if (value.is_number_unsigned())
            {
                const uint64_t raw = value.get<uint64_t>();
                if (raw > std::numeric_limits<uint32_t>::max())
                    return false;
                out = static_cast<uint32_t>(raw);
                return true;
            }

            const int64_t raw = value.get<int64_t>();
            if (raw < 0 || raw > std::numeric_limits<uint32_t>::max())
                return false;
            out = static_cast<uint32_t>(raw);
            return true;
        }

        bool ReadFloats(const json &value, float *out, std::size_t count)
        {
            if (!value.is_array() || value.size() != count)
                return false;

            float values[16]{};
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!value[i].is_number())
                    return false;
                values[i] = value[i].get<float>();
            }
            std::memcpy(out, values, count * sizeof(float));
            return true;
        }
    }

    json FieldToJson(FieldKind kind, const void *bytes)
    {
        switch (kind)
        {
        case FieldKind::Bool:
            return *static_cast<const bool *>(bytes);
        case FieldKind::Int:
            return *static_cast<const int32_t *>(bytes);
        case FieldKind::Float:
            return CleanFloat(*static_cast<const float *>(bytes));
        case FieldKind::Vec3:
            {
                const auto &v = *static_cast<const glm::vec3 *>(bytes);
                return json::array({ CleanFloat(v.x), CleanFloat(v.y), CleanFloat(v.z) });
            }
        case FieldKind::WorldPosition:
            {
                const auto &v = *static_cast<const WorldVec3 *>(bytes);
                if constexpr (kLargeWorld)
                    return json::array({ v.x, v.y, v.z });
                else
                    return json::array({ CleanFloat(static_cast<float>(v.x)), CleanFloat(static_cast<float>(v.y)),
                                         CleanFloat(static_cast<float>(v.z)) });
            }
        case FieldKind::Vec4:
            {
                const auto &v = *static_cast<const glm::vec4 *>(bytes);
                return json::array({ CleanFloat(v.x), CleanFloat(v.y), CleanFloat(v.z), CleanFloat(v.w) });
            }
        case FieldKind::Quat:
            {
                const auto &q = *static_cast<const glm::quat *>(bytes);
                return json::array({ CleanFloat(q.x), CleanFloat(q.y), CleanFloat(q.z), CleanFloat(q.w) });
            }
        case FieldKind::Mat4:
            {
                const float *m = &static_cast<const glm::mat4 *>(bytes)->operator[](0).x;
                json arr = json::array();
                for (int i = 0; i < 16; ++i)
                    arr.push_back(CleanFloat(m[i]));
                return arr;
            }
        case FieldKind::Handle:
            {
                const auto *h = static_cast<const uint32_t *>(bytes);
                return json::array({ h[0], h[1] });
            }
        case FieldKind::String:
            {
                return std::string(static_cast<const char *>(bytes));
            }
        case FieldKind::EntityRef:
            break;
        }
        return nullptr;
    }

    bool JsonToField(FieldKind kind, const json &value, void *outBytes)
    {
        switch (kind)
        {
        case FieldKind::Bool:
            {
                if (!value.is_boolean())
                    return false;
                const bool parsed = value.get<bool>();
                std::memcpy(outBytes, &parsed, sizeof(parsed));
                return true;
            }
        case FieldKind::Int:
            {
                int32_t parsed = 0;
                if (!ReadInt32(value, parsed))
                    return false;
                std::memcpy(outBytes, &parsed, sizeof(parsed));
                return true;
            }
        case FieldKind::Float:
            {
                if (!value.is_number())
                    return false;
                const float parsed = value.get<float>();
                std::memcpy(outBytes, &parsed, sizeof(parsed));
                return true;
            }
        case FieldKind::Vec3:
            {
                glm::vec3 parsed{};
                if (!ReadFloats(value, &parsed.x, 3))
                    return false;
                std::memcpy(outBytes, &parsed, sizeof(parsed));
                return true;
            }
        case FieldKind::WorldPosition:
            {
                if (!value.is_array() || value.size() != 3)
                    return false;

                glm::dvec3 meters{};
                for (int i = 0; i < 3; ++i)
                {
                    if (!value[i].is_number())
                        return false;
                    meters[i] = value[i].get<double>();
                }
                const std::optional<WorldVec3> position = TryWorldVec3FromDouble(meters);
                if (!position)
                    return false;
                std::memcpy(outBytes, &*position, sizeof(*position));
                return true;
            }
        case FieldKind::Vec4:
            {
                glm::vec4 parsed{};
                if (!ReadFloats(value, &parsed.x, 4))
                    return false;
                std::memcpy(outBytes, &parsed, sizeof(parsed));
                return true;
            }
        case FieldKind::Quat:
            {
                glm::quat parsed{};
                float values[4]{};
                if (!ReadFloats(value, values, 4))
                    return false;
                parsed.x = values[0];
                parsed.y = values[1];
                parsed.z = values[2];
                parsed.w = values[3];
                std::memcpy(outBytes, &parsed, sizeof(parsed));
                return true;
            }
        case FieldKind::Mat4:
            {
                glm::mat4 parsed{};
                if (!ReadFloats(value, &parsed[0].x, 16))
                    return false;
                std::memcpy(outBytes, &parsed, sizeof(parsed));
                return true;
            }
        case FieldKind::Handle:
            {
                uint32_t handles[2]{};
                if (!value.is_array() || value.size() != 2)
                    return false;
                if (!ReadUint32(value[0], handles[0]) || !ReadUint32(value[1], handles[1]))
                    return false;
                std::memcpy(outBytes, handles, sizeof(handles));
                return true;
            }
        case FieldKind::String:
            {
                if (!value.is_string())
                    return false;
                const std::string &text = value.get_ref<const std::string &>();
                auto *out = static_cast<char *>(outBytes);
                const std::size_t count =
                    std::min(text.size(), static_cast<std::size_t>(kFieldStringCapacity) - 1);
                std::memcpy(out, text.data(), count);
                out[count] = '\0';
                return true;
            }
        case FieldKind::EntityRef:
            return true;
        }
        return false;
    }
}

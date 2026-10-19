/**
 * @file ComponentRegistration.cpp
 * @author Rahul Nair
 * @brief Registers every component the renderer module defines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/ComponentRegistration.h"

#include "renderer/components/Camera.h"
#include "renderer/components/Light.h"
#include "renderer/components/MeshRenderer.h"
#include "renderer/components/Sky.h"
#include "renderer/BloomEffect.h"
#include "renderer/DepthOfFieldEffect.h"
#include "renderer/FogEffect.h"
#include "renderer/HdrEffect.h"
#include "renderer/Shapes.h"

#include <core/ecs/ComponentGizmo.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EntityPresetRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <core/log/Log.h>

#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string_view>

namespace mir
{
    namespace
    {
        bool AnyEntityHas(const World &world, bool (*has)(const World &, Entity))
        {
            bool found = false;
            world.ForEachAliveEntity([&](Entity entity)
                {
                    if (!found && has(world, entity))
                        found = true;
                });
            return found;
        }

        bool CanCreateSky(const World &world)
        {
            const bool exists = AnyEntityHas(world, [](const World &w, Entity e) { return w.Has<Sky>(e); });
            if (exists)
                MIR_LOG_ERROR("Create Sky: a Sky entity already exists in this scene - only one is supported");
            return !exists;
        }

        bool CanCreateSunLight(const World &world)
        {
            const bool exists = AnyEntityHas(world, [](const World &w, Entity e)
                {
                    const Light *light = w.GetComponent<Light>(e);
                    return light != nullptr && light->mType == LightType::Sun;
                });
            if (exists)
                MIR_LOG_ERROR("Create Sun Light: a Sun-type Light already exists in this scene - only the first "
                    "one found drives the sun, so a second would silently do nothing; only one is supported");
            return !exists;
        }

        template <LightType Type>
        void CreateLightEntity(World &world, Entity entity)
        {
            AddTransform(world, entity);
            Light light{};
            light.mType = Type;
            world.AddComponent<Light>(entity, light);
            world.AddComponent<Name>(entity,
                MakeName(std::format("{}_Entity{}", LightTypeName(Type), entity.mIndex)));
        }

        void CreatePrimitiveEntity(World &world, Entity entity, std::string_view meshName,
            std::string_view displayName)
        {
            AddTransform(world, entity);

            MeshRenderer renderer{};
            std::memcpy(renderer.meshName, meshName.data(),
                std::min(meshName.size(), sizeof(renderer.meshName) - 1));
            constexpr std::string_view kDefaultShader = "default";
            std::memcpy(renderer.materialShader, kDefaultShader.data(), kDefaultShader.size());
            world.AddComponent<MeshRenderer>(entity, renderer);

            world.AddComponent<Name>(entity, MakeName(std::format("{}_Entity{}", displayName, entity.mIndex)));
        }

        // Viewport click-pick: a world-space sphere radius around the entity's transform origin.
        float LightPickRadius(const void *component)
        {
            const Light &light = *static_cast<const Light *>(component);
            constexpr float kBaseRadius = 0.3f;
            switch (light.mType)
            {
            case LightType::Sun:
                return kBaseRadius;
            case LightType::Point:
            case LightType::Spot:
                return std::max(kBaseRadius, light.mRange);
            case LightType::Area:
                return std::max(kBaseRadius, light.mAreaSize * 1.5f);
            }
            return kBaseRadius;
        }

        void DrawLightGizmo(const void *component, const glm::mat4 &worldMatrix, bool selected,
            const GizmoCanvas &canvas)
        {
            const Light &light = *static_cast<const Light *>(component);

            const glm::vec3 pos = glm::vec3(worldMatrix[3]);
            const glm::vec3 right = glm::normalize(glm::vec3(worldMatrix[0]));
            const glm::vec3 up = glm::normalize(glm::vec3(worldMatrix[1]));
            const glm::vec3 forward = glm::normalize(glm::vec3(worldMatrix * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

            const uint32_t color = selected ? 0xFF50DCFFu : 0x8C78C8C8u;

            switch (light.mType)
            {
            case LightType::Sun:
                {
                    const glm::vec3 sunDir =
                        glm::normalize(glm::vec3(worldMatrix * glm::vec4(kSunReferenceLocalDirection, 0.0f)));
                    canvas.Circle(pos, right, up, 0.3f, color, 16);
                    canvas.Line(pos, pos + sunDir * 0.6f, color, 2.0f);
                    break;
                }
            case LightType::Point:
                {
                    canvas.Circle(pos, right, up, light.mRange, color, 32);
                    canvas.Circle(pos, up, forward, light.mRange, color, 32);
                    canvas.Circle(pos, forward, right, light.mRange, color, 32);
                    break;
                }
            case LightType::Spot:
                {
                    const float clampedAngle = std::clamp(light.mSpotAngleDegrees, 1.0f, 179.0f);
                    const float halfAngle = glm::radians(clampedAngle * 0.5f);
                    const float baseRadius = light.mRange * std::tan(halfAngle);
                    const glm::vec3 baseCenter = pos + forward * light.mRange;
                    canvas.Circle(baseCenter, right, up, baseRadius, color, 24);
                    constexpr int kSpokes = 8;
                    for (int i = 0; i < kSpokes; ++i)
                    {
                        const float angle =
                            (static_cast<float>(i) / static_cast<float>(kSpokes)) * 2.0f * glm::pi<float>();
                        const glm::vec3 edge = baseCenter + right * (baseRadius * std::cos(angle)) +
                            up * (baseRadius * std::sin(angle));
                        canvas.Line(pos, edge, color, 1.5f);
                    }
                    break;
                }
            case LightType::Area:
                {
                    const float s = light.mAreaSize;
                    const glm::vec3 corners[4] = {
                        pos + right * s + up * s, pos - right * s + up * s,
                        pos - right * s - up * s, pos + right * s - up * s };
                    for (int i = 0; i < 4; ++i)
                        canvas.Line(corners[i], corners[(i + 1) % 4], color, 2.0f);
                    canvas.Line(pos, pos + forward * (s * 0.5f), color, 1.5f);
                    const uint32_t rangeColor = selected ? 0x5A50DCFFu : 0x3C78C8C8u;
                    canvas.Circle(pos, right, up, light.mRange, rangeColor, 32);
                    break;
                }
            }
        }
    }

    void RegisterRendererComponents()
    {
        ComponentRegistry::Instance().Register<MeshRenderer>(kMeshRendererFields);
        ComponentRegistry::Instance().Register<Camera>(kCameraFields);
        ComponentRegistry::Instance().Register<Light>(kLightFields, &LightPickRadius, &DrawLightGizmo);
        ComponentRegistry::Instance().Register<Sky>(kSkyFields);
    }

    void RegisterRendererEntityTypes()
    {
        EntityPresetRegistry::Instance().Register("Light/Sun", &CreateLightEntity<LightType::Sun>, &CanCreateSunLight);
        EntityPresetRegistry::Instance().Register("Light/Point", &CreateLightEntity<LightType::Point>);
        EntityPresetRegistry::Instance().Register("Light/Spot", &CreateLightEntity<LightType::Spot>);
        EntityPresetRegistry::Instance().Register("Light/Area", &CreateLightEntity<LightType::Area>);

        EntityPresetRegistry::Instance().Register("Sky", [](World &world, Entity entity)
            {
                AddTransform(world, entity);
                world.AddComponent<Sky>(entity, Sky{});
                world.AddComponent<Name>(entity, MakeName(std::format("Sky_Entity{}", entity.mIndex)));
            }, &CanCreateSky);

        EntityPresetRegistry::Instance().Register("Primitive/Cube", [](World &world, Entity entity)
            {
                CreatePrimitiveEntity(world, entity, kCubeMeshName, "Cube");
            });
        EntityPresetRegistry::Instance().Register("Primitive/Sphere", [](World &world, Entity entity)
            {
                CreatePrimitiveEntity(world, entity, kSphereMeshName, "Sphere");
            });
        EntityPresetRegistry::Instance().Register("Primitive/Plane", [](World &world, Entity entity)
            {
                CreatePrimitiveEntity(world, entity, kPlaneMeshName, "Plane");
            });
        EntityPresetRegistry::Instance().Register("Primitive/Cylinder", [](World &world, Entity entity)
            {
                CreatePrimitiveEntity(world, entity, kCylinderMeshName, "Cylinder");
            });
        EntityPresetRegistry::Instance().Register("Primitive/Capsule", [](World &world, Entity entity)
            {
                CreatePrimitiveEntity(world, entity, kCapsuleMeshName, "Capsule");
            });
        EntityPresetRegistry::Instance().Register("Primitive/Cone", [](World &world, Entity entity)
            {
                CreatePrimitiveEntity(world, entity, kConeMeshName, "Cone");
            });
    }

    void RegisterRendererPostEffects()
    {
        RegisterFogPostEffect();
        RegisterHdrPostEffect();
        RegisterBloomPostEffect();
        RegisterDepthOfFieldPostEffect();
    }
}

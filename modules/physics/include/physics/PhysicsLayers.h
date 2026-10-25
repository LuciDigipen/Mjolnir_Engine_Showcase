/**
 * @file PhysicsLayers.h
 * @author sumin.park
 * @brief The layers a RigidBody can sit on and which layers collide.
 *
 * Deliberately free of any Jolt header, like PhysicsTypes.h.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    inline constexpr int kPhysicsLayerCount = 32;

    // Layer 0 is "Default". An unnamed layer has an empty name and is still a valid index.
    // A table that was never set has every pair colliding.
    struct PhysicsLayerTable
    {
        // collides[a] has bit b set when layer a may collide with layer b, kept symmetric
        std::array<std::string, kPhysicsLayerCount> names;
        std::array<uint32_t, kPhysicsLayerCount> collides;

        PhysicsLayerTable()
        {
            names[0] = "Default";
            collides.fill(0xFFFFFFFFu);
        }

        bool Collide(int a, int b) const { return ((collides[a] >> b) & 1u) != 0; }

        void SetCollide(int a, int b, bool collide)
        {
            const uint32_t forB = 1u << b;
            const uint32_t forA = 1u << a;
            collides[a] = collide ? (collides[a] | forB) : (collides[a] & ~forB);
            collides[b] = collide ? (collides[b] | forA) : (collides[b] & ~forA);
        }

        std::optional<int> IndexOf(std::string_view name) const
        {
            if (name.empty())
                return std::nullopt;
            for (int i = 0; i < kPhysicsLayerCount; ++i)
            {
                if (names[i] == name)
                    return i;
            }
            return std::nullopt;
        }
    };
}

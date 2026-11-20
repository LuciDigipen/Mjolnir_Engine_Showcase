/**
 * @file ActionSystem.h
 * @author Quy Dong Ta
 * @brief Advances the Game clock's actions while the simulation runs.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/System.h>

namespace mir
{
    // In Update, after scripts queue actions in PreUpdate and before physics and propagation read Transform
    class ActionSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override;
    };
}

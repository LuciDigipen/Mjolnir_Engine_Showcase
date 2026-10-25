/**
 * @file PrefabModule.h
 * @author sumin.park
 * @brief The prefab coordinator and the system that commits it while editing.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>

#include <filesystem>

namespace mir
{
    class PrefabCoordinator;

    class PrefabModule
    {
    public:
        // scans sourceDir for prefab documents when it exists
        PrefabModule(ModuleContext context, const std::filesystem::path &sourceDir);

        PrefabModule(const PrefabModule &) = delete;
        PrefabModule &operator=(const PrefabModule &) = delete;

        PrefabCoordinator &Coordinator() { return mCoordinator; }

    private:
        // a World resource, which outlives this module
        PrefabCoordinator &mCoordinator;
    };
}

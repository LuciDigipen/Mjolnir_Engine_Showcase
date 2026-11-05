/**
 * @file ProjectResources.cpp
 * @author sumin.park
 * @brief The project configuration App loads once and lends to every World.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "app/ProjectResources.h"

#include <core/ecs/World.h>
#include <core/fs/Paths.h>
#include <input/DefaultInputMap.h>
#include <physics_layers/PhysicsLayerSettings.h>

namespace mir
{
    namespace
    {
        InputMap LoadInput(const std::filesystem::path &settingsPath)
        {
            InputMap map = InputMap::LoadFile(settingsPath).value_or(InputMap{});
            AddDefaultActions(map);
            return map;
        }
    } // namespace

    ProjectResources::ProjectResources(const GameProject &project)
        : mInput(LoadInput(project.mProjectSettingsPath)),
        mRender(RenderSettings::LoadFile(project.mProjectSettingsPath).value_or(RenderSettings{})),
        mPostProcessing(PostProcessingSettings::LoadFile(project.mProjectSettingsPath)
            .value_or(PostProcessingSettings{})),
        mPhysicsLayers(ReadPhysicsLayers(project.mProjectSettingsPath)),
        mTerrain(project.mRootDir / kTerrainStreamingSettingsFileName,
            ExecutableDir() / kTerrainStreamingLocalSettingsFileName)
    #if MIR_WITH_AUDIO
        ,
        mAudioDefaults(LoadAudioSettings(project.mProjectSettingsPath))
    #endif
    {}

    void ProjectResources::ShareInto(World &world)
    {
        world.ShareResource(mInput);
        world.ShareResource(mRender);
        world.ShareResource(mPostProcessing);
        world.ShareResource(mPhysicsLayers);
        world.ShareResource(mTerrain);
    }

    void ProjectResources::Refresh()
    {
        mTerrain.Refresh();
    }
} // namespace mir

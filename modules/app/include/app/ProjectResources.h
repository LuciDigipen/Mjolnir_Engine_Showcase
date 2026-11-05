/**
 * @file ProjectResources.h
 * @author sumin.park
 * @brief The project configuration App loads once and lends to every World.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <project/GameProject.h>
#include <input/InputMap.h>
#include <physics_layers/PhysicsLayers.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/RenderSettings.h>
#include <terrain/TerrainStreamingConfiguration.h>

#ifndef MIR_WITH_AUDIO
#error "MIR_WITH_AUDIO is set by linking mir::app"
#endif
#if MIR_WITH_AUDIO
#include <audio/AudioSettings.h>
#endif

namespace mir
{
    class World;

    // Worlds hold references to these values, so a writer edits them in place and never replaces them.
    class ProjectResources
    {
      public:
        explicit ProjectResources(const GameProject &project);

        ProjectResources(const ProjectResources &) = delete;
        ProjectResources &operator=(const ProjectResources &) = delete;
        ProjectResources(ProjectResources &&) = delete;
        ProjectResources &operator=(ProjectResources &&) = delete;

        // The World borrows the values, so this object must outlive it. Audio stays with AudioModule.
        void ShareInto(World &world);

        // The point in a frame where edited configuration is merged and validated.
        void Refresh();

        InputMap &Input() { return mInput; }
        RenderSettings &Render() { return mRender; }
        PostProcessingSettings &PostProcessing() { return mPostProcessing; }
        PhysicsLayerTable &PhysicsLayers() { return mPhysicsLayers; }
        TerrainStreamingConfiguration &Terrain() { return mTerrain; }
    #if MIR_WITH_AUDIO
        AudioSettings &AudioDefaults() { return mAudioDefaults; }
    #endif

      private:
        InputMap mInput;
        RenderSettings mRender;
        PostProcessingSettings mPostProcessing;
        PhysicsLayerTable mPhysicsLayers;
        TerrainStreamingConfiguration mTerrain;
    #if MIR_WITH_AUDIO
        AudioSettings mAudioDefaults;
    #endif
    };
} // namespace mir

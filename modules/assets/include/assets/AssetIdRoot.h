/**
 * @file AssetIdRoot.h
 * @author sumin.park
 * @brief A game's asset id prefix, derived from its game.json paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

namespace mir
{
    struct GameProject;

    // Fills project.mAssetIdRoot with the normalizer AssetCooker applies to its --source
    // roots, so ids match. Throws SetupError (ProjectData) for an assetsRoot that is not
    // a relative path inside the game directory
    void ResolveAssetIdRoot(GameProject &project);
}

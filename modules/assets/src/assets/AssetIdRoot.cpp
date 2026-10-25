/**
 * @file AssetIdRoot.cpp
 * @author sumin.park
 * @brief A game's asset id prefix, derived from its game.json paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/AssetIdRoot.h>

#include <assets/AssetPath.h>
#include <core/log/SetupError.h>
#include <project/GameProject.h>

#include <string>
#include <utility>

namespace mir
{
    void ResolveAssetIdRoot(GameProject &project)
    {
        AssetPathResult idRoot = MakeLogicalAssetPath(project.mDir, project.mAssetsRoot);
        if (!idRoot.Ok())
        {
            throw SetupError(ErrorCategory::ProjectData, "game.json has an unusable assetsRoot '" + project.mAssetsRoot + "'",
                project.mRootDir / kGameManifestFileName, std::string(Describe(idRoot.error)),
                "Make assetsRoot a relative path inside the game directory, e.g. assets.");
        }
        project.mAssetIdRoot = std::move(idRoot.path);
    }
}

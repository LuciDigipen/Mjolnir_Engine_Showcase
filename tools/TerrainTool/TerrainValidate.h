/**
 * @file TerrainValidate.h
 * @author Jackey Cen
 * @brief Validates a baked terrain candidate: integrity, coverage, finite heights, seams and identity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainContinuity.h"
#include "TerrainRecipe.h"
#include "TerrainReport.h"

#include <terrain/TerrainRelease.h>

#include <filesystem>

namespace mir::terrainbake
{
    struct TerrainValidateOptions
    {
        // When set, its fingerprint and layout must match.
        const TerrainRecipe *recipe = nullptr;
        // Published release directories have no candidate.json.
        bool requireManifest = true;
        // Manifest, files, packs and identity; content checks stay with bake and validate.
        bool integrityOnly = false;
        // An incremental candidate: records of unchanged tiles must equal this release's payload bytes.
        const TerrainReleaseHandle *baseRelease = nullptr;
    };

    // Reads only through TerrainReleaseHandle; writes nothing.
    void ValidateTerrainCandidate(const std::filesystem::path &dir, const TerrainValidateOptions &options,
                                  TerrainReport &report);

    struct TerrainValidateRequest
    {
        std::filesystem::path candidateDir;
        // Empty: no recipe check.
        std::filesystem::path recipePath;
        // An incremental candidate's base; absent rederives every payload.
        std::optional<TerrainReleaseHandle> baseRelease;
    };

    TerrainReport RunTerrainValidate(const TerrainValidateRequest &request);
}

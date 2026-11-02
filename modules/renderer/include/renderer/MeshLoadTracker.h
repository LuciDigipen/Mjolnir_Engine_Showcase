/**
 * @file MeshLoadTracker.h
 * @author sumin.park
 * @brief Follows each mesh asset from its background decode to a usable GPU copy.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/AssetUploadCache.h>
#include <renderer/IMeshUploader.h>
#include <renderer/MeshAssetDecoder.h>

namespace mir
{
    using MeshLoadPhase = AssetUploadPhase;
    struct MeshLoad
    {
        MeshLoadPhase phase = MeshLoadPhase::Decoding;
        MeshHandle mesh;
        MeshBounds bounds;
    };
    class MeshLoadTracker : public AssetUploadCache<DecodedMesh, MeshHandle, MeshLoad, &MeshLoad::mesh>
    {
        using Base = AssetUploadCache<DecodedMesh, MeshHandle, MeshLoad, &MeshLoad::mesh>;
    public:
        MeshLoadTracker(IMeshUploader &uploader, AsyncAssetService *assets);
    };
    using MeshLoadLease = AssetUploadLease<MeshLoadTracker>;
}

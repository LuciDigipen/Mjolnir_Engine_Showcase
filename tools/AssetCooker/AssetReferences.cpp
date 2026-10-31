/**
 * @file AssetReferences.cpp
 * @author sumin.park
 * @brief Lists the assets a cooked blob points at.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "AssetReferences.h"

#include <assets/ModelAsset.h>
#include <particles/ParticleEffect.h>
#include <prefab/PrefabAsset.h>
#include <prefab/PrefabRegistry.h>

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace mir::cook
{
    namespace
    {
        class Collector
        {
        public:
            void Add(AssetId id)
            {
                if (!id.IsNull() && mSeen.insert(id.value).second)
                    mIds.push_back(id);
            }
            std::vector<AssetId> Take() { return std::move(mIds); }

        private:
            std::vector<AssetId> mIds;
            std::unordered_set<uint64_t> mSeen;
        };

        template <class T>
        T Decode(const AssetBlobView &blob)
        {
            T asset;
            std::string error;
            if (!ReadTypedAsset(blob, asset, error))
                throw std::runtime_error(error);
            return asset;
        }
    }

    std::vector<AssetId> CollectAssetReferences(const AssetBlobView &blob)
    {
        Collector references;
        switch (blob.header.typeTag)
        {
        case kModelAssetTypeTag:
            {
                const ModelAsset model = Decode<ModelAsset>(blob);
                for (const ModelNode &node : model.nodes)
                    references.Add(node.mesh);
                for (const SkinBinding &skin : model.skins)
                    references.Add(skin.skeleton);
                for (const AssetId clip : model.animations)
                    references.Add(clip);
                break;
            }
        case kMeshAssetTypeTag:
            {
                const MeshAsset mesh = Decode<MeshAsset>(blob);
                for (const MeshPrimitive &primitive : mesh.primitives)
                {
                    references.Add(primitive.material);
                    references.Add(primitive.baseColorTexture);
                }
                break;
            }
        case kMaterialAssetTypeTag:
            references.Add(Decode<MaterialAsset>(blob).baseColorTexture);
            break;
        case kSurfaceMaterialAssetTypeTag:
            {
                const SurfaceMaterialAsset material = Decode<SurfaceMaterialAsset>(blob);
                references.Add(material.albedoTexture);
                references.Add(material.normalTexture);
                references.Add(material.surfaceTexture);
                break;
            }
        case kParticleEmitterTypeTag:
            {
                const ParticleEmitterAssetResult emitter = ReadParticleEmitterAsset(blob);
                if (!emitter.mEmitter)
                    throw std::runtime_error(emitter.mError);
                references.Add(emitter.mEmitter->texture);
                references.Add(emitter.mEmitter->script);
                references.Add(emitter.mEmitter->scriptLayout);
                break;
            }
        case kParticleEffectTypeTag:
            {
                const ParticleEffectResult effect = ReadParticleEffectAsset(blob);
                if (!effect.mEffect)
                    throw std::runtime_error(effect.mError);
                for (const AssetId emitter : effect.mEffect->emitters)
                    references.Add(emitter);
                break;
            }
        case kPrefabAssetTypeTag:
            {
                const PrefabAssetParseResult prefab = ReadPrefabAsset(blob);
                if (!prefab.mAsset)
                    throw std::runtime_error("prefab payload does not decode");
                for (const PrefabId nested : prefab.mAsset->mDependencies)
                    references.Add(PrefabAssetId(nested));
                break;
            }
        default:
            break;
        }
        return references.Take();
    }
}

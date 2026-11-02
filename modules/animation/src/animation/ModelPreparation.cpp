/**
 * @file ModelPreparation.cpp
 * @author sumin.park
 * @brief Background model preparation, texture settling and GPU mesh and material publication.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "animation/ModelPreparation.h"

#include <core/log/Log.h>
#include <renderer/MeshAssetDecoder.h>

#include <format>
#include <stdexcept>

namespace mir
{
    namespace
    {
        SamplerDesc ToSamplerDesc(const MaterialAsset &material)
        {
            const auto wrap = [](uint32_t gl)
                {
                    return gl == 33071 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                        : gl == 33648 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                        : VK_SAMPLER_ADDRESS_MODE_REPEAT;
                };

            SamplerDesc sampler;
            sampler.magFilter = material.magFilter == 9728 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            // NEAREST, NEAREST_MIPMAP_NEAREST, NEAREST_MIPMAP_LINEAR.
            sampler.minFilter = (material.minFilter == 9728 || material.minFilter == 9984 ||
                material.minFilter == 9986)
                ? VK_FILTER_NEAREST
                : VK_FILTER_LINEAR;
            // *_MIPMAP_LINEAR.
            sampler.mipmapMode = (material.minFilter == 9986 || material.minFilter == 9987)
                ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = wrap(material.wrapS);
            sampler.addressModeV = wrap(material.wrapT);
            // Only the four *_MIPMAP_* minFilters sample past level 0.
            sampler.useMips = material.minFilter >= 9984;
            return sampler;
        }
    }

    ModelPreparation::ModelPreparation(IMeshResolveHost &host, ITextureUploader &textures, TextureHandle errorTexture,
        AsyncAssetService *assets, AssetId id, std::string label)
        : mHost(host), mErrorTexture(errorTexture), mId(id), mLabel(std::move(label)),
          mTracker(textures.TextureLoads(assets)), mMeshTracker(host.ModelMeshes(assets)),
          mMaterials(host.MaterialRecords()), mService(assets), mRevision(assets != nullptr ? assets->Revision() : 0)
    {
        if (assets == nullptr)
        {
            Fail("there is no asset service");
            return;
        }
        mResources = assets->RequestLease(id, ModelResourcesDecoder());
    }

    void ModelPreparation::Fail(std::string reason)
    {
        mError = std::move(reason);
        mPrepared.reset();
        mMeshSlots.clear();
        mTextureClaims.clear();
        mResources.Reset();
        mState = State::Failed;
    }

    ModelPreparation::State ModelPreparation::Advance()
    {
        if (mState == State::Loading)
        {
            switch (mResources.State())
            {
            case AssetLoadState::Queued:
                return mState;
            case AssetLoadState::Ready:
                mLoaded = mResources.Acquire();
                if (!mLoaded)
                    Fail("its decoded data is missing");
                else
                    RequestTextures(*mLoaded);
                break;
            case AssetLoadState::Failed:
                Fail(mResources.FailureReason());
                break;
            case AssetLoadState::Stale:
                Fail("its decoded data is no longer available");
                break;
            }
        }
        if (mState == State::Textures)
        {
            mTracker->Update();
            if (TexturesSettled())
                Build();
        }
        if (mState == State::Meshes)
            AwaitMeshes();
        return mState;
    }

    void ModelPreparation::RequestTextures(const ModelResources &resources)
    {
        mResources.Reset();
        for (const auto &[key, material] : resources.materials)
        {
            const AssetId texture = material.baseColorTexture;
            if (texture.IsNull() || mTextureClaims.contains(texture.value))
                continue;
            const UploadKey key{ texture, UploadContent(mService, texture) };
            mTextureKeys.emplace(texture.value, key);
            mTracker->Request(key, std::format("{} texture {:016x}", mLabel, texture.value));
            mTextureClaims.emplace(texture.value, TextureLoadLease(mTracker, key));
        }
        mState = State::Textures;
    }

    bool ModelPreparation::TexturesSettled() const
    {
        for (const auto &[key, claim] : mTextureClaims)
        {
            const TextureLoad *view = claim.Get();
            if (view != nullptr && view->phase != AssetUploadPhase::Ready && view->phase != AssetUploadPhase::Failed)
                return false;
        }
        return true;
    }

    void ModelPreparation::Build()
    {
        auto model = std::make_unique<PreparedModel>();
        model->resources = mLoaded;
        std::vector<MeshSlot> slots;
        try
        {
            const ModelResources &resources = *mLoaded;
            for (const auto &[key, mesh] : resources.meshes)
            {
                auto &primitives = model->meshes[key];
                for (size_t index = 0; index < mesh.primitives.size(); ++index)
                {
                    const MeshPrimitive &p = mesh.primitives[index];
                    // converted by the worker that decoded the model, nothing to draw fails the model
                    const std::shared_ptr<const DecodedMesh> upload = resources.gpuMeshes.at(key).at(index);
                    if (!upload)
                        throw std::runtime_error("GPU mesh creation failed");
                    MaterialAsset material;
                    bool materialRead = true;
                    if (!p.material.IsNull())
                    {
                        const auto found = resources.materials.find(p.material.value);
                        materialRead = found != resources.materials.end();
                        if (materialRead)
                            material = found->second;
                        else
                            MIR_LOG_ERROR("Model {:016x}: material {:016x} is unreadable, using error texture",
                                mId.value, p.material.value);
                    }
                    if (materialRead && material.baseColorTexture != p.baseColorTexture)
                        throw std::runtime_error("mesh/material texture reference mismatch");
                    MaterialDesc desc;
                    desc.shaderName = "model";
                    desc.cullMode = material.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
                    desc.topology = p.topology == MeshTopology::Triangles ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                        : p.topology == MeshTopology::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                        : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                    desc.sampler = ToSamplerDesc(material);
                    desc.alphaMode = static_cast<MaterialAlphaMode>(material.alpha);
                    desc.alphaCutoff = material.alphaCutoff;
                    AssetId claimedTexture;
                    if (!materialRead)
                        desc.baseColorTexture = mErrorTexture;
                    else if (!material.baseColorTexture.IsNull())
                    {
                        const TextureLoad *view = mTextureClaims.at(material.baseColorTexture.value).Get();
                        if (view != nullptr && view->phase == AssetUploadPhase::Ready)
                        {
                            desc.baseColorTexture = view->texture;
                            claimedTexture = material.baseColorTexture;
                        }
                        else
                        {
                            MIR_LOG_ERROR("Model {:016x}: texture {:016x} did not load, using error texture",
                                mId.value, material.baseColorTexture.value);
                            desc.baseColorTexture = mErrorTexture;
                        }
                    }

                    // Both variants share the texture, each record keeps its own claim on it.
                    const auto acquire = [&](const MaterialDesc &variant)
                    {
                        std::vector<TextureLoadLease> claims;
                        if (!claimedTexture.IsNull())
                            claims.emplace_back(mTracker, mTextureKeys.at(claimedTexture.value));
                        MaterialLease lease(mMaterials, mMaterials->Acquire(variant, std::move(claims)));
                        const MaterialHandle handle = lease.Get();
                        model->materialClaims.push_back(std::move(lease));
                        return handle;
                    };
                    PreparedPrimitive gpu;
                    gpu.material = acquire(desc);
                    desc.frontFace = VK_FRONT_FACE_CLOCKWISE;
                    gpu.mirrored = acquire(desc);
                    const bool materialFailed = gpu.material.IsNull() || gpu.mirrored.IsNull();
                    if (materialFailed)
                    {
                        MIR_LOG_ERROR("Model {:016x}: '{}' material creation failed, using error material", mId.value,
                            desc.shaderName);
                        if (gpu.material.IsNull())
                            gpu.material = mHost.ErrorMaterial();
                        if (gpu.mirrored.IsNull())
                            gpu.mirrored = mHost.ErrorMaterial();
                    }
                    // Magenta, not the base color, which would tint the error texture and hide the failure.
                    gpu.factor = materialFailed || desc.baseColorTexture == mErrorTexture
                        ? glm::vec4(1, 0, 1, 1)
                        : material.baseColor;
                    gpu.transparent = !materialFailed && material.alpha == MaterialAlpha::Blend;
                    primitives.push_back(gpu);

                    // A record is shared by every consumer that asks for the same bytes and options.
                    // The mesh blob alone decides the vertices, so an unchanged mesh keeps its record across a reload.
                    const uint64_t content = UploadContent(mService, AssetId{ key });
                    const ContentIdentity identity = (content >> 63) != 0 ? ContentIdentity::ForRevision(mRevision)
                                                                          : ContentIdentity::ForContent(content);
                    const GpuRecordKey recordKey{ AssetId{ key }, static_cast<uint32_t>(index), identity,
                        MeshUploadOptions{ .rayTracing = true } };
                    const AssetId record = mMeshTracker->Request(recordKey,
                        [&upload] { return upload; },
                        std::format("{} mesh {:016x}.{}", mLabel, key, index));
                    model->meshClaims.emplace_back(mMeshTracker, record);
                    slots.push_back({ key, primitives.size() - 1 });
                }
            }
        }
        catch (const std::exception &exception)
        {
            Fail(exception.what());
            return;
        }
        for (auto &[key, claim] : mTextureClaims)
            model->textures.push_back(std::move(claim));
        mTextureClaims.clear();
        mLoaded.reset();
        mPrepared = std::move(model);
        mMeshSlots = std::move(slots);
        mState = State::Meshes;
    }

    void ModelPreparation::AwaitMeshes()
    {
        mMeshTracker->Update();
        mMaterials->Trim();
        bool ready = true;
        for (const ModelMeshLease &claim : mPrepared->meshClaims)
        {
            const MeshLoad *view = claim.Get();
            if (view == nullptr || view->phase == AssetUploadPhase::Failed)
            {
                Fail("GPU mesh creation failed");
                return;
            }
            ready = ready && view->phase == AssetUploadPhase::Ready;
        }
        if (!ready)
            return;
        for (size_t slot = 0; slot < mMeshSlots.size(); ++slot)
            mPrepared->meshes.at(mMeshSlots[slot].mesh)[mMeshSlots[slot].primitive].mesh =
                mPrepared->meshClaims[slot].Get()->mesh;
        mMeshSlots.clear();
        mState = State::Ready;
    }

    std::unique_ptr<PreparedModel> ModelPreparation::Take()
    {
        return mState == State::Ready ? std::move(mPrepared) : nullptr;
    }
}

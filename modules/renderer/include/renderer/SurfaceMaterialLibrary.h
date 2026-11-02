/**
 * @file SurfaceMaterialLibrary.h
 * @author Jackey Cen
 * @brief Runtime ownership of cooked surface materials and their packed records.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <renderer/Material.h>
#include <renderer/SurfaceMaterialCatalog.h>
#include <renderer/SurfaceMaterialTexture.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mir
{
    class AssetCache;
    class VulkanRenderer;

    struct SurfaceMaterialBinding
    {
        MaterialHandle material;
        // Present only when the material declares tileSizeMeters.
        std::optional<double> tilePeriodMeters;
        std::optional<SurfaceMaterialMacroParameters> macro;
        std::optional<SurfaceMaterialParallaxParameters> parallax;
    };

    // One cached entry after a committed shader generation migrated its record:
    // the same handle, the committed layout and the values the consumer reads
    // from the committed bytes.
    struct SurfaceMaterialSchemaAdoption
    {
        SurfaceMaterialBinding binding;
        MaterialLayout layout;
        std::string parametersJson;
        std::string revision;
    };

    // Decodes a committed record into the resolved parameters, the terrain
    // metadata and the layout revision. The handle in `binding` is copied
    // through unchanged, so the caller keeps the live material.
    std::optional<SurfaceMaterialSchemaAdoption> AdoptSurfaceMaterialRecord(
        const SurfaceMaterialBinding &binding, const MaterialLayout &layout, std::span<const std::byte> record,
        std::string *error = nullptr);

    // An overlay may name fields from any participating frontend, but unknown
    // fields and texture edits are rejected before frontend-specific filtering.
    bool ValidateSurfaceMaterialParameterOverlay(std::span<const MaterialLayout> layouts,
        std::string_view parametersJson, std::string *error = nullptr);

    // Consumer validation runs before any pipeline is created.
    using SurfaceMaterialValidator = std::function<bool(const SurfaceMaterialBinding &, std::string &)>;
    // Production validation of the packed candidate record, before a numeric
    // preview or a schema migration publishes it.
    using SurfaceMaterialRecordValidator = std::function<bool(const MaterialLayout &, std::span<const std::byte>,
        std::string &)>;

    struct SurfaceMaterialUse
    {
        std::string logicalPath;
        AssetId asset;
        std::string frontend;
        // A previously valid binding must still work after a cook reload.
        bool required = false;

        bool operator==(const SurfaceMaterialUse &) const = default;
    };

    // One instance per RendererModule. Materials are created on first use from
    // the cooked .material.json, the catalog registration and its reflected
    // record; the library shares them by asset path. Invalidate after a cook
    // reload, Reset when the scene's GPU resources are released.
    class SurfaceMaterialLibrary
    {
    public:
        void Bind(AssetCache &assets, VulkanRenderer &renderer);
        void Invalidate();
        void Reset();

        // Reload staging: prepare every active binding from the candidate cache
        // before touching this generation. Commit swaps the whole batch; abort
        // disposes only candidate resources.
        // The active set is what a reload stages and what survives a commit;
        // unreferenced cached materials are dropped.
        void SetMaterialValidator(SurfaceMaterialValidator validator);
        bool ValidateRecord(std::string_view frontend, const MaterialLayout &layout,
            std::span<const std::byte> bytes, std::string &error) const;
        void SetRecordValidator(SurfaceMaterialRecordValidator validator);
        // The consumer bounds, the frontend's validator and the caller's record
        // callback for one proposed record, exactly once. Every library publication path
        // uses this instead of calling the pieces itself.
        bool ValidateRecordForPublication(std::string_view frontend, const MaterialLayout &layout,
            std::span<const std::byte> bytes, std::string &error) const;
        void SetActiveMaterials(std::span<const SurfaceMaterialUse> uses);
        // Mesh consumers keep their own active set; the merged set is what a
        // reload stages, so terrain and mesh owners cannot overwrite each other.
        uint64_t AddMaterialUseOwner();
        void SetMaterialUses(uint64_t owner, std::span<const SurfaceMaterialUse> uses);
        void RemoveMaterialUseOwner(uint64_t owner);
        // The merged set every owner contributed, in a stable order.
        const std::vector<SurfaceMaterialUse> &ActiveMaterials() const { return mActive; }
        bool PrepareReload(AssetCache &candidateAssets, VulkanRenderer &renderer, std::string *error = nullptr);
        void CommitReload(AssetCache &assets);
        void AbortReload();

        uint64_t Generation() const { return mGeneration; }

        // A shader generation was committed, so every surface record may have a
        // new layout. Cheap and non-throwing: the next Acquire or preview adopts
        // the committed layout and bytes from the renderer.
        void NotifyShaderCommit() noexcept;

        // Error is bounded to the current library generation and cached, so
        // callers can log it once. The path is the normalized asset path.
        std::optional<SurfaceMaterialBinding> Acquire(std::string_view logicalPath, AssetId asset,
            std::string_view frontend, std::string *error = nullptr);

        // True while this path and frontend have a material a draw can use.
        bool IsActive(std::string_view logicalPath, std::string_view frontend) const;
        // True while any frontend of this asset has a material a draw can use.
        bool IsActiveAsset(std::string_view logicalPath) const;

        // The layout and revision the library would accept now, read from the
        // registration's current reflection. Missing or malformed reflection is
        // an explicit error.
        std::optional<SurfaceMaterialSchema> CurrentSchema(std::string_view logicalPath, std::string_view frontend,
            std::string *error = nullptr) const;

        // Repacks the active material's resolved record with a partial overlay
        // of numeric fields and publishes it to the shared raster/RT record
        // table. A changed schema is rebuilt first. No pipeline is created for a
        // numeric edit; the consumer and record validators run first and a
        // rejected candidate leaves the last-good record in place.
        bool PreviewParameters(std::string_view logicalPath, std::string_view frontend,
            std::string_view parametersJson, std::string *error = nullptr);

    private:
        struct Entry
        {
            std::optional<SurfaceMaterialBinding> binding;
            std::string error;
            std::string logicalPath;
            AssetId asset;
            std::string frontend;
            // The resolved parameters and their reflected layout, so a preview
            // can repack only the numeric fields and keep the texture rows.
            MaterialLayout layout;
            std::string parametersJson;
            std::string schemaRevision;
            // The shader commit this layout came from; an older one is adopted
            // again before the entry is used or previewed.
            uint64_t mSchemaEpoch = 0;
        };

        TextureHandle EnsureTexture(SurfaceTextureRole role, AssetId asset, std::string &error);
        TextureHandle DefaultTexture(SurfaceTextureRole role);
        // peerLayout and peerRecord carry the live numeric values a later frontend of this asset joins;
        // null or empty builds from the cooked baseline only.
        MaterialHandle CreateFromAsset(std::string_view logicalPath, AssetId asset, std::string_view frontend,
            Entry &entry, const MaterialLayout *peerLayout, std::span<const std::byte> peerRecord,
            std::string &error);
        bool AdoptAcceptedRecord(Entry &entry, std::string &error);
        bool PrepareSchemaCandidate(const Entry &entry, Entry &candidate, bool &changed, std::string &error);
        Entry *FindPeerEntry(AssetId asset, std::string_view frontend);
        void RebuildActiveMaterials();
        void ReleaseRetired();
        void ReleaseAll();
        void TakeResources(std::vector<MaterialHandle> &materials, std::vector<TextureHandle> &textures) const;

        AssetCache *mAssets = nullptr;
        VulkanRenderer *mRenderer = nullptr;
        uint64_t mGeneration = 0;
        uint64_t mSchemaEpoch = 0;
        std::unordered_map<std::string, Entry> mEntries;
        std::unordered_map<uint64_t, TextureHandle> mTextures;
        // Materials replaced by a schema refresh, kept until the next active-set
        // refresh so the frame that still draws the old handle stays valid.
        std::vector<MaterialHandle> mRetiredMaterials;
        TextureHandle mDefaults[static_cast<size_t>(SurfaceTextureRole::Count)];
        std::vector<SurfaceMaterialUse> mActive;
        std::unordered_set<std::string> mActiveKeys;
        // The terrain channel and every mesh owner; the union is mActive.
        std::vector<SurfaceMaterialUse> mTerrainUses;
        std::map<uint64_t, std::vector<SurfaceMaterialUse>> mUseOwners;
        uint64_t mNextUseOwner = 1;
        SurfaceMaterialValidator mValidator;
        SurfaceMaterialRecordValidator mRecordValidator;
        std::unique_ptr<SurfaceMaterialLibrary> mCandidate;
    };
}

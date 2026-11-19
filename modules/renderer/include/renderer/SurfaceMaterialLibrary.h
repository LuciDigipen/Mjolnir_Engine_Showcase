/**
 * @file SurfaceMaterialLibrary.h
 * @author Jackey Cen
 * @brief Runtime ownership of cooked surface materials and their packed records.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetDecoder.h>
#include <assets/AssetId.h>
#include <assets/AssetLease.h>
#include <assets/ModelAsset.h>
#include <renderer/Material.h>
#include <renderer/SurfaceMaterialCatalog.h>
#include <renderer/SurfaceMaterialTexture.h>
#include <renderer/TerrainMaterialAsset.h>
#include <renderer/TextureLoadTracker.h>

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
    class AsyncAssetService;
    class ITextureUploader;
    class SurfaceMaterialHost;
    class VulkanRenderer;

    struct SurfaceMaterialBinding
    {
        MaterialHandle material;
        // Present only when the material declares tileSizeMeters.
        std::optional<double> tilePeriodMeters;
        std::optional<SurfaceMaterialMacroParameters> macro;
        std::optional<SurfaceMaterialParallaxParameters> parallax;
        // Ordered layer names for a layered material; empty for a single surface material.
        std::vector<std::string> layers;
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

    enum class SurfaceMaterialStatus : uint8_t { Ready, Pending, Failed };

    // Pending is a first load that has not settled; a live last-good binding stays Ready while its replacement
    // loads or after that replacement fails.
    struct SurfaceMaterialResolution
    {
        SurfaceMaterialStatus status = SurfaceMaterialStatus::Failed;
        std::optional<SurfaceMaterialBinding> binding;
        std::string error;
    };

    enum class SurfaceMaterialPublicationStatus : uint8_t { Inactive, Pending, Published, Failed };

    // The renderer's current publication of one asset, separate from the last-good draw binding Resolve returns
    // and from the library's global generation. Published means every active frontend accepted the current asset
    // content; Inactive means the scene draws none of it.
    struct SurfaceMaterialPublication
    {
        SurfaceMaterialPublicationStatus status = SurfaceMaterialPublicationStatus::Inactive;
        uint64_t content = 0;
        uint32_t revision = 0;
        std::string error;
    };

    // Exact asset or unique-stem lookup. A stem shared by several recipes is ambiguous, never first-match.
    enum class SurfaceMaterialLookupStatus : uint8_t { NotSurface, Found, Ambiguous, Missing };

    struct SurfaceMaterialLookup
    {
        SurfaceMaterialLookupStatus status = SurfaceMaterialLookupStatus::NotSurface;
        const SurfaceMaterialRegistration *registration = nullptr;
        std::string error;
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

    // The shared cooked payload decoder, so every request for one material shares one decode.
    const AssetDecoder<SurfaceMaterialAsset> &SurfaceMaterialDecoder();

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

        bool operator==(const SurfaceMaterialUse &) const = default;
    };

    // One instance per RendererModule. Materials are created on first use from
    // the cooked .material.json, the catalog registration and its reflected
    // record; the library shares them by asset path. Advance on the main thread
    // once a frame after the asset service drained its completions.
    class SurfaceMaterialLibrary
    {
    public:
        SurfaceMaterialLibrary();
        ~SurfaceMaterialLibrary();
        // The library owns GPU handles; one copy would leave two owners.
        SurfaceMaterialLibrary(const SurfaceMaterialLibrary &) = delete;
        SurfaceMaterialLibrary &operator=(const SurfaceMaterialLibrary &) = delete;

        void Bind(AsyncAssetService *assets, VulkanRenderer &renderer);
        // The seam tests use: host owns GPU-facing creation and the texture uploader owns transfers.
        void Bind(AsyncAssetService *assets, SurfaceMaterialHost &host, ITextureUploader &textures);
        void Reset();

        // Progresses asset decodes and texture uploads, then publishes every ready frontend of one asset
        // together. Pending, failure and supersession are distinct.
        void Advance();

        // The catalog committed a reload: replace live materials asynchronously and keep their old handles
        // until each replacement is ready.
        void CommitReload();

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
        // A consumer calls this after every handle it cached from the previous generation has been rebound.
        // Owner 0 is the terrain set; retired handles and their texture claims outlive it until every owner
        // acknowledges the generation that replaced them.
        void AcknowledgeGeneration(uint64_t owner, uint64_t generation);
        // The merged set every owner contributed, in a stable order.
        const std::vector<SurfaceMaterialUse> &ActiveMaterials() const { return mActive; }

        uint64_t Generation() const { return mGeneration; }
        // Registration lookup stays here so mesh resolvers only need IMeshResolveHost. The reference is an
        // exact .material.json asset path or a shader stem; a shared stem is reported as ambiguous.
        SurfaceMaterialLookup LookupMeshSurface(std::string_view reference) const;

        // The actual async publication of one asset. It never changes the last-good binding or generation.
        SurfaceMaterialPublication Publication(std::string_view logicalPath) const;

        // A shader generation was committed, so every surface record may have a
        // new layout. Cheap and non-throwing: the next Resolve or preview adopts
        // the committed layout and bytes from the renderer.
        void NotifyShaderCommit() noexcept;

        // A first request returns Pending and starts a background load. A live binding returns Ready while a
        // replacement is pending or failed. The error is bounded to the current attempt.
        SurfaceMaterialResolution Resolve(std::string_view logicalPath, AssetId asset, std::string_view frontend);

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
            bool failed = false;
            uint32_t attemptRevision = 0;
            bool attemptFailed = false;
            std::string attemptError;
            uint64_t attemptContent = 0;
            uint64_t acceptedContent = 0;
            uint32_t acceptedRevision = 0;
            bool layered = false;
            TerrainMaterialAsset layeredDefinition;
            std::vector<MaterialHandle> layerMaterials;
        };

        struct Load
        {
            uint64_t identity = 0;
            uint32_t revision = 0;
            AssetLease<SurfaceMaterialAsset> assetLease;
            AssetLease<TerrainMaterialSource> sourceLease;
            std::shared_ptr<const SurfaceMaterialAsset> materialAsset;
            TerrainMaterialAsset layered;
            std::vector<Entry> layerEntries;
            std::vector<std::unique_ptr<Load>> layerLoads;
            bool parsed = false;
            bool failed = false;
            std::string error;
            // by SurfaceTextureRole
            std::unordered_map<uint32_t, TextureLoadLease> textures;
            bool candidateBuilt = false;
            bool candidateCreated = false;
            SurfaceMaterialBinding candidate;
            MaterialLayout candidateLayout;
            std::string candidateParametersJson;
            std::string candidateRevision;
            std::vector<std::byte> candidateRecord;
        };

        static std::string MaterialKey(std::string_view logicalPath, std::string_view frontend);
        Entry &EnsureEntry(std::string_view logicalPath, AssetId asset, std::string_view frontend);
        bool StartLoad(Entry &entry);
        bool ParseLoad(Entry &entry, Load &load, std::string &error);
        bool EnsureTextureLoad(Entry &entry, Load &load, SurfaceTextureRole role,
            const SurfaceMaterialAsset &material, std::string &error);
        bool TexturesSettled(const Load &load) const;
        void FailLoad(Entry &entry, Load &load, std::string error);
        void PublishLoad(Entry &entry, Load &load, uint64_t generation);
        uint64_t CurrentIdentity(AssetId asset, std::span<const uint32_t> tags) const;
        uint64_t EntryIdentity(const Entry &entry) const;
        uint64_t LayeredIdentity(AssetId parent, const TerrainMaterialAsset &definition) const;
        void DiscardLoad(const std::string &key);
        bool AdvanceLayers(Entry &entry, Load &load, bool &pending, std::string &error);
        bool BuildLayeredCandidateData(Entry &entry, Load &load, std::string &error);
        void DiscardLayerCandidates(Load &load);
        void PruneUnused();

        // peerLayout and peerRecord carry the live numeric values a later frontend of this asset joins;
        // null or empty builds from the cooked baseline only.
        bool BuildCandidateData(Entry &entry, Load &load, const MaterialLayout *peerLayout,
            std::span<const std::byte> peerRecord, std::string &error);
        bool AdoptAcceptedRecord(Entry &entry, std::string &error);
        bool PrepareSchemaCandidate(const Entry &entry, Entry &candidate, bool &changed, std::string &error);
        void RebuildActiveMaterials();
        void TryReleaseRetired();
        void ReleaseAll();
        void TakeResources(std::vector<MaterialHandle> &materials) const;

        struct RetiredResources
        {
            uint64_t generation = 0;
            std::vector<MaterialHandle> materials;
            std::vector<TextureLoadLease> textures;
        };

        std::unique_ptr<SurfaceMaterialHost> mOwnedHost;
        SurfaceMaterialHost *mHost = nullptr;
        AsyncAssetService *mAssets = nullptr;
        std::shared_ptr<TextureLoadTracker> mTextureLoads;
        uint64_t mGeneration = 0;
        uint64_t mSchemaEpoch = 0;
        std::unordered_map<std::string, Entry> mEntries;
        std::unordered_map<std::string, Load> mLoads;
        std::unordered_map<std::string, std::vector<TextureLoadLease>> mBindingTextures;
        // Replaced handles stay alive until every registered owner acknowledged the replacing generation.
        std::vector<RetiredResources> mRetired;
        // Candidates created for a failed batch have never been published, so they are released at once.
        std::vector<MaterialHandle> mUncommittedMaterials;
        std::vector<SurfaceMaterialUse> mActive;
        bool mUsagePublished = false;
        std::unordered_set<std::string> mActiveKeys;
        // The terrain channel and every mesh owner; the union is mActive.
        std::vector<SurfaceMaterialUse> mTerrainUses;
        uint64_t mTerrainAck = 0;
        std::map<uint64_t, std::vector<SurfaceMaterialUse>> mUseOwners;
        std::map<uint64_t, uint64_t> mOwnerAcks;
        uint64_t mNextUseOwner = 1;
        SurfaceMaterialValidator mValidator;
        SurfaceMaterialRecordValidator mRecordValidator;
    };
}

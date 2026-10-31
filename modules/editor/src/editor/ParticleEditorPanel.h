/**
 * @file ParticleEditorPanel.h
 * @author sumin.park
 * @brief Particle system and emitter authoring, preview controls and diagnostics.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <particles/ParticleRuntime.h>
#include <core/ecs/Entity.h>

#include <glm/vec3.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    class AssetImportManager;
    class AssetCache;
    class EditorCamera;
    class World;
    class ParticleWorld;
    class ParticleEditorPanel
    {
    public:
        ParticleEditorPanel(std::filesystem::path sourceRoot, std::string idRoot);

        // the id of the file at a path under the source root, null when there is no such file
        AssetId IdOf(const std::string &path);
        void Draw(bool &open, World &world, AssetCache &assets, AssetImportManager &imports,
            const EditorCamera &camera, float dt);
        void OnHidden(World &world);
        bool OpenSystem(const std::string &path, World &world);
        void PrepareNewSystem(const std::string &folder);
        bool CanUndoDraft() const { return !mUndoDrafts.empty() || mGestureStart.has_value(); }
        bool CanRedoDraft() const { return !mRedoDrafts.empty(); }
        void RequestUndoDraft() { mPendingHistoryAction = -1; }
        void RequestRedoDraft() { mPendingHistoryAction = 1; }
        struct InstanceOffsetEdit
        {
            Entity mEntity = kNullEntity;
            uint32_t mPlacementId = 0;
            glm::vec3 mValue{ 0 };
        };
        std::optional<InstanceOffsetEdit> TakeInstanceOffsetEdit() { return std::exchange(mOffsetEdit, std::nullopt); }
    private:
        std::filesystem::path SourceFile(std::string_view relative) const;
        void Scan();
        void LoadSystem(const std::string &path);
        void LoadEmitter(size_t index);
        void CreateSystem(AssetImportManager &imports);
        void CreateEmitter(AssetImportManager &imports);
        void AttachEmitter(const std::string &path);
        void SaveSystem(AssetImportManager &imports);
        void SaveEmitter(AssetImportManager &imports);
        std::string EnsureDefaultTexture(AssetImportManager &imports);
        void DrawEmitter();
        void DrawBehaviors();
        void DrawBehaviorControls(ParticleBehavior &behavior, const ParticleBehaviorInfo &info);
        void DrawDiagnostics(World &world);
        void SaveAll(AssetImportManager &imports);
        void DrawSelectBar(World &world, AssetImportManager &imports);
        void DrawEditBar(AssetImportManager &imports);
        void DrawPreviewStrip(World &world, const EditorCamera &camera, float dt);
        void SyncPreviewDraft(ParticleWorld &particles);
        void DrawEmitterStrip(World &world, AssetImportManager &imports);
        void DrawStatus(AssetImportManager &imports);
        void DrawSettings(World &world);
        // An edited emitter that is not the selected one. Only dirty drafts are kept, so switching never loses an edit.
        struct EmitterDraft
        {
            ParticleEmitterAsset emitter;
            std::string texturePath, source;
        };
        struct DraftSnapshot
        {
            ParticleEffect system;
            std::vector<std::string> references;
            std::optional<ParticleEmitterAsset> emitter;
            std::string systemPath, emitterPath, texturePath;
            std::string systemSource, emitterSource;
            size_t selected = 0;
            std::map<std::string, EmitterDraft> stash;
        };
        bool AnyEmitterDirty() const { return mDirtyEmitter || !mStashedEmitters.empty(); }
        bool IsDirty() const { return mDirtySystem || AnyEmitterDirty(); }
        void StashSelectedEmitter();
        // Dirty flags from a snapshot, so Draw and an undo or redo decide them the same way.
        void RecomputeDirty(const DraftSnapshot &draft);
        // An edit that changes the system, or the emitter when the same one is still selected. Selecting another is no edit.
        static bool DraftChanged(const DraftSnapshot &before, const DraftSnapshot &after);
        // The path itself when unused, else the first free numbered variant, checking every suffix a new file would take.
        std::string UnusedSourcePath(const std::string &path, std::string_view suffix,
            std::initializer_list<std::string_view> alsoSuffixes = {}) const;
        void SaveEmitterDraft(AssetImportManager &imports, const std::string &path, ParticleEmitterAsset &emitter,
            const std::string &texturePath);
        DraftSnapshot CaptureDraft() const;
        void RestoreDraft(const DraftSnapshot &draft);
        void CommitDraftGesture();
        void UndoDraft();
        void RedoDraft();
        std::filesystem::path mSourceRoot;
        std::string mIdRoot, mSystemPath, mEmitterPath, mTexturePath, mError;
        AssetId IdAtLogical(std::string_view logicalPath) const;
        std::function<AssetId(std::string_view)> IdLookup() const;
        std::string mNewSystemPath = "particles/new.particle.json";
        std::string mNewEmitterPath = "particles/new.emitter.json";
        std::vector<std::string> mSystems, mEmitters, mTextures, mReferences;
        std::unordered_map<std::string, size_t> mEmitterUsage;
        ParticleEffect mSystem;
        ParticleBudgets mBudgetDraft;
        bool mBudgetsKnown = false;
        std::optional<ParticleEmitterAsset> mEmitter;
        size_t mSelected = 0;
        int mNewEmitterTarget = 0;
        bool mScanned = false, mDirtySystem = false, mDirtyEmitter = false;
        bool mPreviewing = false, mPreviewPaused = false, mMovePreview = false;
        bool mHistoryApplied = false;
        int mPendingHistoryAction = 0;
        bool mCloseRequested = false;
        std::string mSavedSystemSource;
        std::unordered_map<std::string, std::string> mSavedEmitterSources;
        std::map<std::string, EmitterDraft> mStashedEmitters;
        std::vector<DraftSnapshot> mUndoDrafts, mRedoDrafts;
        std::optional<DraftSnapshot> mGestureStart;
        WorldPose mPreviewAnchor;
        Entity mPreviewEntity = kNullEntity;
        float mPreviewTime = 0;
        // The preview plays the last cook, so data edits are pushed into it live and a recook-only edit marks it stale.
        ParticleHandle mSyncedHandle;
        std::string mSyncedEmitterSource, mDraftEmitterSource;
        bool mPreviewStale = false;
        std::optional<InstanceOffsetEdit> mOffsetEdit;
    };
}

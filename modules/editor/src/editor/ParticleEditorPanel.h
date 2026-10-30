/**
 * @file ParticleEditorPanel.h
 * @author sumin.park
 * @brief Particle system and emitter authoring, preview controls and diagnostics.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <particles/ParticleRuntime.h>
#include <core/ecs/Entity.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mir
{
    class AssetImportManager;
    class AssetCache;
    class EditorCamera;
    class World;
    class ParticleEditorPanel
    {
    public:
        ParticleEditorPanel(std::filesystem::path sourceRoot, std::string idRoot);
        void Draw(bool &open, World &world, AssetCache &assets, AssetImportManager &imports,
            const EditorCamera &camera, float dt);
        void OnHidden(World &world);
        bool OpenSystem(const std::string &path, World &world);
        void PrepareNewSystem(const std::string &folder);
        bool CanUndoDraft() const { return !mUndoDrafts.empty() || mGestureStart.has_value(); }
        bool CanRedoDraft() const { return !mRedoDrafts.empty(); }
        void RequestUndoDraft() { mPendingHistoryAction = -1; }
        void RequestRedoDraft() { mPendingHistoryAction = 1; }
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
        void DrawStack(bool spawn);
        void DrawBehaviors();
        void DrawBehaviorControls(ParticleBehavior &behavior, const ParticleBehaviorInfo &info);
        void DrawDiagnostics(World &world);
        struct DraftSnapshot
        {
            ParticleEffect system;
            std::vector<std::string> references;
            std::optional<ParticleEmitterAsset> emitter;
            std::string systemPath, emitterPath, texturePath;
            std::string systemSource, emitterSource;
            size_t selected = 0;
        };
        DraftSnapshot CaptureDraft() const;
        void RestoreDraft(const DraftSnapshot &draft);
        void CommitDraftGesture();
        void UndoDraft();
        void RedoDraft();
        std::filesystem::path mSourceRoot;
        std::string mIdRoot, mSystemPath, mEmitterPath, mTexturePath, mError;
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
        std::vector<DraftSnapshot> mUndoDrafts, mRedoDrafts;
        std::optional<DraftSnapshot> mGestureStart;
        WorldPose mPreviewAnchor;
        Entity mPreviewEntity = kNullEntity;
        float mPreviewTime = 0;
    };
}

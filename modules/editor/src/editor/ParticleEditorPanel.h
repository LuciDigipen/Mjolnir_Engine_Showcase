/**
 * @file ParticleEditorPanel.h
 * @author sumin.park
 * @brief Particle stack authoring, cooked preview controls and diagnostics.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <particles/ParticleRuntime.h>

#include <filesystem>
#include <optional>
#include <string>
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
    private:
        std::filesystem::path SourceFile(std::string_view relative) const;
        void Scan();
        void LoadSystem(const std::string &path);
        void LoadEmitter(size_t index);
        void DrawEmitter();
        void DrawStack(bool spawn);
        void DrawDiagnostics(World &world);
        std::filesystem::path mSourceRoot;
        std::string mIdRoot, mSystemPath, mEmitterPath, mTexturePath, mError;
        std::vector<std::string> mSystems, mReferences;
        ParticleEffect mSystem;
        ParticleBudgets mBudgetDraft;
        bool mBudgetsKnown = false;
        std::optional<ParticleEmitterAsset> mEmitter;
        size_t mSelected = 0;
        bool mScanned = false, mDirtySystem = false, mDirtyEmitter = false;
        bool mPreviewing = false, mPreviewPaused = false, mMovePreview = false;
        WorldPose mPreviewAnchor;
        float mPreviewTime = 0;
    };
}

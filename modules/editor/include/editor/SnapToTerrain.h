/**
 * @file SnapToTerrain.h
 * @author Jackey Cen
 * @brief One undoable Snap to Terrain: source-precision samples, a fitted plane and a pending session.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>
#include <core/math/WorldCoordinates.h>
#include <editor/history/DocumentId.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    class GroundQuery;
    class IAuthoredEntities;
    class ICommand;
    class World;

    struct SnapToTerrainFacts
    {
        bool mHasBounds = false;
        glm::dvec3 mLocalMin{0.0};
        glm::dvec3 mLocalMax{0.0};
    };

    struct SnapToTerrainPlane
    {
        glm::dvec3 mPoint{0.0};
        glm::dvec3 mNormal{0.0, 1.0, 0.0};
        double mResidual = 0.0;
    };

    // Centroid-relative least squares y = a x + b z + c; nullopt for a degenerate spread.
    [[nodiscard]] std::optional<SnapToTerrainPlane> FitSnapToTerrainPlane(std::span<const glm::dvec3> points);

    // Keeps the current world-Y heading and takes only the terrain's tilt; nullopt for a vertical normal.
    [[nodiscard]] std::optional<glm::quat> SnapToTerrainWorldRotation(const glm::quat &currentWorldRotation,
                                                                      const glm::dvec3 &normal);

    // The same extraction for a parent's world linear part; nullopt for nonuniform, singular or sheared parents.
    [[nodiscard]] std::optional<glm::quat> TrySnapParentRotation(const glm::mat3 &linear,
                                                                 std::string *reason = nullptr);

    // Query positions for the bottom face, centre first: corners plus centre, a grid when the span is
    // large, or a cross of one source spacing when the entity has no bounds.
    [[nodiscard]] std::vector<glm::dvec3> SnapToTerrainFootprint(WorldPosition origin, const glm::mat3 &linear,
                                                                 const SnapToTerrainFacts &facts,
                                                                 double sourceSpacing);

    struct SnapToTerrainEdit
    {
        WorldVec3 mPosition;
        glm::quat mRotation{1.0f, 0.0f, 0.0f, 0.0f};
        std::size_t mSamples = 0;
        double mResidual = 0.0;
        double mSourceSpacing = 0.0;
    };

    struct SnapToTerrainAction
    {
        bool mEnabled = false;
        bool mWaiting = false;
        std::string mReason;
        std::string mStatus;
    };

    // One command for position and rotation; a Prefab member's fields go through the authored backend.
    [[nodiscard]] std::unique_ptr<ICommand> MakeSnapToTerrainCommand(Entity entity,
                                                                     const SnapToTerrainEdit &edit,
                                                                     IAuthoredEntities *authored);

    // What the Inspector and Edit menu show; it never changes the World.
    [[nodiscard]] SnapToTerrainAction SnapToTerrainActionFor(World &world, DocumentId document,
                                                              const GroundQuery *ground);

    class SnapToTerrainSession
    {
    public:
        enum class State
        {
            Idle,
            Waiting,
            Ready,
            Failed,
        };

        static constexpr float kTimeoutSeconds = 10.0f;

        // Starts waiting on the primary selection; returns the reason when it cannot.
        [[nodiscard]] SnapToTerrainAction Begin(World &world, DocumentId document, GroundQuery &ground);

        // Safe at any time; a running session fails with the reason.
        void Cancel(std::string reason);

        // Called once a frame, never from the panel's Draw.
        void Update(World &world, DocumentId document, GroundQuery &ground, float dt, bool editing);

        State CurrentState() const { return mState; }
        bool Active() const { return mState == State::Waiting; }
        Entity Target() const { return mBindings.mEntity; }
        const std::string &Status() const { return mMessage; }

        // Ready only; consumes the edit.
        [[nodiscard]] std::optional<SnapToTerrainEdit> TakeEdit();
        // Failed only; consumes the failure.
        [[nodiscard]] std::string TakeFailure();

        std::size_t SampleCount() const { return mEdit.mSamples; }
        double Residual() const { return mEdit.mResidual; }
        double SourceSpacing() const { return mEdit.mSourceSpacing; }

    private:
        struct Bindings
        {
            World *mWorld = nullptr;
            DocumentId mDocument;
            Entity mEntity;
            Entity mParent;
            uint32_t mTransformVersion = 0;
            uint32_t mParentTransformVersion = 0;
            uint32_t mParentWorldVersion = 0;
            uint64_t mGroundRevision = 0;
            SnapToTerrainFacts mFacts;
            WorldPosition mOrigin;
            glm::quat mCurrentWorldRotation{1.0f, 0.0f, 0.0f, 0.0f};
            glm::quat mParentRotation{1.0f, 0.0f, 0.0f, 0.0f};
            glm::vec3 mScale{1.0f};
            WorldPosition mParentPosition;
            glm::mat3 mParentLinear{1.0f};
            bool mHasParent = false;
        };

        struct Sampled
        {
            enum class State
            {
                Ready,
                Loading,
                Failed,
            };

            State mState = State::Failed;
            std::vector<glm::dvec3> mSurface;
            double mSourceSpacing = 0.0;
            uint64_t mIdentity = 0;
            std::string mMessage;
        };

        void Advance(GroundQuery &ground);
        [[nodiscard]] Sampled Sample(WorldPosition origin, const glm::mat3 &linear, const GroundQuery &ground) const;
        void Fail(std::string reason);
        bool StillBound(const World &world, DocumentId document, const GroundQuery &ground, bool editing);

        State mState = State::Idle;
        Bindings mBindings;
        SnapToTerrainEdit mEdit;
        std::string mMessage;
        float mElapsed = 0.0f;
        int mRound = 0;
        glm::dvec3 mCandidatePosition{0.0};
        glm::quat mCandidateRotation{1.0f, 0.0f, 0.0f, 0.0f};
        uint64_t mGroundIdentity = 0;
    };
}

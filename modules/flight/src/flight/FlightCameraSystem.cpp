/**
 * @file FlightCameraSystem.cpp
 * @author Jackey Cen
 * @brief Applies an aircraft's camera configuration and switches the view on the camera toggle.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightCameraSystem.h>

#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightFrames.h>
#include <flight/FlightInput.h>
#include <flight/components/Aircraft.h>

#include <camera/components/CameraController.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <input/InputState.h>
#include <renderer/CameraCut.h>
#include <renderer/components/Camera.h>

namespace mir
{
    namespace
    {
        bool IsChaseController(const CameraController &controller, Entity aircraft)
        {
            return controller.mMode == CameraMode::Follow && controller.mTarget == aircraft;
        }

        template <typename Fn>
        void ForEachChaseCamera(World &world, Query<CameraController> &controllers, Entity aircraft, Fn &&fn)
        {
            controllers.ForEach(
                [&](Entity entity, CameraController &controller)
                {
                    if (IsChaseController(controller, aircraft) && world.Has<Camera>(entity))
                        fn(entity);
                });
        }

        void SetUpCameras(World &world, Entity aircraftEntity, const AircraftConfig &config,
            AircraftRuntime &runtime, Query<CameraController> &controllers)
        {
            controllers.ForEach(
                [&](Entity, CameraController &controller)
                {
                    if (!IsChaseController(controller, aircraftEntity))
                        return;
                    controller.mOffset = glm::vec3{0.0f, config.camera.chaseHeight, config.camera.chaseDistance};
                    controller.mOffsetInTargetSpace = true;
                    controller.mOffsetHeadingOnly = true;
                    controller.mLookAtTarget = true;
                });

            runtime.cockpitCamera = kNullEntity;
            ForEachChild(world, aircraftEntity,
                [&](Entity child)
                {
                    if (runtime.cockpitCamera.IsNull() && world.Has<Camera>(child))
                        runtime.cockpitCamera = child;
                });

            if (!runtime.cockpitCamera.IsNull())
            {
                if (Camera *cockpit = world.GetComponent<Camera>(runtime.cockpitCamera))
                    cockpit->mActive = false;
                if (Transform *transform = world.GetComponent<Transform>(runtime.cockpitCamera))
                {
                    transform->SetPosition(WorldVec3(BodyToLocal(config.camera.eye)));
                    transform->SetRotation(kIdentityRotation);
                }
            }

            runtime.cameraView = FlightCameraView::Chase;
            runtime.cameraConfig = &config;
        }

        void ToggleView(World &world, Entity aircraftEntity, AircraftRuntime &runtime,
            Query<CameraController> &controllers)
        {
            const Entity cockpitEntity = runtime.cockpitCamera;
            if (cockpitEntity.IsNull() || !world.IsAlive(cockpitEntity))
                return;

            Camera *cockpit = world.GetComponent<Camera>(cockpitEntity);
            if (cockpit == nullptr)
                return;

            bool hasChase = false;
            ForEachChaseCamera(world, controllers, aircraftEntity, [&hasChase](Entity) { hasChase = true; });
            if (!hasChase)
                return;

            const bool toCockpit = runtime.cameraView == FlightCameraView::Chase;
            cockpit->mActive = toCockpit;
            ForEachChaseCamera(world, controllers, aircraftEntity,
                [&](Entity entity)
                {
                    if (Camera *camera = world.GetComponent<Camera>(entity))
                        camera->mActive = !toCockpit;
                    if (!toCockpit)
                        CutCamera(world, entity);
                });
            if (toCockpit)
                CutCamera(world, cockpitEntity);

            runtime.cameraView = toCockpit ? FlightCameraView::Cockpit : FlightCameraView::Chase;
        }
    }

    void FlightCameraSystem::OnStart(SystemContext &context)
    {
        mAircraft = &context.world.GetOrCreateQuery<Aircraft>();
        mControllers = &context.world.GetOrCreateQuery<CameraController>();
    }

    void FlightCameraSystem::OnUpdate(SystemContext &context)
    {
        // Edit mode would write the derived camera setup into the authored scene.
        if (!IsSimulating(context.world))
            return;

        World &world = context.world;
        mAircraft->ForEach(
            [&](Entity entity, Aircraft &aircraft)
            {
                const AircraftConfig *config = aircraft.runtime.config;
                if (config == nullptr || config == aircraft.runtime.cameraConfig)
                    return;

                SetUpCameras(world, entity, *config, aircraft.runtime, *mControllers);
            });

        const InputState *input = world.TryResource<InputState>();
        if (input == nullptr || !input->ForGame(kFlightCameraToggleAction).justPressed)
            return;

        mAircraft->ForEach(
            [&](Entity entity, Aircraft &aircraft)
            {
                if (aircraft.playerInput)
                    ToggleView(world, entity, aircraft.runtime, *mControllers);
            });
    }
}

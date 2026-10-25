engine_add_module_tests(physics
    CollisionEvents.tests.cpp
    HeightField.tests.cpp
    PhysicsInterpolation.tests.cpp
    PhysicsLayerSettings.tests.cpp
    PhysicsSystem.tests.cpp
    PhysicsWorld.tests.cpp
)

# Test-only: HeightField checks the collider against the terrain surface; engine_physics never links terrain.
target_link_libraries(engine_physics_tests PRIVATE mir::terrain)

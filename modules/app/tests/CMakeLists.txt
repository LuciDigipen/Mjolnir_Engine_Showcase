engine_add_module_tests(app
    ContentBrowserWatcher.tests.cpp
    PrefabAuthoring.tests.cpp
    PrefabDocuments.tests.cpp
)

target_link_libraries(engine_app_tests PRIVATE mir::prefab mir::scene)

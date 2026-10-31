engine_add_module_tests(assets
    AssetBlob.tests.cpp
    AssetManifest.tests.cpp
    AssetMeta.tests.cpp
    AssetPath.tests.cpp
    AssetResolver.tests.cpp
    AssetCache.tests.cpp
    AssetSettings.tests.cpp
    AssetsModule.tests.cpp
    AsyncAssetService.tests.cpp
    ModelAsset.tests.cpp
)

# the service tests build a JobExecutor
target_link_libraries(engine_assets_tests PRIVATE mir::jobs)

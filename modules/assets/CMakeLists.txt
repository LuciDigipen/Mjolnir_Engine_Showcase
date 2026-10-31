engine_add_module(assets)

target_sources(engine_assets PRIVATE
    src/assets/AssetBlob.cpp
    src/assets/AssetCache.cpp
    src/assets/AssetId.cpp
    src/assets/AssetIdRoot.cpp
    src/assets/AssetManifest.cpp
    src/assets/AssetPath.cpp
    src/assets/AssetSettings.cpp
    src/assets/AssetsModule.cpp
    src/assets/AsyncAssetService.cpp
    src/assets/ModelAsset.cpp
)

# jobs: the service holds a background lane, its types stay out of the public headers
target_link_libraries(engine_assets PUBLIC mir::core PRIVATE mir::project mir::jobs)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

/**
 * @file ModelAnimation.tests.cpp
 * @author Jackey Cen
 * @brief Tests for animation/ModelAnimation and the ModelPlayer field table.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <animation/ModelAnimation.h>
#include <animation/ModelPlayer.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <unordered_map>
using namespace mir;
TEST_CASE("Mirrored skinning preserves the normal-map tangent handedness", "[animation][pbr]")
{
    MeshPrimitive mesh;
    mesh.vertices.resize(1);
    mesh.vertices[0].position = {1, 0, 0};
    mesh.vertices[0].normal = {0, 0, 1};
    mesh.vertices[0].tangent = {1, 0, 0, 1};
    mesh.influenceRanges = {{0, 1}};
    mesh.influences = {{0, 1}};
    glm::mat4 joint(1);
    joint[0][0] = -2;
    joint[1][1] = 3;
    const auto result = DeformPrimitive(mesh, {}, std::span(&joint, 1));
    REQUIRE(result.size() == 1);
    CHECK(result[0].position.x == Catch::Approx(-2));
    CHECK(result[0].normal.z == Catch::Approx(1));
    CHECK(result[0].tangent.x == Catch::Approx(-1));
    CHECK(result[0].tangent.w == -1);
}

TEST_CASE("ModelPlayer scene fields omit transient preview state", "[animation]")
{
    RegisterModelPlayer();
    const ComponentOps *ops = ComponentRegistry::Instance().Find("ModelPlayer");
    REQUIRE(ops != nullptr);
    CHECK(ops->FindField("preview") == nullptr);
    CHECK(ops->FindField("modelAsset") != nullptr);
    CHECK(ops->FindField("animationAsset") != nullptr);
}
TEST_CASE("ModelPlayer fields go through validating setters", "[animation]")
{
    RegisterModelPlayer();
    const ComponentOps *ops = ComponentRegistry::Instance().Find("ModelPlayer");
    REQUIRE(ops != nullptr);
    ModelPlayer player;

    char text[kFieldStringCapacity] = "00000000000000ab";
    ops->FindField("modelAsset")->Write(&player, text);
    CHECK(player.ModelAsset().value == 0xab);
    char out[kFieldStringCapacity]{};
    ops->FindField("modelAsset")->Read(&player, out);
    CHECK(std::string_view(out) == "00000000000000ab");

    char garbage[kFieldStringCapacity] = "not-hex";
    ops->FindField("modelAsset")->Write(&player, garbage);
    CHECK(player.ModelAsset().IsNull());

    const float nan = std::numeric_limits<float>::quiet_NaN();
    player.SetTime(2);
    ops->FindField("time")->Write(&player, &nan);
    CHECK(player.Time() == 2);
    player.SetClip(-3);
    CHECK(player.Clip() == 0);
    player.SetScene(-5);
    CHECK(player.Scene() == -1);
}
TEST_CASE("Animation sampling respects step linear and cubic time scaling", "[animation]")
{
    AnimationTrack t;
    t.target = "node";
    t.path = AnimationPath::Weights;
    t.components = 1;
    t.times = {0, 2};
    t.values = {0, 2};
    CHECK(SampleAnimationTrack(t, 1)[0] == Catch::Approx(1));
    t.interpolation = AnimationInterpolation::Step;
    CHECK(SampleAnimationTrack(t, 1)[0] == 0);
    CHECK(SampleAnimationTrack(t, 2)[0] == 2);
    t.interpolation = AnimationInterpolation::CubicSpline;
    t.values = {0, 0, 1, 1, 2, 0};
    CHECK(SampleAnimationTrack(t, 1)[0] == Catch::Approx(1));
    CHECK(SampleAnimationTrack(t, -1)[0] == 0);
    CHECK(SampleAnimationTrack(t, 3)[0] == 2);
}
TEST_CASE("Morph is applied before skin and instances do not share weights", "[animation]")
{
    MeshPrimitive p;
    p.vertices.resize(1);
    p.vertices[0].position = {1, 0, 0};
    p.morphTargets.push_back({{{1, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}});
    p.influenceRanges = {{0, 1}};
    p.influences = {{0, 1}};
    glm::mat4 joint(1);
    joint[0][0] = 2;
    float half = .5f, zero = 0;
    auto a = DeformPrimitive(p, {&half, 1}, {&joint, 1}), b = DeformPrimitive(p, {&zero, 1}, {&joint, 1});
    CHECK(a[0].position.x == Catch::Approx(3));
    CHECK(b[0].position.x == Catch::Approx(2));
    CHECK(p.vertices[0].position.x == 1);
}
TEST_CASE("Typed mesh rejects changed content version and invalid indices", "[assets][animation]")
{
    MeshAsset m;
    MeshPrimitive p;
    p.vertices.resize(3);
    p.indices = {0, 1, 2};
    m.primitives.push_back(p);
    auto bytes = BuildTypedAsset(m);
    auto view = ParseAssetBlob(bytes);
    REQUIRE(view);
    MeshAsset decoded;
    std::string error;
    REQUIRE(ReadTypedAsset(*view, decoded, error));
    view->header.contentVersion++;
    CHECK_FALSE(ReadTypedAsset(*view, decoded, error));
    m.primitives[0].indices[2] = 3;
    CHECK_FALSE(ValidateAsset(m, error));
}
TEST_CASE("Cubic animation uses tangents and quaternion linear takes shortest path", "[animation]")
{
    AnimationTrack t;
    t.target = "node";
    t.path = AnimationPath::Weights;
    t.components = 1;
    t.times = {0, 2};
    t.interpolation = AnimationInterpolation::CubicSpline;
    t.values = {0, 0, 2, 0, 0, 0};
    CHECK(SampleAnimationTrack(t, 1)[0] == Catch::Approx(.5));
    t.path = AnimationPath::Rotation;
    t.components = 4;
    t.interpolation = AnimationInterpolation::Linear;
    t.values = {0, 0, 0, 1, 0, 0, 0, -1};
    auto q = SampleAnimationTrack(t, 1);
    CHECK(std::abs(q[3]) == Catch::Approx(1));
    CHECK(q[0] == 0);
}
TEST_CASE("Model seek loop and hierarchy evaluation are deterministic", "[animation]")
{
    ModelResources r;
    ModelAsset asset;
    asset.nodes.resize(2);
    asset.nodes[0].key = "root";
    asset.nodes[0].translation = {3, 0, 0};
    asset.nodes[1].key = "child";
    asset.nodes[1].parent = 0;
    asset.nodes[1].translation = {0, 2, 0};
    r.model = std::make_shared<ModelAsset>(asset);
    AnimationTrack t;
    t.target = "root";
    t.path = AnimationPath::Translation;
    t.times = {0, 2};
    t.values = {3, 0, 0, 5, 0, 0};
    AnimationClipAsset clip;
    clip.duration = 2;
    clip.tracks = {t};
    r.clips = {std::make_shared<AnimationClipAsset>(clip)};
    auto a = EvaluateModel(r, 0, 1, false), b = EvaluateModel(r, 0, 3, true), c = EvaluateModel(r, 0, 0, false);
    CHECK(a[1].world[3].x == 4);
    CHECK(a[1].world[3].y == 2);
    CHECK(b[1].world[3].x == 4);
    CHECK(c[1].world[3].x == 3);
    CHECK(a[1].world[3].x == 4);
}
TEST_CASE("Typed assets reject truncation oversized arrays trailing bytes and cyclic nodes", "[assets][animation]")
{
    TextureAsset t;
    t.width = t.height = 1;
    t.format = TextureFormat::Rgba8Srgb;
    t.colorSpace = TextureColorSpace::Srgb;
    t.rgba = {255, 0, 0, 255};
    auto bytes = BuildTypedAsset(t);
    auto view = ParseAssetBlob(bytes);
    REQUIRE(view);
    TextureAsset result;
    std::string error;
    auto truncated = *view;
    truncated.content = truncated.content.first(4);
    CHECK_FALSE(ReadTypedAsset(truncated, result, error));
    std::vector<std::byte> content(view->content.begin(), view->content.end());
    content[8] = std::byte{255};
    content[9] = std::byte{255};
    content[10] = std::byte{255};
    content[11] = std::byte{255};
    auto corrupt = *view;
    corrupt.content = content;
    CHECK_FALSE(ReadTypedAsset(corrupt, result, error));
    content.assign(view->content.begin(), view->content.end());
    content.push_back(std::byte{0});
    corrupt.content = content;
    CHECK_FALSE(ReadTypedAsset(corrupt, result, error));
    ModelAsset m;
    m.nodes.resize(1);
    m.nodes[0].key = "cycle";
    m.nodes[0].parent = 0;
    CHECK_FALSE(ValidateAsset(m, error));
}

namespace
{
    // Blobs by id, as a worker would hold them after reading a model's files
    struct BlobSet
    {
        std::unordered_map<uint64_t, std::vector<std::byte>> bytes;
        std::unordered_map<uint64_t, AssetBlobView> views;

        void Add(AssetId id, std::vector<std::byte> blob)
        {
            auto &stored = bytes[id.value] = std::move(blob);
            views[id.value] = *ParseAssetBlob(stored);
        }
        AssetDependencies Lookup() const
        {
            return AssetDependencies([this](AssetId id) -> const AssetBlobView * {
                const auto found = views.find(id.value);
                return found == views.end() ? nullptr : &found->second;
            });
        }
    };

    struct ModelBlobs
    {
        AssetId model = MakeAssetId("model"), mesh = MakeAssetId("mesh"), clip = MakeAssetId("clip");
        BlobSet blobs;

        ModelBlobs()
        {
            ModelAsset asset;
            asset.nodes.resize(1);
            asset.nodes[0].key = "root";
            asset.nodes[0].mesh = mesh;
            asset.scenes = {{"scene", {0}}};
            asset.animations = {clip};
            blobs.Add(model, BuildTypedAsset(asset));

            MeshAsset geometry;
            MeshPrimitive primitive;
            primitive.vertices.resize(3);
            primitive.indices = {0, 1, 2};
            geometry.primitives.push_back(primitive);
            blobs.Add(mesh, BuildTypedAsset(geometry));

            AnimationTrack track;
            track.target = "root";
            track.path = AnimationPath::Translation;
            track.times = {0, 1};
            track.values = {0, 0, 0, 1, 0, 0};
            AnimationClipAsset animation;
            animation.duration = 1;
            animation.tracks = {track};
            blobs.Add(clip, BuildTypedAsset(animation));
        }
    };
}

TEST_CASE("Model resources decode from a root blob and its dependency blobs", "[animation][assets]")
{
    ModelBlobs set;
    ModelResources resources;
    std::string error;
    const AssetDependencies lookup = set.blobs.Lookup();
    REQUIRE(DecodeModelResources(set.blobs.views.at(set.model.value), lookup, resources, error));
    CHECK(resources.model->nodes.size() == 1);
    CHECK(resources.meshes.contains(set.mesh.value));
    CHECK(resources.clips.size() == 1);
    const MeshRecord &mesh = *resources.meshes.at(set.mesh.value);
    REQUIRE(mesh.uploads.size() == 1);
    REQUIRE(mesh.uploads[0] != nullptr);
    CHECK(mesh.uploads[0]->vertices.size() == 3);
    CHECK(mesh.uploads[0]->indices.size() == 3);
}

TEST_CASE("Model resources fail when a dependency blob is not available", "[animation][assets]")
{
    ModelBlobs set;
    ModelResources resources;
    std::string error;

    SECTION("the mesh")
    {
        set.blobs.views.erase(set.mesh.value);
    }
    SECTION("the clip")
    {
        set.blobs.views.erase(set.clip.value);
    }
    const AssetDependencies lookup = set.blobs.Lookup();
    CHECK_FALSE(DecodeModelResources(set.blobs.views.at(set.model.value), lookup, resources, error));
    CHECK(error == "no cooked blob");
    CHECK(resources.meshes.empty());
}

TEST_CASE("Each record kind decodes its own blob alone and has one shared descriptor", "[animation][assets]")
{
    // no dependency types: a mesh shared by two models is one request and one decoded record
    CHECK(ModelRecordDecoder().dependencyTags.empty());
    CHECK(MeshRecordDecoder().dependencyTags.empty());
    CHECK(SkeletonRecordDecoder().dependencyTags.empty());
    CHECK(ClipRecordDecoder().dependencyTags.empty());
    CHECK(MaterialRecordDecoder().dependencyTags.empty());
    CHECK(&MeshRecordDecoder() == &MeshRecordDecoder());

    ModelBlobs set;
    MeshRecord record;
    std::string error;
    const AssetDependencies lookup = set.blobs.Lookup();
    REQUIRE(MeshRecordDecoder().decode(set.blobs.views.at(set.mesh.value), lookup, record, error));
    CHECK(record.asset.primitives.size() == 1);
    REQUIRE(record.uploads.size() == 1);
    CHECK(record.uploads[0] != nullptr);
}

TEST_CASE("A model names each record once, and its materials come from its meshes", "[animation][assets]")
{
    ModelAsset model;
    model.nodes.resize(3);
    const AssetId meshA = MakeAssetId("mesh-a"), meshB = MakeAssetId("mesh-b"), skeleton = MakeAssetId("skeleton"),
                  clip = MakeAssetId("clip");
    model.nodes[0].mesh = meshA;
    model.nodes[1].mesh = meshA;
    model.nodes[2].mesh = meshB;
    model.skins.resize(2);
    model.skins[0].skeleton = skeleton;
    model.skins[1].skeleton = skeleton;
    model.animations = {clip, clip};
    const ModelDependencies needed = DependenciesOf(model);
    CHECK(needed.meshes == std::vector<AssetId>{meshA, meshB});
    CHECK(needed.skeletons == std::vector<AssetId>{skeleton});
    CHECK(needed.clips == std::vector<AssetId>{clip});

    MeshAsset mesh;
    mesh.primitives.resize(3);
    const AssetId material = MakeAssetId("material");
    mesh.primitives[0].material = material;
    mesh.primitives[2].material = material;
    CHECK(MaterialsOf(mesh) == std::vector<AssetId>{material});
}

TEST_CASE("A record reports every dynamic allocation it owns", "[animation][assets]")
{
    const auto bytes = [](const MeshRecord &record) { return MeshRecordDecoder().sizeBytes(record); };

    MeshRecord small;
    small.asset.primitives.resize(1);
    small.asset.primitives[0].morphTargets.resize(2);
    MeshRecord large = small;
    for (MorphTarget &target : large.asset.primitives[0].morphTargets)
    {
        target.positions.assign(1000, glm::vec3(1));
        target.normals.assign(1000, glm::vec3(1));
        target.tangents.assign(1000, glm::vec3(1));
    }
    CHECK(bytes(large) - bytes(small) >= 2 * 3 * 1000 * sizeof(glm::vec3));

    MeshRecord uploaded = small;
    auto decoded = std::make_shared<DecodedMesh>();
    decoded->vertices.resize(100);
    decoded->indices.resize(300);
    uploaded.uploads.push_back(decoded);
    CHECK(bytes(uploaded) - bytes(small) >= 100 * sizeof(Vertex) + 300 * sizeof(uint32_t));

    SkeletonAsset skeleton;
    skeleton.nodes.resize(4);
    const uint64_t before = SkeletonRecordDecoder().sizeBytes(skeleton);
    for (ModelNode &node : skeleton.nodes)
    {
        node.key = std::string(500, 'k');
        node.name = std::string(500, 'n');
        node.weights.assign(64, 0.0f);
    }
    // each node's two strings and weights, less the small buffers they already had
    CHECK(SkeletonRecordDecoder().sizeBytes(skeleton) - before >= 4 * (900 + 64 * sizeof(float)));

    ModelAsset model;
    model.nodes.resize(2);
    model.skins.resize(1);
    const uint64_t modelBefore = ModelRecordDecoder().sizeBytes(model);
    model.skins[0].jointKeys.assign(10, std::string(200, 'j'));
    model.skins[0].inverseBindMatrices.assign(10, glm::mat4(1));
    model.nodes[0].key = std::string(300, 'x');
    CHECK(ModelRecordDecoder().sizeBytes(model) - modelBefore >= 10 * 200 + 10 * sizeof(glm::mat4) + 300);

    AnimationClipAsset clip;
    clip.tracks.resize(1);
    const uint64_t clipBefore = ClipRecordDecoder().sizeBytes(clip);
    clip.tracks[0].times.assign(1000, 0.0f);
    clip.tracks[0].values.assign(3000, 0.0f);
    CHECK(ClipRecordDecoder().sizeBytes(clip) - clipBefore >= 4000 * sizeof(float));
}

TEST_CASE("A record does not count a string's inline buffer twice", "[animation][assets]")
{
    ModelAsset model;
    model.nodes.resize(1000);
    const uint64_t owned = sizeof(ModelAsset) + model.nodes.capacity() * sizeof(ModelNode);
    CHECK(ModelRecordDecoder().sizeBytes(model) == owned);
    // a string that outgrew its inline buffer is counted once, by its heap block
    model.nodes[0].key = std::string(500, 'k');
    CHECK(ModelRecordDecoder().sizeBytes(model) >= owned + 500);
    CHECK(ModelRecordDecoder().sizeBytes(model) <= owned + 600);
}

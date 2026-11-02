/**
 * @file ModelAnimation.cpp
 * @author Jackey Cen
 * @brief Model dependency validation, track interpolation, hierarchical pose and CPU/GPU deformation data.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <algorithm>
#include <animation/ModelAnimation.h>
#include <cmath>
#include <functional>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace mir
{
    namespace
    {
        std::shared_ptr<const DecodedMesh> ToUpload(const MeshPrimitive &primitive)
        {
            if (primitive.vertices.empty() || primitive.indices.empty())
                return nullptr;
            auto decoded = std::make_shared<DecodedMesh>();
            decoded->vertices.reserve(primitive.vertices.size());
            for (const auto &v : primitive.vertices)
                decoded->vertices.push_back({ v.position, glm::vec3(v.color), v.normal, v.uv, v.color.a, v.tangent });
            decoded->indices = primitive.indices;
            decoded->bounds = ComputeMeshBounds(decoded->vertices);
            return decoded;
        }

        // Every owned allocation, by capacity. A shared allocation belongs to one record and is counted there.
        uint64_t StringBytes(const std::string &value) { return value.capacity(); }
        template <class T> uint64_t VectorBytes(const std::vector<T> &values) { return values.capacity() * sizeof(T); }

        uint64_t NodeBytes(const ModelNode &node)
        {
            return StringBytes(node.key) + StringBytes(node.name) + VectorBytes(node.weights);
        }

        uint64_t PrimitiveBytes(const MeshPrimitive &p)
        {
            uint64_t bytes = VectorBytes(p.vertices) + VectorBytes(p.indices) + VectorBytes(p.influenceRanges) +
                VectorBytes(p.influences) + VectorBytes(p.morphTargets);
            for (const MorphTarget &target : p.morphTargets)
                bytes += VectorBytes(target.positions) + VectorBytes(target.normals) + VectorBytes(target.tangents);
            return bytes;
        }

        uint64_t ModelBytes(const ModelAsset &model)
        {
            uint64_t bytes = sizeof(model) + VectorBytes(model.nodes) + VectorBytes(model.scenes) +
                VectorBytes(model.skins) + VectorBytes(model.animations);
            for (const ModelNode &node : model.nodes)
                bytes += NodeBytes(node);
            for (const ModelScene &scene : model.scenes)
                bytes += StringBytes(scene.name) + VectorBytes(scene.roots);
            for (const SkinBinding &skin : model.skins)
            {
                bytes += VectorBytes(skin.jointKeys) + VectorBytes(skin.inverseBindMatrices);
                for (const std::string &key : skin.jointKeys)
                    bytes += StringBytes(key);
            }
            return bytes;
        }

        uint64_t SkeletonBytes(const SkeletonAsset &skeleton)
        {
            uint64_t bytes = sizeof(skeleton) + StringBytes(skeleton.name) + VectorBytes(skeleton.nodes) +
                VectorBytes(skeleton.joints);
            for (const ModelNode &node : skeleton.nodes)
                bytes += NodeBytes(node);
            return bytes;
        }

        uint64_t ClipBytes(const AnimationClipAsset &clip)
        {
            uint64_t bytes = sizeof(clip) + StringBytes(clip.name) + VectorBytes(clip.tracks);
            for (const AnimationTrack &track : clip.tracks)
                bytes += StringBytes(track.target) + VectorBytes(track.times) + VectorBytes(track.values);
            return bytes;
        }

        uint64_t MeshRecordBytes(const MeshRecord &record)
        {
            uint64_t bytes = sizeof(record) + StringBytes(record.asset.name) + VectorBytes(record.asset.weights) +
                VectorBytes(record.asset.primitives) + VectorBytes(record.uploads);
            for (const MeshPrimitive &primitive : record.asset.primitives)
                bytes += PrimitiveBytes(primitive);
            for (const auto &upload : record.uploads)
                if (upload)
                    bytes += DecodedMeshBytes(*upload);
            return bytes;
        }

        uint64_t MaterialBytes(const MaterialAsset &material) { return sizeof(material); }

        template <class T>
        bool DecodeTyped(const AssetBlobView &root, const AssetDependencies &, T &out, std::string &error)
        {
            return ReadTypedAsset(root, out, error);
        }

        bool DecodeMeshRecord(const AssetBlobView &root, const AssetDependencies &, MeshRecord &out, std::string &error)
        {
            if (!ReadTypedAsset(root, out.asset, error))
                return false;
            out.uploads.reserve(out.asset.primitives.size());
            for (const MeshPrimitive &primitive : out.asset.primitives)
                out.uploads.push_back(ToUpload(primitive));
            return true;
        }
    }

    const AssetDecoder<ModelAsset> &ModelRecordDecoder()
    {
        static constexpr AssetDecoder<ModelAsset> kDecoder{ DecodeTyped<ModelAsset>, {}, ModelBytes };
        return kDecoder;
    }
    const AssetDecoder<MeshRecord> &MeshRecordDecoder()
    {
        static constexpr AssetDecoder<MeshRecord> kDecoder{ DecodeMeshRecord, {}, MeshRecordBytes };
        return kDecoder;
    }
    const AssetDecoder<SkeletonAsset> &SkeletonRecordDecoder()
    {
        static constexpr AssetDecoder<SkeletonAsset> kDecoder{ DecodeTyped<SkeletonAsset>, {}, SkeletonBytes };
        return kDecoder;
    }
    const AssetDecoder<AnimationClipAsset> &ClipRecordDecoder()
    {
        static constexpr AssetDecoder<AnimationClipAsset> kDecoder{ DecodeTyped<AnimationClipAsset>, {}, ClipBytes };
        return kDecoder;
    }
    const AssetDecoder<MaterialAsset> &MaterialRecordDecoder()
    {
        static constexpr AssetDecoder<MaterialAsset> kDecoder{ DecodeTyped<MaterialAsset>, {}, MaterialBytes };
        return kDecoder;
    }

    ModelDependencies DependenciesOf(const ModelAsset &model)
    {
        ModelDependencies out;
        const auto add = [](std::vector<AssetId> &ids, AssetId id)
        {
            if (!id.IsNull() && std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
        };
        for (const ModelNode &node : model.nodes)
            add(out.meshes, node.mesh);
        for (const SkinBinding &skin : model.skins)
            add(out.skeletons, skin.skeleton);
        for (const AssetId clip : model.animations)
            add(out.clips, clip);
        return out;
    }

    std::vector<AssetId> MaterialsOf(const MeshAsset &mesh)
    {
        std::vector<AssetId> ids;
        for (const MeshPrimitive &primitive : mesh.primitives)
            if (!primitive.material.IsNull() && std::find(ids.begin(), ids.end(), primitive.material) == ids.end())
                ids.push_back(primitive.material);
        return ids;
    }

    bool ValidateModelResources(const ModelResources &data, std::string &error)
    {
        for (const auto &skin : data.model->skins)
        {
            const auto found = data.skeletons.find(skin.skeleton.value);
            if (found == data.skeletons.end())
            {
                error = "no cooked blob";
                return false;
            }
            const SkeletonAsset &skeleton = *found->second;
            if (skeleton.joints.size() != skin.jointKeys.size())
            {
                error = "skeleton palette count mismatch";
                return false;
            }
            for (size_t i = 0; i < skin.jointKeys.size(); ++i)
                if (skeleton.nodes[skeleton.joints[i]].key != skin.jointKeys[i])
                {
                    error = "skeleton joint mapping mismatch";
                    return false;
                }
        }
        for (const auto &node : data.model->nodes)
            if (!node.mesh.IsNull())
            {
                const auto found = data.meshes.find(node.mesh.value);
                if (found == data.meshes.end())
                {
                    error = "no cooked blob";
                    return false;
                }
                const MeshAsset &mesh = found->second->asset;
                if (node.weights.size() != mesh.weights.size())
                {
                    error = "node morph weights mismatch";
                    return false;
                }
                if (node.skin != kNoAssetIndex)
                    for (const auto &p : mesh.primitives)
                    {
                        if (p.influenceRanges.empty())
                        {
                            error = "skin has no vertex influences";
                            return false;
                        }
                        for (auto influence : p.influences)
                            if (influence.joint >= data.model->skins[node.skin].jointKeys.size())
                            {
                                error = "joint out of palette range";
                                return false;
                            }
                    }
            }
        if (data.clips.size() != data.model->animations.size())
        {
            error = "no cooked blob";
            return false;
        }
        for (const auto &clip : data.clips)
            for (const auto &track : clip->tracks)
            {
                auto node = std::find_if(data.model->nodes.begin(), data.model->nodes.end(),
                                         [&](const auto &n) { return n.key == track.target; });
                if (node == data.model->nodes.end() ||
                    (track.path == AnimationPath::Weights ? node->weights.size() != track.components : node->hasMatrix))
                {
                    error = "incompatible animation target";
                    return false;
                }
            }
        return true;
    }

    bool DecodeModelResources(const AssetBlobView &root, const AssetDependencies &dependencies, ModelResources &result,
                              std::string &error)
    {
        ModelResources data;
        auto modelRecord = std::make_shared<ModelAsset>();
        if (!ReadTypedAsset(root, *modelRecord, error))
            return false;
        data.model = std::move(modelRecord);
        // a record the model needs, read from its own blob
        const auto read = [&](AssetId key, auto &out, std::string &problem)
        {
            const AssetBlobView *blob = dependencies.Find(key);
            if (blob == nullptr)
            {
                problem = "no cooked blob";
                return false;
            }
            return ReadTypedAsset(*blob, out, problem);
        };
        const ModelDependencies needed = DependenciesOf(*data.model);
        for (const AssetId id : needed.meshes)
        {
            auto record = std::make_shared<MeshRecord>();
            const AssetBlobView *blob = dependencies.Find(id);
            if (blob == nullptr)
            {
                error = "no cooked blob";
                return false;
            }
            if (!DecodeMeshRecord(*blob, dependencies, *record, error))
                return false;
            for (const AssetId materialId : MaterialsOf(record->asset))
                if (!data.materials.contains(materialId.value))
                {
                    auto material = std::make_shared<MaterialAsset>();
                    std::string ignored;
                    if (read(materialId, *material, ignored))
                        data.materials.emplace(materialId.value, std::move(material));
                }
            data.meshes.emplace(id.value, std::move(record));
        }
        for (const AssetId id : needed.skeletons)
        {
            auto skeleton = std::make_shared<SkeletonAsset>();
            if (!read(id, *skeleton, error))
                return false;
            data.skeletons.emplace(id.value, std::move(skeleton));
        }
        for (const AssetId id : data.model->animations)
        {
            auto clip = std::make_shared<AnimationClipAsset>();
            if (!read(id, *clip, error))
                return false;
            data.clips.push_back(std::move(clip));
        }
        if (!ValidateModelResources(data, error))
            return false;
        result = std::move(data);
        return true;
    }
    bool LoadModelResources(AssetCache &cache, AssetId id, ModelResources &result, std::string &error)
    {
        const AssetBlobView *root = cache.Load(id);
        if (root == nullptr)
        {
            error = "no cooked blob";
            return false;
        }
        const AssetDependencies dependencies([&cache](AssetId key) { return cache.Load(key); });
        return DecodeModelResources(*root, dependencies, result, error);
    }
    std::vector<float> SampleAnimationTrack(const AnimationTrack &t, float time)
    {
        if (t.times.empty())
            return {};
        const size_t n = t.components, stride = n * (t.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1);
        auto value = [&](size_t key, size_t component) {
            return t.values[key * stride + (stride == n ? 0 : n) + component];
        };
        const auto upper = std::upper_bound(t.times.begin(), t.times.end(), time);
        size_t hi = std::min(size_t(upper - t.times.begin()), t.times.size() - 1), lo = hi ? hi - 1 : 0;
        if (time <= t.times.front())
            lo = hi = 0;
        if (time >= t.times.back())
            lo = hi = t.times.size() - 1;
        const float dt = t.times[hi] - t.times[lo], u = dt > 0 ? std::clamp((time - t.times[lo]) / dt, 0.0f, 1.0f) : 0;
        std::vector<float> out(n);
        if (t.interpolation == AnimationInterpolation::Linear && t.path == AnimationPath::Rotation && lo != hi)
        {
            const glm::quat a(value(lo, 3), value(lo, 0), value(lo, 1), value(lo, 2)),
                b(value(hi, 3), value(hi, 0), value(hi, 1), value(hi, 2));
            const auto q = glm::normalize(glm::slerp(a, b, u));
            out = {q.x, q.y, q.z, q.w};
            return out;
        }
        for (size_t c = 0; c < n; ++c)
        {
            const float a = value(lo, c), b = value(hi, c);
            if (lo == hi || t.interpolation == AnimationInterpolation::Step)
                out[c] = a;
            else if (t.interpolation == AnimationInterpolation::Linear)
                out[c] = a + (b - a) * u;
            else
            {
                const float u2 = u * u, u3 = u2 * u;
                out[c] = (2 * u3 - 3 * u2 + 1) * a + (u3 - 2 * u2 + u) * dt * t.values[lo * stride + 2 * n + c] +
                         (-2 * u3 + 3 * u2) * b + (u3 - u2) * dt * t.values[hi * stride + c];
            }
        }
        if (t.path == AnimationPath::Rotation)
        {
            glm::quat q(out[3], out[0], out[1], out[2]);
            const float len = glm::length(q);
            if (len < 1e-8f)
                throw std::runtime_error("animation produced zero quaternion");
            q /= len;
            out = {q.x, q.y, q.z, q.w};
        }
        return out;
    }
    void ResolvePoseWorld(const ModelAsset &model, std::vector<NodePose> &pose)
    {
        std::vector<bool> done(pose.size());
        std::function<void(size_t)> resolve = [&](size_t i) {
            if (done[i])
                return;
            const auto &node = model.nodes[i];
            auto &p = pose[i];
            p.world = node.hasMatrix ? node.matrix
                                     : glm::translate(glm::mat4(1), p.translation) * glm::mat4_cast(p.rotation) *
                                           glm::scale(glm::mat4(1), p.scale);
            if (node.parent != kNoAssetIndex)
            {
                resolve(node.parent);
                p.world = pose[node.parent].world * p.world;
            }
            done[i] = true;
        };
        for (size_t i = 0; i < pose.size(); ++i)
            resolve(i);
    }
    std::vector<NodePose> EvaluateModel(const ModelResources &data, uint32_t clipIndex, float time, bool loop)
    {
        const auto &model = *data.model;
        std::vector<NodePose> pose;
        for (const auto &n : model.nodes)
            pose.push_back({n.translation, n.scale, n.rotation, glm::mat4(1), n.weights});
        if (clipIndex < data.clips.size())
        {
            const auto &clip = *data.clips[clipIndex];
            if (loop && clip.duration > 0)
            {
                time = std::fmod(time, clip.duration);
                if (time < 0)
                    time += clip.duration;
            }
            else
                time = std::clamp(time, 0.0f, clip.duration);
            for (const auto &track : clip.tracks)
            {
                auto node = std::find_if(model.nodes.begin(), model.nodes.end(),
                                         [&](const auto &n) { return n.key == track.target; });
                if (node == model.nodes.end())
                    throw std::runtime_error("missing animation target");
                auto &p = pose[size_t(node - model.nodes.begin())];
                auto value = SampleAnimationTrack(track, time);
                switch (track.path)
                {
                case AnimationPath::Translation:
                    p.translation = {value[0], value[1], value[2]};
                    break;
                case AnimationPath::Scale:
                    p.scale = {value[0], value[1], value[2]};
                    break;
                case AnimationPath::Rotation:
                    p.rotation = {value[3], value[0], value[1], value[2]};
                    break;
                case AnimationPath::Weights:
                    p.weights = std::move(value);
                    break;
                }
            }
        }
        ResolvePoseWorld(model, pose);
        return pose;
    }
    std::vector<glm::mat4> SkinMatrices(const ModelAsset &model, const std::vector<NodePose> &pose, uint32_t node)
    {
        const auto skin = model.nodes[node].skin;
        if (skin == kNoAssetIndex)
            return {};
        const auto &binding = model.skins[skin];
        std::vector<glm::mat4> matrices;
        if (std::abs(glm::determinant(pose[node].world)) < 1e-10f)
            throw std::runtime_error("singular skinned node transform");
        const auto inverseMesh = glm::inverse(pose[node].world);
        for (size_t j = 0; j < binding.jointKeys.size(); ++j)
        {
            auto target = std::find_if(model.nodes.begin(), model.nodes.end(),
                                       [&](const auto &n) { return n.key == binding.jointKeys[j]; });
            if (target == model.nodes.end())
                throw std::runtime_error("missing joint node");
            matrices.push_back(inverseMesh * pose[size_t(target - model.nodes.begin())].world *
                               binding.inverseBindMatrices[j]);
        }
        return matrices;
    }
    std::vector<bool> VisibleModelNodes(const ModelAsset &model, uint32_t scene)
    {
        if (scene >= model.scenes.size())
            scene = model.defaultScene;
        std::vector<bool> visible(model.nodes.size());
        for (size_t i = 0; i < model.nodes.size(); ++i)
        {
            size_t root = i;
            while (model.nodes[root].parent != kNoAssetIndex)
                root = model.nodes[root].parent;
            visible[i] = std::find(model.scenes[scene].roots.begin(), model.scenes[scene].roots.end(), root) !=
                         model.scenes[scene].roots.end();
        }
        return visible;
    }
    std::vector<AssetVertex> DeformPrimitive(const MeshPrimitive &p, std::span<const float> weights,
                                             std::span<const glm::mat4> joints)
    {
        auto out = p.vertices;
        for (size_t i = 0; i < out.size(); ++i)
        {
            auto &v = out[i];
            auto tangent = glm::vec3(v.tangent);
            for (size_t t = 0; t < p.morphTargets.size(); ++t)
            {
                v.position += weights[t] * p.morphTargets[t].positions[i];
                v.normal += weights[t] * p.morphTargets[t].normals[i];
                tangent += weights[t] * p.morphTargets[t].tangents[i];
            }
            if (!joints.empty())
            {
                glm::mat4 m(0);
                const auto range = p.influenceRanges[i];
                for (size_t k = range.offset; k < size_t(range.offset) + range.count; ++k)
                    m += p.influences[k].weight * joints[p.influences[k].joint];
                v.position = glm::vec3(m * glm::vec4(v.position, 1));
                if (std::abs(glm::determinant(glm::mat3(m))) > 1e-10f)
                    v.normal = glm::inverseTranspose(glm::mat3(m)) * v.normal;
                tangent = glm::mat3(m) * tangent;
            }
            if (glm::dot(v.normal, v.normal) > 1e-20f)
                v.normal = glm::normalize(v.normal);
            tangent -= v.normal * glm::dot(v.normal, tangent);
            if (glm::dot(tangent, tangent) > 1e-20f)
                tangent = glm::normalize(tangent);
            v.tangent = glm::vec4(tangent, v.tangent.w);
        }
        return out;
    }
    std::vector<glm::vec4> BuildDeformationData(const MeshPrimitive &p, std::span<const float> weights,
                                                std::span<const glm::mat4> joints)
    {
        if (p.morphTargets.empty() && joints.empty())
            return {};
        std::vector<glm::vec4> data(3, glm::vec4(0));
        auto offset = [&]() {
            if (data.size() >= 16777216)
                throw std::runtime_error("GPU deformation data exceeds exact float indexing limit");
            return float(data.size());
        };
        data[0].x = float(p.vertices.size());
        data[0].y = float(p.morphTargets.size());
        data[1].z = offset();
        for (float weight : weights)
            data.emplace_back(weight, 0, 0, 0);
        data[1].y = offset();
        for (const auto &m : p.morphTargets)
            for (size_t i = 0; i < p.vertices.size(); ++i)
            {
                data.emplace_back(m.positions[i], 0);
                data.emplace_back(m.normals[i], 0);
                data.emplace_back(m.tangents[i], 0);
            }
        data[1].x = offset();
        if (!joints.empty())
            for (auto r : p.influenceRanges)
                data.emplace_back(float(r.offset), float(r.count), 0, 0);
        else
            for (size_t i = 0; i < p.vertices.size(); ++i)
                data.emplace_back(0.0f);
        data[0].w = offset();
        for (auto influence : p.influences)
            data.emplace_back(float(influence.joint), influence.weight, 0, 0);
        data[0].z = offset();
        for (const auto &m : joints)
            for (int c = 0; c < 4; ++c)
                data.push_back(m[c]);
        offset();
        return data;
    }
} // namespace mir

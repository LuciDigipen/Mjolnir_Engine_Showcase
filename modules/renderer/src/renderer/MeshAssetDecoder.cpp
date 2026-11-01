/**
 * @file MeshAssetDecoder.cpp
 * @author sumin.park
 * @brief Decodes a cooked mesh JSON asset into vertices, indices and bounds, safe to run on a worker.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshAssetDecoder.h>

#include <assets/AssetBlob.h>

#include <glm/common.hpp>
#include <glm/vec3.hpp>

#include <nlohmann/json.hpp>

#include <limits>
#include <string_view>
#include <utility>

namespace mir
{
    MeshBounds ComputeMeshBounds(std::span<const Vertex> vertices)
    {
        glm::vec3 min(std::numeric_limits<float>::max());
        glm::vec3 max(std::numeric_limits<float>::lowest());

        for (const Vertex &vertex : vertices)
        {
            min = glm::min(min, vertex.pos);
            max = glm::max(max, vertex.pos);
        }

        if (vertices.empty())
            min = max = glm::vec3(0.0f);

        return MeshBounds{ min, max };
    }

    bool DecodeMeshAsset(const AssetBlobView &root, const AssetDependencies &, DecodedMesh &out, std::string &error)
    {
        try
        {
            const nlohmann::json json = nlohmann::json::parse(AsStringView(root));

            out.vertices.clear();
            out.indices.clear();
            for (const auto &v : json.value("vertices", nlohmann::json::array()))
            {
                Vertex vertex{};
                const auto &p = v.at("position");
                const auto &c = v.at("color");
                const auto &n = v.at("normal");
                vertex.pos = glm::vec3(p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>());
                vertex.color = glm::vec3(c.at(0).get<float>(), c.at(1).get<float>(), c.at(2).get<float>());
                vertex.normal = glm::vec3(n.at(0).get<float>(), n.at(1).get<float>(), n.at(2).get<float>());
                out.vertices.push_back(vertex);
            }
            out.indices = json.value("indices", std::vector<uint32_t>{});
        }
        catch (const nlohmann::json::exception &exception)
        {
            error = std::string("not a valid mesh: ") + exception.what();
            return false;
        }

        // an index past the vertices would read out of bounds on the GPU
        for (const uint32_t index : out.indices)
        {
            if (index >= out.vertices.size())
            {
                error = "index " + std::to_string(index) + " is past its " + std::to_string(out.vertices.size()) +
                    " vertices";
                return false;
            }
        }

        out.bounds = ComputeMeshBounds(out.vertices);
        return true;
    }
}

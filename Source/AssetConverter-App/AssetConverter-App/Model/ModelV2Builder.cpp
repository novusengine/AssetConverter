#include "ModelV2Builder.h"
#include "ModelV2Allocation.h"

#include "AssetConverter-App/Runtime.h"

#include <Base/Memory/Bytebuffer.h>
#include <Base/Util/DebugHandler.h>

#include <FileFormat/Novus/Model/Material.h>
#include <FileFormat/Novus/Model/Model.h>
#include <FileFormat/Shared.h>
#include <FileFormat/Warcraft/M2/M2.h>
#include <FileFormat/Warcraft/WMO/Wmo.h>

#include <meshoptimizer.h>
#include <xxhash/xxhash64.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    namespace CookSettings
    {
        // Each optimization is deliberately independent. Turning one off is a
        // useful first isolation step when diagnosing a renderer-side artifact.
        inline constexpr bool GenerateDiscreteLODs = true;
        inline constexpr bool GenerateTangents = true;
        inline constexpr bool OptimizeVertexCache = true;
        inline constexpr bool OptimizeOverdraw = true;
        inline constexpr bool OptimizeVertexFetch = true;
        inline constexpr bool OptimizeMeshlets = true;
        inline constexpr bool BuildLODsConcurrently = true;
        inline constexpr bool CookLargeWMOLODsConcurrently = true;
        inline constexpr bool DeferMaterialEmission = true;
        inline constexpr bool EnableDetailedProfiling = false;

        inline constexpr std::array<f32, 3> LODTriangleRatios = { 0.5f, 0.25f, 0.125f };
        inline constexpr u32 ConcurrentLODMinimumIndexCount = 30'000;
        inline constexpr u32 ConcurrentWMOCookMinimumIndexCount = 10'000;
        inline constexpr f32 LODTargetError = 0.02f;
        inline constexpr f32 OverdrawThreshold = 1.05f;
        inline constexpr f32 MeshletConeWeight = 0.5f;
    }

    namespace WowMaterialABI
    {
        inline constexpr u32 Version = 1;
        inline constexpr u32 ManifestSchemaVersion = 1;
        inline constexpr u32 MaxTextures = 8;
        inline constexpr u32 BaseColorOffset = 0;
        inline constexpr u32 TextureOffset = 16;
        inline constexpr u32 LegacyUnitOffset = 48;
        inline constexpr u32 FlagsOffset = 80;
        inline constexpr u32 AlphaCutoffOffset = 84;
        inline constexpr u32 ParameterBlockSize = 96;

        enum LightingModel : u16
        {
            Standard = 0,
            Unlit = 1
        };

        enum ExecutionGroup : u16
        {
            OpaqueSimple = 0,
            OpaqueLayered = 1,
            AlphaTestSimple = 2,
            AlphaTestLayered = 3,
            TransparentSimple = 4,
            TransparentLayered = 5
        };
    }

    namespace MeshoptimizerTiming
    {
        enum Counter : u32
        {
            Simplification,
            TangentGeneration,
            VertexRemap,
            VertexCache,
            Overdraw,
            VertexFetch,
            MeshletBuild,
            MeshletOptimization,
            MeshletBounds,
            SourcePreparation,
            BaseLODAssembly,
            GeneratedLODAssembly,
            MaterialProcessing,
            GeometryCooking,
            Serialization,
            PactWrite,
            Count
        };

        struct WorkerCounters;
        struct Registry
        {
            std::mutex mutex;
            std::vector<WorkerCounters*> workers;
        };

        // The registry intentionally outlives worker-thread TLS teardown. It is
        // tiny, process-scoped state and avoids static-destruction ordering
        // hazards when Enki's worker threads are joined during shutdown.
        Registry& GetRegistry()
        {
            static Registry* registry = new Registry();
            return *registry;
        }

        struct WorkerCounters
        {
            WorkerCounters()
            {
                Registry& registry = GetRegistry();
                std::scoped_lock lock(registry.mutex);
                registry.workers.push_back(this);
            }

            ~WorkerCounters()
            {
                Registry& registry = GetRegistry();
                std::scoped_lock lock(registry.mutex);
                std::erase(registry.workers, this);
            }

            std::array<u64, Count> values = {};
        };

        WorkerCounters& GetWorkerCounters()
        {
            thread_local WorkerCounters counters;
            return counters;
        }

        using Clock = std::chrono::steady_clock;
        Clock::time_point Start()
        {
            if constexpr (CookSettings::EnableDetailedProfiling)
                return Clock::now();
            return {};
        }

        void Add(Counter destination, Clock::time_point start)
        {
            if constexpr (CookSettings::EnableDetailedProfiling)
            {
                GetWorkerCounters().values[destination] += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - start).count());
            }
        }

        void Reset()
        {
            if constexpr (!CookSettings::EnableDetailedProfiling)
                return;
            Registry& registry = GetRegistry();
            std::scoped_lock lock(registry.mutex);
            for (WorkerCounters* worker : registry.workers)
                worker->values.fill(0);
        }

        u64 Get(Counter counter)
        {
            if constexpr (!CookSettings::EnableDetailedProfiling)
                return 0;
            Registry& registry = GetRegistry();
            std::scoped_lock lock(registry.mutex);
            u64 total = 0;
            for (const WorkerCounters* worker : registry.workers)
                total += worker->values[counter];
            return total;
        }
    }

    struct SourceVertex
    {
        vec3 position = {};
        vec3 normal = vec3(0.0f, 1.0f, 0.0f);
        vec2 uv0 = {};
        vec2 uv1 = {};
        std::array<u8, 4> jointIndices = {};
        std::array<u8, 4> jointWeights = {};
    };

    enum class SourceBlendMode : u16
    {
        Opaque,
        AlphaKey,
        Alpha,
        NoAlphaAdd,
        Add,
        Mod,
        Mod2X,
        BlendAdd
    };

    enum class SourceMaterialKind : u8
    {
        M2,
        WMO
    };

    struct SourceMaterial
    {
        SourceBlendMode blendMode = SourceBlendMode::Opaque;
        u32 sourceBlendMode = 0;
        SourceMaterialKind kind = SourceMaterialKind::M2;
        u32 sourceFlags = 0;
        bool isUnlit = false;
        bool isUnfogged = false;
        bool isTwoSided = false;
    };

    struct SourceTexture
    {
        u64 assetID = FileFormat::INVALID_ASSET_ID;
        u16 samplerID = 0;
    };

    struct SourceTextureUnit
    {
        i16 shaderID = 0;
        u16 materialIndex = 0;
        u16 materialLayer = 0;
        u16 authoredTextureCount = 0;
        u8 flags = 0;
        std::vector<u32> textureIndices;
    };

    struct SourceBatch
    {
        u32 groupID = 0;
        u32 indexStart = 0;
        u32 indexCount = 0;
        std::vector<SourceTextureUnit> textureUnits;
    };

    struct SourceInstance
    {
        u64 modelAssetID = FileFormat::INVALID_ASSET_ID;
        vec3 position = {};
        quat rotation = quat(1.0f, 0.0f, 0.0f, 0.0f);
        f32 scale = 1.0f;
        u32 color = 0xFFFFFFFFu;
    };

    struct SourceInstanceSet
    {
        std::array<char, 20> name = {};
        u32 instanceOffset = 0;
        u32 numInstances = 0;
    };

    struct SourceModel
    {
        std::vector<SourceVertex> vertices;
        std::vector<u32> indices;
        std::vector<SourceMaterial> materials;
        std::vector<SourceTexture> textures;
        std::vector<SourceBatch> batches;
        std::vector<SourceInstanceSet> instanceSets;
        std::vector<SourceInstance> instances;
        bool isWMO = false;
    };

    struct CookVertex
    {
        f32 position[3] = {};
        f32 normal[3] = {};
        f32 tangent[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        f32 uv0[2] = {};
        f32 uv1[2] = {};
        u8 jointIndices[4] = {};
        u8 jointWeights[4] = {};
    };

    struct CookScratch
    {
        std::vector<f32> simplificationAttributes;
        std::vector<u32> simplificationLocalIndices;
        std::vector<u32> simplifiedIndices;
        std::vector<f32> tangents;
        std::vector<CookVertex> corners;
        std::vector<CookVertex> vertices;
        std::vector<u32> indices;
        std::vector<u32> optimizationLocalIndices;
        std::vector<u32> vertexFetchRemap;
        std::vector<CookVertex> optimizedVertices;
        std::vector<SourceVertex> boundsVertices;
        std::vector<u32> meshletLocalIndices;
        std::vector<meshopt_Meshlet> meshlets;
        std::vector<u32> meshletVertices;
        std::vector<u8> meshletTriangles;
    };

    thread_local CookScratch gCookScratch;

    struct IndexRange
    {
        u32 offset = 0;
        u32 count = 0;
        u32 batchIndex = 0;
    };

    struct LODSource
    {
        std::vector<u32> indices;
        std::vector<IndexRange> ranges;
        f32 geometricError = 0.0f;
    };

    struct MaterialDescription
    {
        struct ProgramUnit
        {
            u16 authoredShaderID = 0;
            u16 layer = 0;
            u32 sourceMaterialFlags = 0;
            u32 sourceBlendMode = 0;
            u16 textureCount = 0;
            u8 flags = 0;
            SourceBlendMode blendMode = SourceBlendMode::Opaque;
            SourceMaterialKind sourceMaterialKind = SourceMaterialKind::M2;
            bool isUnlit = false;
            bool isUnfogged = false;
            bool isTwoSided = false;
        };

        struct ProgramDefinition
        {
            std::array<u8, 256> bytes = {};
            u32 size = 0;

            bool operator==(const ProgramDefinition&) const = default;
        };

        FileFormat::Material::RasterClass rasterClass = FileFormat::Material::RasterClass::Solid;
        u32 flags = FileFormat::Material::MaterialFlags_CastsShadows |
            FileFormat::Material::MaterialFlags_ReceivesDecals |
            FileFormat::Material::MaterialFlags_ReceivesFog;
        u16 lightingModelID = WowMaterialABI::Standard;
        u16 executionGroupID = WowMaterialABI::OpaqueSimple;
        u32 programID = 0;
        FileFormat::Material::MaterialProgramKey programKey = FileFormat::Material::INVALID_MATERIAL_PROGRAM_KEY;
        u64 instanceSignature = 0;
        ProgramDefinition programDefinition;
        std::vector<ProgramUnit> programUnits;
        std::array<u64, WowMaterialABI::MaxTextures> textureAssetIDs = {};
        std::array<u16, WowMaterialABI::MaxTextures> samplerIDs = {};
        std::array<u32, WowMaterialABI::MaxTextures> legacyUnits = {};
        u32 textureCount = 0;
        f32 alphaCutoff = 0.5f;
    };

    std::mutex gGeneratedMaterialMutex;
    std::unordered_set<u64> gGeneratedMaterialPaths;
    std::unordered_map<u64, MaterialDescription> gPendingMaterials;
    std::unordered_map<FileFormat::Material::MaterialProgramKey, MaterialDescription> gMaterialProgramDefinitions;

    u64 HashBytes(const void* data, size_t size)
    {
        return XXHash64::hash(data, size, 0);
    }

    u64 HashString(const std::string& value)
    {
        return HashBytes(value.data(), value.size());
    }

    std::string Hex(u64 value)
    {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0') << std::setw(16) << value;
        return stream.str();
    }

    u32 FoldHash32(u64 value)
    {
        return static_cast<u32>(value) ^ static_cast<u32>(value >> 32u);
    }

    bool AddGeneratedFileLocked(Runtime* runtime, const std::string& path, std::shared_ptr<Bytebuffer>& buffer)
    {
        const u64 pathHash = HashString(path);
        if (gGeneratedMaterialPaths.contains(pathHash))
            return true;

        auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, buffer->writtenData);
        if (!manifest.AddFile(runtime, path, buffer))
            return false;

        gGeneratedMaterialPaths.insert(pathHash);
        return true;
    }

    bool RegisterMaterialProgramLocked(const MaterialDescription& description)
    {
        if (description.programKey == FileFormat::Material::INVALID_MATERIAL_PROGRAM_KEY ||
            description.programDefinition.size > description.programDefinition.bytes.size())
        {
            NC_LOG_ERROR("[Model V2] Invalid canonical material program definition for key {0}", Hex(description.programKey));
            return false;
        }

        const auto [programIt, insertedProgram] = gMaterialProgramDefinitions.try_emplace(
            description.programKey, description);
        if (!insertedProgram && programIt->second.programDefinition != description.programDefinition)
        {
            NC_LOG_ERROR("[Model V2] Canonical material program key collision for {0}", Hex(description.programKey));
            return false;
        }
        return true;
    }

    const char* SourceMaterialKindName(SourceMaterialKind kind)
    {
        switch (kind)
        {
            case SourceMaterialKind::M2: return "M2";
            case SourceMaterialKind::WMO: return "WMO";
        }
        return "Unknown";
    }

    std::string BytesToHex(const u8* bytes, u32 size)
    {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0');
        for (u32 i = 0; i < size; ++i)
            stream << std::setw(2) << static_cast<u32>(bytes[i]);
        return stream.str();
    }

    u32 PackSnorm(f32 value, u32 bits)
    {
        const i32 maxValue = (1 << (bits - 1u)) - 1;
        const i32 packed = static_cast<i32>(std::round(glm::clamp(value, -1.0f, 1.0f) * static_cast<f32>(maxValue)));
        return static_cast<u32>(packed) & ((1u << bits) - 1u);
    }

    vec2 EncodeOctahedral(const vec3& value)
    {
        vec3 normal = value;
        const f32 lengthSquared = glm::dot(normal, normal);
        if (lengthSquared <= std::numeric_limits<f32>::epsilon())
            normal = vec3(0.0f, 1.0f, 0.0f);
        else
            normal *= glm::inversesqrt(lengthSquared);

        normal /= glm::abs(normal.x) + glm::abs(normal.y) + glm::abs(normal.z);
        vec2 encoded(normal.x, normal.y);
        if (normal.z < 0.0f)
        {
            const vec2 sign(encoded.x >= 0.0f ? 1.0f : -1.0f, encoded.y >= 0.0f ? 1.0f : -1.0f);
            encoded = (vec2(1.0f) - glm::abs(vec2(encoded.y, encoded.x))) * sign;
        }
        return encoded;
    }

    u32 PackNormal(const vec3& normal)
    {
        const vec2 oct = EncodeOctahedral(normal);
        return PackSnorm(oct.x, 16) | (PackSnorm(oct.y, 16) << 16u);
    }

    u32 PackTangent(const vec4& tangent)
    {
        const vec2 oct = EncodeOctahedral(vec3(tangent));
        return PackSnorm(oct.x, 15) |
            (PackSnorm(oct.y, 15) << 15u) |
            ((tangent.w < 0.0f ? 1u : 0u) << 30u);
    }

    FileFormat::Model::Bounds CalculateBounds(const std::vector<SourceVertex>& vertices, const std::vector<u32>* indices = nullptr)
    {
        FileFormat::Model::Bounds result;
        if (vertices.empty() || (indices && indices->empty()))
            return result;

        vec3 minimum(std::numeric_limits<f32>::max());
        vec3 maximum(std::numeric_limits<f32>::lowest());
        if (indices)
        {
            for (u32 index : *indices)
            {
                minimum = glm::min(minimum, vertices[index].position);
                maximum = glm::max(maximum, vertices[index].position);
            }
        }
        else
        {
            for (const SourceVertex& vertex : vertices)
            {
                minimum = glm::min(minimum, vertex.position);
                maximum = glm::max(maximum, vertex.position);
            }
        }

        result.center = (minimum + maximum) * 0.5f;
        result.extents = maximum - result.center;
        f32 radiusSquared = 0.0f;
        if (indices)
        {
            for (u32 index : *indices)
                radiusSquared = glm::max(radiusSquared, glm::length2(vertices[index].position - result.center));
        }
        else
        {
            for (const SourceVertex& vertex : vertices)
                radiusSquared = glm::max(radiusSquared, glm::length2(vertex.position - result.center));
        }
        result.sphereRadius = std::sqrt(radiusSquared);
        return result;
    }

    bool BuildBaseLOD(const SourceModel& source, LODSource& output)
    {
        auto getIndexCount = [&source](u32 batchIndex)
        {
            const SourceBatch& batch = source.batches[batchIndex];
            const u64 declaredEnd = static_cast<u64>(batch.indexStart) + batch.indexCount;
            if (batch.indexCount >= 3 && batch.indexCount % 3 == 0 && declaredEnd <= source.indices.size())
                return batch.indexCount;

            // M2SkinSection stores indexCount as u16. A section larger than
            // 65,535 indices wraps even though the skin's combined index array
            // and level-adjusted indexStart retain enough information. Recover
            // the range from the next section (or the array end for the last
            // section); a valid triangle list must still end on a triplet.
            u64 inferredEnd = source.indices.size();
            for (const SourceBatch& other : source.batches)
            {
                if (other.indexStart > batch.indexStart)
                    inferredEnd = glm::min(inferredEnd, static_cast<u64>(other.indexStart));
            }
            if (inferredEnd <= batch.indexStart)
                return 0u;

            const u64 inferredCount = inferredEnd - batch.indexStart;
            return inferredCount >= 3 && inferredCount % 3 == 0 && inferredCount <= std::numeric_limits<u32>::max()
                ? static_cast<u32>(inferredCount) : 0u;
        };

        for (u32 batchIndex = 0; batchIndex < source.batches.size(); ++batchIndex)
        {
            const SourceBatch& batch = source.batches[batchIndex];
            const u32 indexCount = getIndexCount(batchIndex);
            const u64 end = static_cast<u64>(batch.indexStart) + indexCount;
            if (indexCount == 0 || end > source.indices.size())
                continue;

            IndexRange range;
            range.offset = static_cast<u32>(output.indices.size());
            range.batchIndex = batchIndex;
            for (u32 indexOffset = 0; indexOffset < indexCount; ++indexOffset)
            {
                const u32 vertexIndex = source.indices[batch.indexStart + indexOffset];
                if (vertexIndex >= source.vertices.size())
                    return false;
                output.indices.push_back(vertexIndex);
            }
            range.count = static_cast<u32>(output.indices.size()) - range.offset;
            output.ranges.push_back(range);
        }
        return !output.indices.empty();
    }

    void BuildSimplificationAttributes(const std::vector<SourceVertex>& vertices, std::vector<f32>& attributes)
    {
        constexpr u32 AttributeCount = 15;
        attributes.resize(vertices.size() * AttributeCount);
        for (size_t i = 0; i < vertices.size(); ++i)
        {
            const SourceVertex& vertex = vertices[i];
            f32* destination = attributes.data() + i * AttributeCount;
            destination[0] = vertex.normal.x;
            destination[1] = vertex.normal.y;
            destination[2] = vertex.normal.z;
            destination[3] = vertex.uv0.x;
            destination[4] = vertex.uv0.y;
            destination[5] = vertex.uv1.x;
            destination[6] = vertex.uv1.y;
            for (u32 influence = 0; influence < 4; ++influence)
            {
                destination[7 + influence] = static_cast<f32>(vertex.jointWeights[influence]) / 255.0f;
                destination[11 + influence] = static_cast<f32>(vertex.jointIndices[influence]) / 255.0f;
            }
        }
    }

    bool BuildGeneratedLOD(const LODSource& base, const std::vector<SourceVertex>& vertices,
        const std::vector<f32>& attributes, f32 ratio, LODSource& output, CookScratch& scratch)
    {
        constexpr u32 AttributeCount = 15;
        static constexpr std::array<f32, AttributeCount> AttributeWeights = {
            0.5f, 0.5f, 0.5f,
            1.0f, 1.0f, 0.5f, 0.5f,
            1.0f, 1.0f, 1.0f, 1.0f,
            4.0f, 4.0f, 4.0f, 4.0f
        };
        f32 maximumError = 0.0f;
        for (const IndexRange& baseRange : base.ranges)
        {
            const u32* input = base.indices.data() + baseRange.offset;
            const size_t targetIndexCount = glm::max<size_t>(3, (static_cast<size_t>(baseRange.count * ratio) / 3) * 3);
            scratch.simplifiedIndices.resize(baseRange.count);
            std::vector<u32>& simplified = scratch.simplifiedIndices;
            f32 error = 0.0f;
            size_t simplifiedCount = baseRange.count;

            if (baseRange.count >= 96 && targetIndexCount < baseRange.count)
            {
                const auto [minimumIt, maximumIt] = std::minmax_element(input, input + baseRange.count);
                const u32 minimumVertex = *minimumIt;
                const u32 rangeVertexCount = *maximumIt - minimumVertex + 1u;
                scratch.simplificationLocalIndices.resize(baseRange.count);
                std::vector<u32>& localIndices = scratch.simplificationLocalIndices;
                std::transform(input, input + baseRange.count, localIndices.begin(),
                    [minimumVertex](u32 index) { return index - minimumVertex; });
                const auto simplifyStart = MeshoptimizerTiming::Start();
                simplifiedCount = meshopt_simplifyWithAttributes(
                    simplified.data(), localIndices.data(), baseRange.count,
                    &vertices[minimumVertex].position.x, rangeVertexCount, sizeof(SourceVertex),
                    attributes.data() + static_cast<size_t>(minimumVertex) * AttributeCount,
                    AttributeCount * sizeof(f32), AttributeWeights.data(), AttributeCount,
                    nullptr, targetIndexCount, CookSettings::LODTargetError,
                    meshopt_SimplifyLockBorder, &error);
                MeshoptimizerTiming::Add(MeshoptimizerTiming::Simplification, simplifyStart);
                for (size_t index = 0; index < simplifiedCount; ++index)
                    simplified[index] += minimumVertex;
            }
            else
            {
                std::copy(input, input + baseRange.count, simplified.begin());
            }

            if (simplifiedCount < 3 || simplifiedCount % 3 != 0)
                return false;

            IndexRange range;
            range.offset = static_cast<u32>(output.indices.size());
            range.count = static_cast<u32>(simplifiedCount);
            range.batchIndex = baseRange.batchIndex;
            output.indices.insert(output.indices.end(), simplified.begin(), simplified.begin() + simplifiedCount);
            output.ranges.push_back(range);
            maximumError = glm::max(maximumError, error);
        }

        output.geometricError = maximumError;
        return output.indices.size() + 3 < base.indices.size();
    }

    void GenerateCornerTangents(std::vector<CookVertex>& corners, const std::vector<u32>& sourceIndices,
        const std::vector<SourceVertex>& sourceVertices, CookScratch& scratch)
    {
        corners.resize(sourceIndices.size());
        scratch.tangents.assign(sourceIndices.size() * 4, 0.0f);
        std::vector<f32>& tangents = scratch.tangents;
        if (CookSettings::GenerateTangents)
        {
            const auto tangentStart = MeshoptimizerTiming::Start();
            meshopt_generateTangents(tangents.data(), sourceIndices.data(), sourceIndices.size(),
                &sourceVertices[0].position.x, sourceVertices.size(), sizeof(SourceVertex),
                &sourceVertices[0].normal.x, sizeof(SourceVertex),
                &sourceVertices[0].uv0.x, sizeof(SourceVertex), 0);
            MeshoptimizerTiming::Add(MeshoptimizerTiming::TangentGeneration, tangentStart);
        }

        for (size_t cornerIndex = 0; cornerIndex < sourceIndices.size(); ++cornerIndex)
        {
            const SourceVertex& source = sourceVertices[sourceIndices[cornerIndex]];
            CookVertex& destination = corners[cornerIndex];
            std::memcpy(destination.position, &source.position.x, sizeof(destination.position));
            std::memcpy(destination.normal, &source.normal.x, sizeof(destination.normal));
            destination.uv0[0] = source.uv0.x;
            destination.uv0[1] = source.uv0.y;
            destination.uv1[0] = source.uv1.x;
            destination.uv1[1] = source.uv1.y;
            std::memcpy(destination.jointIndices, source.jointIndices.data(), sizeof(destination.jointIndices));
            std::memcpy(destination.jointWeights, source.jointWeights.data(), sizeof(destination.jointWeights));
            if (CookSettings::GenerateTangents)
                std::memcpy(destination.tangent, tangents.data() + cornerIndex * 4, sizeof(destination.tangent));
        }
    }

    void CompactCorners(const std::vector<CookVertex>& corners, std::vector<CookVertex>& vertices, std::vector<u32>& indices)
    {
        indices.resize(corners.size());
        const auto remapStart = MeshoptimizerTiming::Start();
        const size_t vertexCount = meshopt_generateVertexRemap(indices.data(), nullptr, corners.size(), corners.data(), corners.size(), sizeof(CookVertex));
        MeshoptimizerTiming::Add(MeshoptimizerTiming::VertexRemap, remapStart);
        vertices.resize(vertexCount);
        for (size_t corner = 0; corner < corners.size(); ++corner)
            vertices[indices[corner]] = corners[corner];
    }

    void OptimizeLOD(std::vector<CookVertex>& vertices, std::vector<u32>& indices,
        const std::vector<IndexRange>& ranges, CookScratch& scratch)
    {
        for (const IndexRange& range : ranges)
        {
            u32* rangeIndices = indices.data() + range.offset;
            const auto [minimumIt, maximumIt] = std::minmax_element(rangeIndices, rangeIndices + range.count);
            const u32 minimumVertex = *minimumIt;
            const u32 rangeVertexCount = *maximumIt - minimumVertex + 1u;
            scratch.optimizationLocalIndices.resize(range.count);
            std::vector<u32>& localIndices = scratch.optimizationLocalIndices;
            std::transform(rangeIndices, rangeIndices + range.count, localIndices.begin(),
                [minimumVertex](u32 index) { return index - minimumVertex; });
            if (CookSettings::OptimizeVertexCache)
            {
                const auto cacheStart = MeshoptimizerTiming::Start();
                meshopt_optimizeVertexCache(localIndices.data(), localIndices.data(), range.count, rangeVertexCount);
                MeshoptimizerTiming::Add(MeshoptimizerTiming::VertexCache, cacheStart);
            }
            if (CookSettings::OptimizeOverdraw)
            {
                const auto overdrawStart = MeshoptimizerTiming::Start();
                meshopt_optimizeOverdraw(localIndices.data(), localIndices.data(), range.count,
                    vertices[minimumVertex].position, rangeVertexCount, sizeof(CookVertex), CookSettings::OverdrawThreshold);
                MeshoptimizerTiming::Add(MeshoptimizerTiming::Overdraw, overdrawStart);
            }
            std::transform(localIndices.begin(), localIndices.end(), rangeIndices,
                [minimumVertex](u32 index) { return index + minimumVertex; });
        }

        if (CookSettings::OptimizeVertexFetch)
        {
            const auto fetchStart = MeshoptimizerTiming::Start();
            scratch.vertexFetchRemap.resize(vertices.size());
            std::vector<u32>& remap = scratch.vertexFetchRemap;
            const size_t optimizedVertexCount = meshopt_optimizeVertexFetchRemap(remap.data(), indices.data(), indices.size(), vertices.size());
            scratch.optimizedVertices.resize(optimizedVertexCount);
            std::vector<CookVertex>& optimizedVertices = scratch.optimizedVertices;
            meshopt_remapVertexBuffer(optimizedVertices.data(), vertices.data(), vertices.size(), sizeof(CookVertex), remap.data());
            meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
            vertices.swap(optimizedVertices);
            MeshoptimizerTiming::Add(MeshoptimizerTiming::VertexFetch, fetchStart);
        }
    }

    MaterialDescription DescribeMaterial(const SourceModel& source, const SourceBatch& batch)
    {
        MaterialDescription result;
        result.textureAssetIDs.fill(FileFormat::INVALID_ASSET_ID);

        std::vector<u8> programBytes;
        auto appendProgram = [&programBytes](const auto& value)
        {
            const u8* bytes = reinterpret_cast<const u8*>(&value);
            programBytes.insert(programBytes.end(), bytes, bytes + sizeof(value));
        };

        // The canonical key describes source program semantics, not converter
        // routing. Keep these versioned fields in the byte definition so an ABI
        // or manifest-contract change cannot silently reuse an older key.
        appendProgram(WowMaterialABI::ManifestSchemaVersion);
        appendProgram(WowMaterialABI::Version);
        appendProgram(WowMaterialABI::ParameterBlockSize);
        const u32 sourceUnitCount = static_cast<u32>(batch.textureUnits.size());
        appendProgram(sourceUnitCount);

        bool isUnlit = false;
        bool isTwoSided = false;
        bool receivesFog = true;
        SourceBlendMode blendingMode = SourceBlendMode::Opaque;

        for (u32 unitIndex = 0; unitIndex < batch.textureUnits.size(); ++unitIndex)
        {
            const SourceTextureUnit& unit = batch.textureUnits[unitIndex];
            MaterialDescription::ProgramUnit& programUnit = result.programUnits.emplace_back();
            programUnit.authoredShaderID = static_cast<u16>(unit.shaderID);
            programUnit.layer = unit.materialLayer;
            programUnit.textureCount = unit.authoredTextureCount;
            programUnit.flags = unit.flags;
            if (unit.materialIndex < source.materials.size())
            {
                const SourceMaterial& material = source.materials[unit.materialIndex];
                blendingMode = material.blendMode;
                isUnlit |= material.isUnlit;
                isTwoSided |= material.isTwoSided;
                receivesFog &= !material.isUnfogged;
                programUnit.sourceMaterialFlags = material.sourceFlags;
                programUnit.sourceBlendMode = material.sourceBlendMode;
                programUnit.blendMode = material.blendMode;
                programUnit.sourceMaterialKind = material.kind;
                programUnit.isUnlit = material.isUnlit;
                programUnit.isUnfogged = material.isUnfogged;
                programUnit.isTwoSided = material.isTwoSided;
            }

            // Preserve the authored WoW shader ID. Material programs are still
            // placeholders, and the raw value is a lossless discriminator for
            // the eventual Game-side program mapping.
            const u32 packedUnit = static_cast<u16>(unit.shaderID) |
                (static_cast<u32>(unit.textureIndices.size() & 0xFu) << 16u) |
                (static_cast<u32>(unit.materialLayer & 0xFFu) << 20u) |
                (static_cast<u32>(unit.flags & 0xFu) << 28u);
            if (unitIndex < result.legacyUnits.size())
                result.legacyUnits[unitIndex] = packedUnit;

            appendProgram(programUnit.authoredShaderID);
            appendProgram(programUnit.layer);
            appendProgram(programUnit.sourceMaterialFlags);
            appendProgram(programUnit.sourceBlendMode);
            appendProgram(programUnit.textureCount);
            appendProgram(programUnit.flags);
            appendProgram(programUnit.blendMode);
            appendProgram(programUnit.sourceMaterialKind);
            appendProgram(static_cast<u8>(programUnit.isUnlit));
            appendProgram(static_cast<u8>(programUnit.isUnfogged));
            appendProgram(static_cast<u8>(programUnit.isTwoSided));

            for (u32 textureIndex : unit.textureIndices)
            {
                if (result.textureCount >= WowMaterialABI::MaxTextures)
                    break;
                if (textureIndex >= source.textures.size())
                    continue;
                result.textureAssetIDs[result.textureCount] = source.textures[textureIndex].assetID;
                result.samplerIDs[result.textureCount] = source.textures[textureIndex].samplerID;
                ++result.textureCount;
            }
        }

        switch (blendingMode)
        {
            case SourceBlendMode::AlphaKey:
                result.rasterClass = FileFormat::Material::RasterClass::AlphaTest;
                result.flags |= FileFormat::Material::MaterialFlags_HasCoverageFunction;
                result.executionGroupID = batch.textureUnits.size() <= 1 ? WowMaterialABI::AlphaTestSimple : WowMaterialABI::AlphaTestLayered;
                break;
            case SourceBlendMode::Opaque:
                result.rasterClass = FileFormat::Material::RasterClass::Solid;
                result.executionGroupID = batch.textureUnits.size() <= 1 ? WowMaterialABI::OpaqueSimple : WowMaterialABI::OpaqueLayered;
                break;
            default:
                result.rasterClass = FileFormat::Material::RasterClass::Transparent;
                result.flags &= ~FileFormat::Material::MaterialFlags_CastsShadows;
                result.executionGroupID = batch.textureUnits.size() <= 1 ? WowMaterialABI::TransparentSimple : WowMaterialABI::TransparentLayered;
                break;
        }

        if (isTwoSided)
            result.flags |= FileFormat::Material::MaterialFlags_TwoSided;
        if (!receivesFog)
            result.flags &= ~FileFormat::Material::MaterialFlags_ReceivesFog;
        result.lightingModelID = isUnlit ? WowMaterialABI::Unlit : WowMaterialABI::Standard;

        appendProgram(result.rasterClass);
        appendProgram(result.flags);
        appendProgram(result.lightingModelID);
        // executionGroupID is converter routing selected from these semantics;
        // it is deliberately absent from the authored canonical definition.
        result.programDefinition.size = static_cast<u32>(programBytes.size());
        if (programBytes.size() <= result.programDefinition.bytes.size())
            std::memcpy(result.programDefinition.bytes.data(), programBytes.data(), programBytes.size());
        result.programKey = HashBytes(programBytes.data(), programBytes.size());
        result.programID = FoldHash32(result.programKey);

        std::vector<u8> instanceBytes = programBytes;
        const u8* textureBytes = reinterpret_cast<const u8*>(result.textureAssetIDs.data());
        instanceBytes.insert(instanceBytes.end(), textureBytes, textureBytes + sizeof(result.textureAssetIDs));
        const u8* samplerBytes = reinterpret_cast<const u8*>(result.samplerIDs.data());
        instanceBytes.insert(instanceBytes.end(), samplerBytes, samplerBytes + sizeof(result.samplerIDs));
        result.instanceSignature = HashBytes(instanceBytes.data(), instanceBytes.size());
        return result;
    }

    FileFormat::Material::MaterialData BuildMaterialData()
    {
        FileFormat::Material::MaterialData data;
        auto addParameter = [&data](const char* name, u32 offset, u16 size, FileFormat::Material::ParameterType type)
        {
            data.parameters.push_back({ HashBytes(name, std::strlen(name)), offset, size, type, 1 });
        };

        addParameter("BaseColorFactor", WowMaterialABI::BaseColorOffset, 16, FileFormat::Material::ParameterType::Float4);
        for (u32 i = 0; i < WowMaterialABI::MaxTextures; ++i)
        {
            const std::string textureName = "Texture" + std::to_string(i);
            addParameter(textureName.c_str(), WowMaterialABI::TextureOffset + i * 4, 4, FileFormat::Material::ParameterType::Texture2D);
            const std::string unitName = "LegacyUnit" + std::to_string(i);
            addParameter(unitName.c_str(), WowMaterialABI::LegacyUnitOffset + i * 4, 4, FileFormat::Material::ParameterType::UInt);
        }
        addParameter("MaterialFlags", WowMaterialABI::FlagsOffset, 4, FileFormat::Material::ParameterType::UInt);
        addParameter("AlphaCutoff", WowMaterialABI::AlphaCutoffOffset, 4, FileFormat::Material::ParameterType::Float);

        data.defaultParameterData.resize(WowMaterialABI::ParameterBlockSize, 0);
        const vec4 white(1.0f);
        std::memcpy(data.defaultParameterData.data() + WowMaterialABI::BaseColorOffset, &white, sizeof(white));
        const f32 alphaCutoff = 0.5f;
        std::memcpy(data.defaultParameterData.data() + WowMaterialABI::AlphaCutoffOffset, &alphaCutoff, sizeof(alphaCutoff));
        return data;
    }

    const char* ParameterTypeName(FileFormat::Material::ParameterType type)
    {
        using Type = FileFormat::Material::ParameterType;
        switch (type)
        {
            case Type::Float: return "Float";
            case Type::Float2: return "Float2";
            case Type::Float3: return "Float3";
            case Type::Float4: return "Float4";
            case Type::UInt: return "UInt";
            case Type::UInt2: return "UInt2";
            case Type::UInt3: return "UInt3";
            case Type::UInt4: return "UInt4";
            case Type::Texture2D: return "Texture2D";
            case Type::TextureCube: return "TextureCube";
            case Type::Sampler: return "Sampler";
        }
        return "Unknown";
    }

    const char* RasterClassName(FileFormat::Material::RasterClass rasterClass)
    {
        using Class = FileFormat::Material::RasterClass;
        switch (rasterClass)
        {
            case Class::Solid: return "Solid";
            case Class::AlphaTest: return "AlphaTest";
            case Class::Transparent: return "Transparent";
        }
        return "Unknown";
    }

    const char* BlendModeName(SourceBlendMode blendMode)
    {
        switch (blendMode)
        {
            case SourceBlendMode::Opaque: return "Opaque";
            case SourceBlendMode::AlphaKey: return "AlphaKey";
            case SourceBlendMode::Alpha: return "Alpha";
            case SourceBlendMode::NoAlphaAdd: return "NoAlphaAdd";
            case SourceBlendMode::Add: return "Add";
            case SourceBlendMode::Mod: return "Mod";
            case SourceBlendMode::Mod2X: return "Mod2X";
            case SourceBlendMode::BlendAdd: return "BlendAdd";
        }
        return "Unknown";
    }

    bool ExportMaterialProgramManifest(Runtime* runtime, const FileFormat::Material::MaterialData& materialData)
    {
        std::vector<MaterialDescription> programs;
        {
            std::scoped_lock lock(gGeneratedMaterialMutex);
            programs.reserve(gMaterialProgramDefinitions.size());
            for (const auto& [programKey, description] : gMaterialProgramDefinitions)
                programs.push_back(description);
        }
        std::sort(programs.begin(), programs.end(), [](const MaterialDescription& left, const MaterialDescription& right)
        {
            return left.programKey < right.programKey;
        });

        nlohmann::ordered_json manifest;
        manifest["schemaVersion"] = WowMaterialABI::ManifestSchemaVersion;
        manifest["materialABIVersion"] = WowMaterialABI::Version;
        manifest["programCount"] = programs.size();
        manifest["parameterBlockSize"] = WowMaterialABI::ParameterBlockSize;
        manifest["parameterBlockAlignment"] = 16;
        manifest["parameterLayoutHash"] = FileFormat::Material::CalculateParameterLayoutHash(
            materialData.parameters, WowMaterialABI::ParameterBlockSize);
        manifest["parameters"] = nlohmann::ordered_json::array();
        for (const FileFormat::Material::ParameterDefinition& parameter : materialData.parameters)
        {
            nlohmann::ordered_json definition;
            definition["nameHash"] = parameter.nameHash;
            definition["byteOffset"] = parameter.byteOffset;
            definition["byteSize"] = parameter.byteSize;
            definition["type"] = static_cast<u8>(parameter.type);
            definition["typeName"] = ParameterTypeName(parameter.type);
            definition["arrayCount"] = parameter.arrayCount;
            manifest["parameters"].push_back(std::move(definition));
        }

        manifest["programs"] = nlohmann::ordered_json::array();
        for (const MaterialDescription& program : programs)
        {
            nlohmann::ordered_json definition;
            definition["canonicalKey"] = "wow/" + Hex(program.programKey);
            definition["programKey"] = program.programKey;
            definition["programID"] = program.programID;
            definition["canonicalDefinition"] = BytesToHex(
                program.programDefinition.bytes.data(), program.programDefinition.size);
            definition["lightingModelID"] = program.lightingModelID;
            definition["rasterClass"] = static_cast<u8>(program.rasterClass);
            definition["rasterClassName"] = RasterClassName(program.rasterClass);
            definition["materialFlags"] = program.flags;
            definition["units"] = nlohmann::ordered_json::array();
            for (u32 unitIndex = 0; unitIndex < program.programUnits.size(); ++unitIndex)
            {
                const MaterialDescription::ProgramUnit& unit = program.programUnits[unitIndex];
                nlohmann::ordered_json unitDefinition;
                unitDefinition["unitIndex"] = unitIndex;
                unitDefinition["authoredShaderID"] = unit.authoredShaderID;
                unitDefinition["textureCount"] = unit.textureCount;
                unitDefinition["layer"] = unit.layer;
                unitDefinition["flags"] = unit.flags;
                unitDefinition["blendMode"] = static_cast<u16>(unit.blendMode);
                unitDefinition["blendModeName"] = BlendModeName(unit.blendMode);
                unitDefinition["sourceMaterialKind"] = static_cast<u8>(unit.sourceMaterialKind);
                unitDefinition["sourceMaterialKindName"] = SourceMaterialKindName(unit.sourceMaterialKind);
                unitDefinition["sourceMaterialFlags"] = unit.sourceMaterialFlags;
                unitDefinition["sourceBlendMode"] = unit.sourceBlendMode;
                unitDefinition["isUnlit"] = unit.isUnlit;
                unitDefinition["isUnfogged"] = unit.isUnfogged;
                unitDefinition["isTwoSided"] = unit.isTwoSided;
                definition["units"].push_back(std::move(unitDefinition));
            }
            manifest["programs"].push_back(std::move(definition));
        }

        const std::filesystem::path outputPath = runtime->paths.pactRoot / "material_program_manifest.json";
        const std::filesystem::path temporaryPath = outputPath.string() + ".tmp";
        std::ofstream output(temporaryPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            NC_LOG_ERROR("[Model V2] Failed to open material program manifest {0}", temporaryPath.string());
            return false;
        }
        output << manifest.dump(2) << '\n';
        output.close();
        if (!output)
        {
            NC_LOG_ERROR("[Model V2] Failed to write material program manifest {0}", temporaryPath.string());
            return false;
        }

        std::error_code error;
        std::filesystem::remove(outputPath, error);
        error.clear();
        std::filesystem::rename(temporaryPath, outputPath, error);
        if (error)
        {
            NC_LOG_ERROR("[Model V2] Failed to publish material program manifest {0}: {1}", outputPath.string(), error.message());
            return false;
        }
        return true;
    }

    bool ValidateMaterialData(const FileFormat::Material::MaterialData& data)
    {
        if (data.defaultParameterData.size() != WowMaterialABI::ParameterBlockSize || data.parameters.empty())
            return false;

        std::array<bool, WowMaterialABI::ParameterBlockSize> occupiedBytes = {};
        std::unordered_set<u64> parameterNames;
        for (const FileFormat::Material::ParameterDefinition& parameter : data.parameters)
        {
            if (parameter.nameHash == 0 || parameter.byteSize == 0 || parameter.arrayCount != 1 ||
                static_cast<u64>(parameter.byteOffset) + parameter.byteSize > WowMaterialABI::ParameterBlockSize ||
                !parameterNames.insert(parameter.nameHash).second)
                return false;
            for (u32 byteIndex = parameter.byteOffset; byteIndex < parameter.byteOffset + parameter.byteSize; ++byteIndex)
            {
                if (occupiedBytes[byteIndex])
                    return false;
                occupiedBytes[byteIndex] = true;
            }
        }
        return true;
    }

    bool EmitMaterialAssets(Runtime* runtime, const MaterialDescription& description,
        const FileFormat::Material::MaterialData& materialData, u64& instanceAssetID)
    {
        const std::string materialPath = "material/generated/wow/program_" + Hex(description.programKey) + FileFormat::Material::MATERIAL_FILE_EXTENSION;
        const std::string instancePath = "material/generated/wow/instance_" + Hex(description.instanceSignature) + FileFormat::Material::MATERIAL_INSTANCE_FILE_EXTENSION;
        instanceAssetID = HashString(instancePath);

        // Most WoW batches reuse one of a comparatively small number of
        // programs/instances. Check the cache before constructing parameter
        // metadata or serializing buffers; the old ordering repeated that work
        // for every submesh and was particularly costly for large WMOs.
        std::scoped_lock lock(gGeneratedMaterialMutex);
        const bool hasMaterial = gGeneratedMaterialPaths.contains(HashString(materialPath));
        const bool hasInstance = gGeneratedMaterialPaths.contains(HashString(instancePath));
        if (hasMaterial && hasInstance)
            return true;

        if (!hasMaterial)
        {
            FileFormat::Material::MaterialAsset material;
            material.programKey = description.programKey;
            material.programID = description.programID;
            material.lightingModelID = description.lightingModelID;
            material.materialExecutionGroupID = description.executionGroupID;
            material.rasterClass = description.rasterClass;
            material.flags = description.flags;
            material.parameterBlockSize = WowMaterialABI::ParameterBlockSize;

            std::shared_ptr<Bytebuffer> materialBuffer = Bytebuffer::BorrowRuntime(material.GetSerializedSize(materialData));
            if (!material.Save(materialBuffer, materialData) || !AddGeneratedFileLocked(runtime, materialPath, materialBuffer))
                return false;
        }

        if (hasInstance)
            return true;

        FileFormat::Material::MaterialInstanceData instanceData;
        instanceData.parameterData = materialData.defaultParameterData;
        for (u32 i = 0; i < WowMaterialABI::MaxTextures; ++i)
        {
            std::memcpy(instanceData.parameterData.data() + WowMaterialABI::LegacyUnitOffset + i * 4, &description.legacyUnits[i], sizeof(u32));
            if (description.textureAssetIDs[i] != FileFormat::INVALID_ASSET_ID)
            {
                instanceData.resourceBindings.push_back({ description.textureAssetIDs[i], WowMaterialABI::TextureOffset + i * 4,
                    description.samplerIDs[i], FileFormat::Material::ResourceType::Texture2D, FileFormat::Material::ResourceBindingFlags_None });
            }
        }
        std::memcpy(instanceData.parameterData.data() + WowMaterialABI::FlagsOffset, &description.flags, sizeof(description.flags));
        std::memcpy(instanceData.parameterData.data() + WowMaterialABI::AlphaCutoffOffset, &description.alphaCutoff, sizeof(description.alphaCutoff));

        FileFormat::Material::MaterialInstanceAsset instance;
        instance.materialAssetID = HashString(materialPath);
        instance.parameterLayoutHash = FileFormat::Material::CalculateParameterLayoutHash(
            materialData.parameters, WowMaterialABI::ParameterBlockSize);
        std::shared_ptr<Bytebuffer> instanceBuffer = Bytebuffer::BorrowRuntime(instance.GetSerializedSize(instanceData));
        return instance.Save(instanceBuffer, instanceData) && AddGeneratedFileLocked(runtime, instancePath, instanceBuffer);
    }

    bool QueueOrEmitMaterialAssets(Runtime* runtime, const MaterialDescription& description, u64& instanceAssetID)
    {
        const std::string instancePath = "material/generated/wow/instance_" + Hex(description.instanceSignature) +
            FileFormat::Material::MATERIAL_INSTANCE_FILE_EXTENSION;
        instanceAssetID = HashString(instancePath);
        if (!CookSettings::DeferMaterialEmission)
        {
            {
                std::scoped_lock lock(gGeneratedMaterialMutex);
                if (!RegisterMaterialProgramLocked(description))
                    return false;
            }
            const FileFormat::Material::MaterialData materialData = BuildMaterialData();
            return EmitMaterialAssets(runtime, description, materialData, instanceAssetID);
        }

        std::scoped_lock lock(gGeneratedMaterialMutex);
        if (!RegisterMaterialProgramLocked(description))
            return false;
        if (!gGeneratedMaterialPaths.contains(instanceAssetID))
            gPendingMaterials.try_emplace(instanceAssetID, description);
        return true;
    }

    bool AppendLOD(const LODSource& sourceLOD, const std::vector<SourceVertex>& sourceVertices,
        FileFormat::Model::ModelData& output, const FileFormat::Model::Mesh& mesh, CookScratch& scratch)
    {
        scratch.corners.clear();
        std::vector<CookVertex>& corners = scratch.corners;
        GenerateCornerTangents(corners, sourceLOD.indices, sourceVertices, scratch);

        scratch.vertices.clear();
        scratch.indices.clear();
        std::vector<CookVertex>& vertices = scratch.vertices;
        std::vector<u32>& indices = scratch.indices;
        CompactCorners(corners, vertices, indices);
        if (vertices.empty())
            return false;
        OptimizeLOD(vertices, indices, sourceLOD.ranges, scratch);

        FileFormat::Model::MeshLOD lod;
        lod.vertexOffset = static_cast<u32>(output.positions.size());
        lod.numVertices = static_cast<u32>(vertices.size());
        lod.vertexAttributeOffset = static_cast<u32>(output.vertexAttributes.size());
        lod.numVertexAttributes = lod.numVertices;
        lod.submeshOffset = static_cast<u32>(output.submeshes.size());
        lod.meshletOffset = static_cast<u32>(output.meshlets.size());
        lod.geometricError = sourceLOD.geometricError;

        scratch.boundsVertices.resize(vertices.size());
        std::vector<SourceVertex>& boundsVertices = scratch.boundsVertices;
        bool isSkinned = false;
        std::array<i16, 256> jointRemap;
        jointRemap.fill(-1);
        std::vector<u16> palette;
        for (u32 vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex)
        {
            const CookVertex& vertex = vertices[vertexIndex];
            boundsVertices[vertexIndex].position = vec3(vertex.position[0], vertex.position[1], vertex.position[2]);
            for (u32 influence = 0; influence < 4; ++influence)
            {
                if (vertex.jointWeights[influence] == 0)
                    continue;
                isSkinned = true;
                const u8 sourceJoint = vertex.jointIndices[influence];
                if (jointRemap[sourceJoint] < 0)
                {
                    jointRemap[sourceJoint] = static_cast<i16>(palette.size());
                    palette.push_back(sourceJoint);
                }
            }
        }
        lod.bounds = CalculateBounds(boundsVertices);
        lod.jointPaletteRemapOffset = static_cast<u32>(output.jointPaletteRemaps.size());
        lod.numJointPaletteRemaps = static_cast<u32>(palette.size());
        output.jointPaletteRemaps.insert(output.jointPaletteRemaps.end(), palette.begin(), palette.end());

        const vec3 decodeMinimum = mesh.positionDecodeOffset;
        const vec3 decodeExtent = mesh.positionDecodeExtent;
        for (const CookVertex& vertex : vertices)
        {
            FileFormat::Model::PackedPosition position;
            for (u32 component = 0; component < 3; ++component)
            {
                const f32 normalized = decodeExtent[component] > 0.0f ? (vertex.position[component] - decodeMinimum[component]) / decodeExtent[component] : 0.0f;
                reinterpret_cast<u16*>(&position)[component] = static_cast<u16>(std::round(glm::clamp(normalized, 0.0f, 1.0f) * 65535.0f));
            }
            output.positions.push_back(position);

            FileFormat::Model::PackedVertexAttributes attributes;
            attributes.normal = PackNormal(vec3(vertex.normal[0], vertex.normal[1], vertex.normal[2]));
            attributes.tangent = PackTangent(vec4(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2], vertex.tangent[3]));
            attributes.uv0 = glm::packHalf2x16(vec2(vertex.uv0[0], vertex.uv0[1]));
            attributes.uv1 = glm::packHalf2x16(vec2(vertex.uv1[0], vertex.uv1[1]));
            output.vertexAttributes.push_back(attributes);

            if (isSkinned)
            {
                FileFormat::Model::PackedSkinningData skinning;
                for (u32 influence = 0; influence < 4; ++influence)
                {
                    const u8 weight = vertex.jointWeights[influence];
                    const u8 localJoint = weight > 0 ? static_cast<u8>(jointRemap[vertex.jointIndices[influence]]) : 0;
                    skinning.jointIndices |= static_cast<u32>(localJoint) << (influence * 8u);
                    skinning.jointWeights |= static_cast<u32>(weight) << (influence * 8u);
                }
                output.skinningData.push_back(skinning);
            }
        }
        if (isSkinned)
        {
            lod.skinningDataOffset = static_cast<u32>(output.skinningData.size()) - lod.numVertices;
            lod.numSkinningData = lod.numVertices;
            lod.flags |= FileFormat::Model::MeshLODFlags_HasSkinningData | FileFormat::Model::MeshLODFlags_StaticFallbackIsBindPose;
        }

        for (const IndexRange& range : sourceLOD.ranges)
        {
            FileFormat::Model::Submesh submesh;
            submesh.meshletOffset = static_cast<u32>(output.meshlets.size());
            submesh.materialSlotIndex = range.batchIndex;
            submesh.geometryGroupID = range.batchIndex;
            submesh.semanticPartID = range.batchIndex;

            const u32* rangeIndices = indices.data() + range.offset;
            const auto [minimumIt, maximumIt] = std::minmax_element(rangeIndices, rangeIndices + range.count);
            const u32 minimumVertex = *minimumIt;
            const u32 rangeVertexCount = *maximumIt - minimumVertex + 1u;
            scratch.meshletLocalIndices.resize(range.count);
            std::vector<u32>& localIndices = scratch.meshletLocalIndices;
            std::transform(rangeIndices, rangeIndices + range.count, localIndices.begin(),
                [minimumVertex](u32 index) { return index - minimumVertex; });
            const size_t meshletBound = meshopt_buildMeshletsBound(range.count, FileFormat::Model::MAX_MESHLET_VERTICES, FileFormat::Model::MAX_MESHLET_TRIANGLES);
            scratch.meshlets.resize(meshletBound);
            scratch.meshletVertices.resize(range.count);
            scratch.meshletTriangles.resize(range.count);
            std::vector<meshopt_Meshlet>& meshlets = scratch.meshlets;
            std::vector<u32>& meshletVertices = scratch.meshletVertices;
            std::vector<u8>& meshletTriangles = scratch.meshletTriangles;
            const auto meshletBuildStart = MeshoptimizerTiming::Start();
            const size_t meshletCount = meshopt_buildMeshlets(meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
                localIndices.data(), range.count, vertices[minimumVertex].position, rangeVertexCount, sizeof(CookVertex),
                FileFormat::Model::MAX_MESHLET_VERTICES, FileFormat::Model::MAX_MESHLET_TRIANGLES, CookSettings::MeshletConeWeight);
            MeshoptimizerTiming::Add(MeshoptimizerTiming::MeshletBuild, meshletBuildStart);

            for (size_t meshletIndex = 0; meshletIndex < meshletCount; ++meshletIndex)
            {
                meshopt_Meshlet& sourceMeshlet = meshlets[meshletIndex];
                u32* localVertices = meshletVertices.data() + sourceMeshlet.vertex_offset;
                u8* localTriangles = meshletTriangles.data() + sourceMeshlet.triangle_offset;
                if (CookSettings::OptimizeMeshlets)
                {
                    const auto meshletOptimizationStart = MeshoptimizerTiming::Start();
                    meshopt_optimizeMeshlet(localVertices, localTriangles, sourceMeshlet.triangle_count, sourceMeshlet.vertex_count);
                    MeshoptimizerTiming::Add(MeshoptimizerTiming::MeshletOptimization, meshletOptimizationStart);
                }

                const auto meshletBoundsStart = MeshoptimizerTiming::Start();
                const meshopt_Bounds bounds = meshopt_computeMeshletBounds(localVertices, localTriangles, sourceMeshlet.triangle_count,
                    vertices[minimumVertex].position, rangeVertexCount, sizeof(CookVertex));
                MeshoptimizerTiming::Add(MeshoptimizerTiming::MeshletBounds, meshletBoundsStart);
                FileFormat::Model::Meshlet meshlet;
                meshlet.boundsCenter = vec3(bounds.center[0], bounds.center[1], bounds.center[2]);
                meshlet.boundsRadius = bounds.radius;
                meshlet.vertexOffset = static_cast<u32>(output.meshletVertexIndices.size());
                meshlet.triangleOffset = static_cast<u32>(output.meshletTriangles.size());
                meshlet.packedNormalCone = static_cast<u8>(bounds.cone_axis_s8[0]) |
                    (static_cast<u32>(static_cast<u8>(bounds.cone_axis_s8[1])) << 8u) |
                    (static_cast<u32>(static_cast<u8>(bounds.cone_axis_s8[2])) << 16u) |
                    (static_cast<u32>(static_cast<u8>(bounds.cone_cutoff_s8)) << 24u);
                meshlet.vertexCount = static_cast<u16>(sourceMeshlet.vertex_count);
                meshlet.triangleCount = static_cast<u16>(sourceMeshlet.triangle_count);
                output.meshlets.push_back(meshlet);

                for (u32 vertexIndex = 0; vertexIndex < sourceMeshlet.vertex_count; ++vertexIndex)
                    output.meshletVertexIndices.push_back(localVertices[vertexIndex] + minimumVertex);
                for (u32 triangleIndex = 0; triangleIndex < sourceMeshlet.triangle_count; ++triangleIndex)
                {
                    const u8* triangle = localTriangles + triangleIndex * 3;
                    output.meshletTriangles.push_back({ static_cast<u32>(triangle[0]) |
                        (static_cast<u32>(triangle[1]) << 8u) | (static_cast<u32>(triangle[2]) << 16u) });
                }
            }

            submesh.numMeshlets = static_cast<u32>(output.meshlets.size()) - submesh.meshletOffset;
            output.submeshes.push_back(submesh);
        }

        lod.numSubmeshes = static_cast<u32>(output.submeshes.size()) - lod.submeshOffset;
        lod.numMeshlets = static_cast<u32>(output.meshlets.size()) - lod.meshletOffset;
        output.meshLODs.push_back(lod);
        return lod.numMeshlets > 0;
    }

    void MergeCookedLOD(FileFormat::Model::ModelData& destination, FileFormat::Model::ModelData& source)
    {
        const u32 vertexOffset = static_cast<u32>(destination.positions.size());
        const u32 vertexAttributeOffset = static_cast<u32>(destination.vertexAttributes.size());
        const u32 skinningDataOffset = static_cast<u32>(destination.skinningData.size());
        const u32 jointPaletteOffset = static_cast<u32>(destination.jointPaletteRemaps.size());
        const u32 submeshOffset = static_cast<u32>(destination.submeshes.size());
        const u32 meshletOffset = static_cast<u32>(destination.meshlets.size());
        const u32 meshletVertexOffset = static_cast<u32>(destination.meshletVertexIndices.size());
        const u32 meshletTriangleOffset = static_cast<u32>(destination.meshletTriangles.size());

        for (FileFormat::Model::MeshLOD& lod : source.meshLODs)
        {
            lod.vertexOffset += vertexOffset;
            lod.vertexAttributeOffset += vertexAttributeOffset;
            if (lod.numSkinningData > 0)
                lod.skinningDataOffset += skinningDataOffset;
            lod.jointPaletteRemapOffset += jointPaletteOffset;
            lod.submeshOffset += submeshOffset;
            lod.meshletOffset += meshletOffset;
        }
        for (FileFormat::Model::Submesh& submesh : source.submeshes)
            submesh.meshletOffset += meshletOffset;
        for (FileFormat::Model::Meshlet& meshlet : source.meshlets)
        {
            meshlet.vertexOffset += meshletVertexOffset;
            meshlet.triangleOffset += meshletTriangleOffset;
        }

        auto append = [](auto& to, auto& from)
        {
            to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
        };
        append(destination.meshLODs, source.meshLODs);
        append(destination.submeshes, source.submeshes);
        append(destination.meshlets, source.meshlets);
        append(destination.positions, source.positions);
        append(destination.vertexAttributes, source.vertexAttributes);
        append(destination.skinningData, source.skinningData);
        append(destination.meshletVertexIndices, source.meshletVertexIndices);
        append(destination.meshletTriangles, source.meshletTriangles);
        append(destination.jointPaletteRemaps, source.jointPaletteRemaps);
    }

    bool ValidateModel(const FileFormat::Model::ModelAsset& asset, const FileFormat::Model::ModelData& data)
    {
        if (data.meshes.size() != 1 || data.meshLODs.empty() || data.positions.size() != data.vertexAttributes.size())
            return false;

        const auto validRange = [](u32 offset, u32 count, size_t size)
        {
            return static_cast<u64>(offset) + count <= size;
        };
        const auto finiteBounds = [](const FileFormat::Model::Bounds& bounds)
        {
            return std::isfinite(bounds.center.x) && std::isfinite(bounds.center.y) && std::isfinite(bounds.center.z) &&
                std::isfinite(bounds.extents.x) && std::isfinite(bounds.extents.y) && std::isfinite(bounds.extents.z) &&
                std::isfinite(bounds.sphereRadius) && bounds.sphereRadius >= 0.0f &&
                glm::all(glm::greaterThanEqual(bounds.extents, vec3(0.0f)));
        };

        if (asset.collisionAssetID != FileFormat::INVALID_ASSET_ID || !finiteBounds(asset.bounds) ||
            data.materialSlots.empty() || asset.geometryGroupCount == 0)
            return false;

        for (const FileFormat::Model::PackedPosition& position : data.positions)
        {
            if (position.reserved != 0)
                return false;
        }
        for (const FileFormat::Model::PackedVertexAttributes& attributes : data.vertexAttributes)
        {
            if ((attributes.tangent & 0x80000000u) != 0)
                return false;
        }
        for (const FileFormat::Model::MaterialSlot& slot : data.materialSlots)
        {
            if (slot.defaultMaterialInstanceAssetID == FileFormat::INVALID_ASSET_ID)
                return false;
        }

        for (const FileFormat::Model::Mesh& mesh : data.meshes)
        {
            if (!validRange(mesh.lodOffset, mesh.numLODs, data.meshLODs.size()) ||
                !validRange(mesh.materialSlotOffset, mesh.numMaterialSlots, data.materialSlots.size()) ||
                mesh.numLODs == 0 || mesh.numMaterialSlots == 0 || !finiteBounds(mesh.bounds) ||
                !std::isfinite(mesh.positionDecodeOffset.x) || !std::isfinite(mesh.positionDecodeOffset.y) ||
                !std::isfinite(mesh.positionDecodeOffset.z) || !std::isfinite(mesh.positionDecodeExtent.x) ||
                !std::isfinite(mesh.positionDecodeExtent.y) || !std::isfinite(mesh.positionDecodeExtent.z) ||
                glm::any(glm::lessThan(mesh.positionDecodeExtent, vec3(0.0f))))
                return false;

            f32 previousError = -1.0f;
            for (u32 lodIndex = mesh.lodOffset; lodIndex < mesh.lodOffset + mesh.numLODs; ++lodIndex)
            {
                const FileFormat::Model::MeshLOD& lod = data.meshLODs[lodIndex];
                if (!validRange(lod.vertexOffset, lod.numVertices, data.positions.size()) ||
                    !validRange(lod.vertexAttributeOffset, lod.numVertexAttributes, data.vertexAttributes.size()) ||
                    !validRange(lod.skinningDataOffset, lod.numSkinningData, data.skinningData.size()) ||
                    !validRange(lod.submeshOffset, lod.numSubmeshes, data.submeshes.size()) ||
                    !validRange(lod.meshletOffset, lod.numMeshlets, data.meshlets.size()) ||
                    !validRange(lod.jointPaletteRemapOffset, lod.numJointPaletteRemaps, data.jointPaletteRemaps.size()) ||
                    lod.numVertices == 0 || lod.numVertexAttributes != lod.numVertices || lod.numSubmeshes == 0 || lod.numMeshlets == 0 ||
                    (lod.numSkinningData != 0 && lod.numSkinningData != lod.numVertices) || lod.numJointPaletteRemaps > 256 ||
                    !finiteBounds(lod.bounds) || !std::isfinite(lod.geometricError) || lod.geometricError < previousError ||
                    lod.reserved0 != 0 || lod.reserved1 != 0)
                    return false;
                previousError = lod.geometricError;

                const bool hasSkinning = lod.numSkinningData > 0;
                if (hasSkinning != ((lod.flags & FileFormat::Model::MeshLODFlags_HasSkinningData) != 0) ||
                    (hasSkinning && (lod.flags & FileFormat::Model::MeshLODFlags_StaticFallbackIsBindPose) == 0))
                    return false;
                for (u32 skinIndex = 0; skinIndex < lod.numSkinningData; ++skinIndex)
                {
                    const FileFormat::Model::PackedSkinningData& skinning = data.skinningData[lod.skinningDataOffset + skinIndex];
                    u32 weightSum = 0;
                    for (u32 influence = 0; influence < 4; ++influence)
                    {
                        const u32 weight = (skinning.jointWeights >> (influence * 8u)) & 0xFFu;
                        const u32 joint = (skinning.jointIndices >> (influence * 8u)) & 0xFFu;
                        weightSum += weight;
                        if (weight > 0 && joint >= lod.numJointPaletteRemaps)
                            return false;
                    }
                    if (weightSum != 255)
                        return false;
                }

                u32 expectedMeshletOffset = lod.meshletOffset;
                for (u32 submeshIndex = lod.submeshOffset; submeshIndex < lod.submeshOffset + lod.numSubmeshes; ++submeshIndex)
                {
                    const FileFormat::Model::Submesh& submesh = data.submeshes[submeshIndex];
                    if (submesh.meshletOffset != expectedMeshletOffset || submesh.numMeshlets == 0 ||
                        !validRange(submesh.meshletOffset, submesh.numMeshlets, data.meshlets.size()) ||
                        submesh.meshletOffset < lod.meshletOffset ||
                        static_cast<u64>(submesh.meshletOffset) + submesh.numMeshlets > static_cast<u64>(lod.meshletOffset) + lod.numMeshlets ||
                        submesh.materialSlotIndex >= mesh.numMaterialSlots || submesh.geometryGroupID >= asset.geometryGroupCount)
                        return false;
                    expectedMeshletOffset += submesh.numMeshlets;

                    for (u32 meshletIndex = submesh.meshletOffset; meshletIndex < submesh.meshletOffset + submesh.numMeshlets; ++meshletIndex)
                    {
                        const FileFormat::Model::Meshlet& meshlet = data.meshlets[meshletIndex];
                        if (meshlet.vertexCount == 0 || meshlet.triangleCount == 0 ||
                            meshlet.vertexCount > FileFormat::Model::MAX_MESHLET_VERTICES ||
                            meshlet.triangleCount > FileFormat::Model::MAX_MESHLET_TRIANGLES ||
                            !validRange(meshlet.vertexOffset, meshlet.vertexCount, data.meshletVertexIndices.size()) ||
                            !validRange(meshlet.triangleOffset, meshlet.triangleCount, data.meshletTriangles.size()) ||
                            !std::isfinite(meshlet.boundsCenter.x) || !std::isfinite(meshlet.boundsCenter.y) ||
                            !std::isfinite(meshlet.boundsCenter.z) || !std::isfinite(meshlet.boundsRadius) || meshlet.boundsRadius < 0.0f)
                            return false;
                        for (u32 vertexIndex = 0; vertexIndex < meshlet.vertexCount; ++vertexIndex)
                        {
                            if (data.meshletVertexIndices[meshlet.vertexOffset + vertexIndex] >= lod.numVertices)
                                return false;
                        }
                        for (u32 triangleIndex = 0; triangleIndex < meshlet.triangleCount; ++triangleIndex)
                        {
                            const u32 triangle = data.meshletTriangles[meshlet.triangleOffset + triangleIndex].localVertexIndices;
                            if ((triangle & 0xFF000000u) != 0 || (triangle & 0xFFu) >= meshlet.vertexCount ||
                                ((triangle >> 8u) & 0xFFu) >= meshlet.vertexCount || ((triangle >> 16u) & 0xFFu) >= meshlet.vertexCount)
                                return false;
                        }
                    }
                }
                if (expectedMeshletOffset != lod.meshletOffset + lod.numMeshlets)
                    return false;
            }
        }

        const bool hasEmbeddedInstances = !data.embeddedInstanceSets.empty() || !data.embeddedInstances.empty();
        if (hasEmbeddedInstances != ((asset.flags & FileFormat::Model::ModelFlags_HasEmbeddedInstances) != 0))
            return false;
        for (const FileFormat::Model::EmbeddedInstanceSet& set : data.embeddedInstanceSets)
        {
            if (!validRange(set.instanceOffset, set.numInstances, data.embeddedInstances.size()))
                return false;
        }
        for (const FileFormat::Model::EmbeddedInstance& instance : data.embeddedInstances)
        {
            if (!std::isfinite(instance.position.x) || !std::isfinite(instance.position.y) || !std::isfinite(instance.position.z) ||
                !std::isfinite(instance.rotation.x) || !std::isfinite(instance.rotation.y) || !std::isfinite(instance.rotation.z) ||
                !std::isfinite(instance.rotation.w) || !std::isfinite(instance.uniformScale))
                return false;
        }
        return true;
    }

    bool CookAndAdd(Runtime* runtime, const SourceModel& source, const std::string& outputPath)
    {
        if (!runtime || source.vertices.empty() || source.batches.empty())
            return false;
        CookScratch& scratch = gCookScratch;

        const auto baseLODStart = MeshoptimizerTiming::Start();
        LODSource baseLOD;
        if (!BuildBaseLOD(source, baseLOD))
            return false;
        MeshoptimizerTiming::Add(MeshoptimizerTiming::BaseLODAssembly, baseLODStart);

        std::vector<LODSource> lods;
        lods.push_back(std::move(baseLOD));
        const auto generatedLODStart = MeshoptimizerTiming::Start();
        if (CookSettings::GenerateDiscreteLODs)
        {
            BuildSimplificationAttributes(source.vertices, scratch.simplificationAttributes);
            const std::vector<f32>& simplificationAttributes = scratch.simplificationAttributes;

            std::array<LODSource, CookSettings::LODTriangleRatios.size()> generatedLODs;
            std::array<bool, CookSettings::LODTriangleRatios.size()> generatedSuccessfully = {};
            if (CookSettings::BuildLODsConcurrently && source.indices.size() >= CookSettings::ConcurrentLODMinimumIndexCount)
            {
                enki::TaskSet buildLODTasks(static_cast<u32>(generatedLODs.size()),
                    [&](enki::TaskSetPartition range, u32)
                    {
                        CookScratch& workerScratch = gCookScratch;
                        for (u32 lodIndex = range.start; lodIndex < range.end; ++lodIndex)
                        {
                            generatedSuccessfully[lodIndex] = BuildGeneratedLOD(lods.front(), source.vertices,
                                simplificationAttributes, CookSettings::LODTriangleRatios[lodIndex],
                                generatedLODs[lodIndex], workerScratch);
                        }
                    });
                runtime->scheduler.AddTaskSetToPipe(&buildLODTasks);
                runtime->scheduler.WaitforTask(&buildLODTasks);
            }
            else
            {
                for (u32 lodIndex = 0; lodIndex < generatedLODs.size(); ++lodIndex)
                {
                    generatedSuccessfully[lodIndex] = BuildGeneratedLOD(lods.front(), source.vertices,
                        simplificationAttributes, CookSettings::LODTriangleRatios[lodIndex],
                        generatedLODs[lodIndex], scratch);
                }
            }

            for (u32 lodIndex = 0; lodIndex < generatedLODs.size(); ++lodIndex)
            {
                LODSource& generated = generatedLODs[lodIndex];
                if (!generatedSuccessfully[lodIndex])
                    break;
                if (generated.indices.size() * 10 >= lods.back().indices.size() * 9)
                    break;
                // Meshoptimizer measures every target independently, so two
                // levels near the error ceiling can differ by a tiny amount in
                // the non-monotonic direction. Runtime LOD selection requires
                // coarser levels to be no more optimistic than finer levels.
                generated.geometricError = glm::max(generated.geometricError, lods.back().geometricError);
                lods.push_back(std::move(generated));
            }
        }
        MeshoptimizerTiming::Add(MeshoptimizerTiming::GeneratedLODAssembly, generatedLODStart);

        FileFormat::Model::ModelAsset asset;
        FileFormat::Model::ModelData data;
        FileFormat::Model::Mesh mesh;
        mesh.bounds = CalculateBounds(source.vertices);
        mesh.positionDecodeOffset = mesh.bounds.center - mesh.bounds.extents;
        mesh.positionDecodeExtent = mesh.bounds.extents * 2.0f;

        const auto materialStart = MeshoptimizerTiming::Start();
        data.materialSlots.reserve(source.batches.size());
        for (u32 batchIndex = 0; batchIndex < source.batches.size(); ++batchIndex)
        {
            const MaterialDescription description = DescribeMaterial(source, source.batches[batchIndex]);
            u64 instanceAssetID = FileFormat::INVALID_ASSET_ID;
            if (!QueueOrEmitMaterialAssets(runtime, description, instanceAssetID))
                return false;
            data.materialSlots.push_back({ instanceAssetID, HashString(outputPath + "/material_" + std::to_string(batchIndex)), batchIndex, 0 });
        }
        mesh.numMaterialSlots = static_cast<u32>(data.materialSlots.size());
        MeshoptimizerTiming::Add(MeshoptimizerTiming::MaterialProcessing, materialStart);

        const auto geometryStart = MeshoptimizerTiming::Start();
        if (CookSettings::CookLargeWMOLODsConcurrently && source.isWMO &&
            source.indices.size() >= CookSettings::ConcurrentWMOCookMinimumIndexCount && lods.size() > 1)
        {
            std::vector<FileFormat::Model::ModelData> cookedLODs(lods.size());
            std::vector<u8> cookedSuccessfully(lods.size(), 0);
            enki::TaskSet cookLODTasks(static_cast<u32>(lods.size()),
                [&](enki::TaskSetPartition range, u32)
                {
                    CookScratch& workerScratch = gCookScratch;
                    for (u32 lodIndex = range.start; lodIndex < range.end; ++lodIndex)
                        cookedSuccessfully[lodIndex] = AppendLOD(lods[lodIndex], source.vertices,
                            cookedLODs[lodIndex], mesh, workerScratch);
                });
            runtime->scheduler.AddTaskSetToPipe(&cookLODTasks);
            runtime->scheduler.WaitforTask(&cookLODTasks);
            for (u32 lodIndex = 0; lodIndex < cookedLODs.size(); ++lodIndex)
            {
                if (!cookedSuccessfully[lodIndex])
                    return false;
                MergeCookedLOD(data, cookedLODs[lodIndex]);
            }
        }
        else
        {
            for (const LODSource& lod : lods)
            {
                if (!AppendLOD(lod, source.vertices, data, mesh, scratch))
                    return false;
            }
        }
        mesh.numLODs = static_cast<u32>(data.meshLODs.size());
        if (!data.skinningData.empty())
            mesh.flags |= FileFormat::Model::MeshFlags_Skinned;
        data.meshes.push_back(mesh);

        for (u32 batchIndex = 0; batchIndex < source.batches.size(); ++batchIndex)
        {
            const u32 groupID = source.batches[batchIndex].groupID;
            asset.geometryGroupCount = glm::max(asset.geometryGroupCount, groupID + 1);
            for (FileFormat::Model::Submesh& submesh : data.submeshes)
            {
                if (submesh.materialSlotIndex == batchIndex)
                {
                    submesh.geometryGroupID = groupID;
                    submesh.semanticPartID = groupID;
                }
            }
        }

        if (!source.instanceSets.empty() && !source.instances.empty())
        {
            asset.flags |= FileFormat::Model::ModelFlags_HasEmbeddedInstances;
            data.embeddedInstanceSets.reserve(source.instanceSets.size());
            for (u32 setIndex = 0; setIndex < source.instanceSets.size(); ++setIndex)
            {
                const SourceInstanceSet& sourceSet = source.instanceSets[setIndex];
                FileFormat::Model::EmbeddedInstanceSet set;
                set.nameHash = HashBytes(sourceSet.name.data(), strnlen(sourceSet.name.data(), sourceSet.name.size()));
                set.instanceOffset = sourceSet.instanceOffset;
                set.numInstances = sourceSet.numInstances;
                set.stableID = setIndex;
                data.embeddedInstanceSets.push_back(set);
            }
            data.embeddedInstances.reserve(source.instances.size());
            for (u32 instanceIndex = 0; instanceIndex < source.instances.size(); ++instanceIndex)
            {
                const SourceInstance& sourceInstance = source.instances[instanceIndex];
                FileFormat::Model::EmbeddedInstance instance;
                instance.modelAssetID = sourceInstance.modelAssetID;
                instance.position = sourceInstance.position;
                instance.rotation = sourceInstance.rotation;
                instance.uniformScale = sourceInstance.scale;
                instance.color = sourceInstance.color;
                instance.stableID = instanceIndex;
            data.embeddedInstances.push_back(instance);
            }
        }
        MeshoptimizerTiming::Add(MeshoptimizerTiming::GeometryCooking, geometryStart);

        asset.bounds = mesh.bounds;
        asset.collisionAssetID = FileFormat::INVALID_ASSET_ID;
        if (!ValidateModel(asset, data))
            return false;

        const auto serializationStart = MeshoptimizerTiming::Start();
        const size_t serializedSize = asset.GetSerializedSize(data);
        std::shared_ptr<Bytebuffer> buffer = Bytebuffer::BorrowRuntime(serializedSize);
        if (!asset.Save(buffer, data) || buffer->writtenData != serializedSize)
            return false;

        FileFormat::Model::ModelAsset readBack;
        buffer->readData = 0;
        if (!FileFormat::Model::ModelAsset::Read(buffer, readBack))
            return false;
        buffer->readData = 0;
        MeshoptimizerTiming::Add(MeshoptimizerTiming::Serialization, serializationStart);

        const auto pactWriteStart = MeshoptimizerTiming::Start();
        auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, buffer->writtenData);
        const bool added = manifest.AddFile(runtime, outputPath, buffer);
        if (added)
            ModelV2AllocationRegistry::Register(HashString(outputPath), asset, data);
        MeshoptimizerTiming::Add(MeshoptimizerTiming::PactWrite, pactWriteStart);
        return added;
    }

    SourceBlendMode ConvertWMOBlendMode(u16 blendMode)
    {
        switch (blendMode)
        {
            case 0: return SourceBlendMode::Opaque;
            case 1: return SourceBlendMode::AlphaKey;
            case 2: return SourceBlendMode::Alpha;
            case 3: return SourceBlendMode::Add;
            case 4: return SourceBlendMode::Mod;
            case 5: return SourceBlendMode::Mod2X;
            case 10: return SourceBlendMode::NoAlphaAdd;
            case 13: return SourceBlendMode::BlendAdd;
            default: return SourceBlendMode::Opaque;
        }
    }
}

u64 ModelV2Builder::MeshoptimizerTimings::GetTotalNanoseconds() const
{
    return simplificationNanoseconds + tangentGenerationNanoseconds + vertexRemapNanoseconds +
        vertexCacheNanoseconds + overdrawNanoseconds + vertexFetchNanoseconds +
        meshletBuildNanoseconds + meshletOptimizationNanoseconds + meshletBoundsNanoseconds;
}

void ModelV2Builder::ResetMeshoptimizerTimings()
{
    MeshoptimizerTiming::Reset();
}

ModelV2Builder::MeshoptimizerTimings ModelV2Builder::GetMeshoptimizerTimings()
{
    MeshoptimizerTimings result;
    result.simplificationNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::Simplification);
    result.tangentGenerationNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::TangentGeneration);
    result.vertexRemapNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::VertexRemap);
    result.vertexCacheNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::VertexCache);
    result.overdrawNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::Overdraw);
    result.vertexFetchNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::VertexFetch);
    result.meshletBuildNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::MeshletBuild);
    result.meshletOptimizationNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::MeshletOptimization);
    result.meshletBoundsNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::MeshletBounds);
    result.sourcePreparationNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::SourcePreparation);
    result.baseLODAssemblyNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::BaseLODAssembly);
    result.generatedLODAssemblyNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::GeneratedLODAssembly);
    result.materialProcessingNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::MaterialProcessing);
    result.geometryCookingNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::GeometryCooking);
    result.serializationNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::Serialization);
    result.pactWriteNanoseconds = MeshoptimizerTiming::Get(MeshoptimizerTiming::PactWrite);
    return result;
}

bool ModelV2Builder::FlushPendingMaterials(Runtime* runtime)
{
    const auto materialStart = MeshoptimizerTiming::Start();
    std::unordered_map<u64, MaterialDescription> pendingMaterials;
    if constexpr (CookSettings::DeferMaterialEmission)
    {
        std::scoped_lock lock(gGeneratedMaterialMutex);
        pendingMaterials.swap(gPendingMaterials);
    }

    const FileFormat::Material::MaterialData materialData = BuildMaterialData();
    if (!ValidateMaterialData(materialData))
        return false;
    for (const auto& [assetID, description] : pendingMaterials)
    {
        u64 emittedAssetID = FileFormat::INVALID_ASSET_ID;
        if (!EmitMaterialAssets(runtime, description, materialData, emittedAssetID) || emittedAssetID != assetID)
            return false;
    }
    if (!ExportMaterialProgramManifest(runtime, materialData))
        return false;
    MeshoptimizerTiming::Add(MeshoptimizerTiming::MaterialProcessing, materialStart);
    return true;
}

bool ModelV2Builder::ConvertM2AndAdd(Runtime* runtime, std::shared_ptr<Bytebuffer>& rootBuffer, std::shared_ptr<Bytebuffer>& skinBuffer,
    M2::Layout& layout, const std::vector<u64>& textureAssetIDs, const std::string& outputPath)
{
    const auto sourcePreparationStart = MeshoptimizerTiming::Start();
    SourceModel source;
    source.vertices.resize(layout.md21.vertices.size);
    for (u32 vertexIndex = 0; vertexIndex < layout.md21.vertices.size; ++vertexIndex)
    {
        const M2::M2Vertex& input = *layout.md21.vertices.GetElement(rootBuffer, vertexIndex);
        SourceVertex& vertex = source.vertices[vertexIndex];
        vertex.position = CoordinateSpaces::ModelPosToNovus(input.position);
        const f32 normalLengthSquared = glm::length2(input.normal);
        vertex.normal = normalLengthSquared > std::numeric_limits<f32>::epsilon()
            ? CoordinateSpaces::ModelPosToNovus(input.normal * glm::inversesqrt(normalLengthSquared))
            : vec3(0.0f, 1.0f, 0.0f);
        vertex.uv0 = input.uvCords[0];
        vertex.uv1 = input.uvCords[1];
        std::copy(std::begin(input.boneIndices), std::end(input.boneIndices), vertex.jointIndices.begin());
        std::copy(std::begin(input.boneWeights), std::end(input.boneWeights), vertex.jointWeights.begin());
    }

    source.materials.resize(layout.md21.materials.size);
    for (u32 materialIndex = 0; materialIndex < layout.md21.materials.size; ++materialIndex)
    {
        const M2::M2Material& input = *layout.md21.materials.GetElement(rootBuffer, materialIndex);
        SourceMaterial& material = source.materials[materialIndex];
        material.blendMode = static_cast<SourceBlendMode>(input.blendingMode);
        material.sourceBlendMode = static_cast<u32>(input.blendingMode);
        material.kind = SourceMaterialKind::M2;
        static_assert(sizeof(input.flags) <= sizeof(material.sourceFlags));
        std::memcpy(&material.sourceFlags, &input.flags, sizeof(input.flags));
        material.isUnlit = input.flags.unLit;
        material.isUnfogged = input.flags.unFogged;
        material.isTwoSided = input.flags.disableBackfaceCulling;
    }

    source.textures.resize(layout.md21.textures.size);
    for (u32 textureIndex = 0; textureIndex < layout.md21.textures.size; ++textureIndex)
    {
        const M2::M2Texture& input = *layout.md21.textures.GetElement(rootBuffer, textureIndex);
        source.textures[textureIndex].assetID = textureIndex < textureAssetIDs.size() ? textureAssetIDs[textureIndex] : FileFormat::INVALID_ASSET_ID;
        source.textures[textureIndex].samplerID = static_cast<u16>((input.flags.wrapX ? 1u : 0u) | (input.flags.wrapY ? 2u : 0u));
    }

    std::vector<u16> vertexLookup(layout.skin.vertices.size);
    for (u32 lookupIndex = 0; lookupIndex < layout.skin.vertices.size; ++lookupIndex)
        vertexLookup[lookupIndex] = *layout.skin.vertices.GetElement(skinBuffer, lookupIndex);
    source.indices.resize(layout.skin.indices.size);
    for (u32 index = 0; index < layout.skin.indices.size; ++index)
    {
        const u16 lookupIndex = *layout.skin.indices.GetElement(skinBuffer, index);
        if (lookupIndex >= vertexLookup.size() || vertexLookup[lookupIndex] >= source.vertices.size())
            return false;
        source.indices[index] = vertexLookup[lookupIndex];
    }

    source.batches.resize(layout.skin.subMeshes.size);
    for (u32 batchIndex = 0; batchIndex < layout.skin.subMeshes.size; ++batchIndex)
    {
        const M2::M2SkinSelection& selection = *layout.skin.subMeshes.GetElement(skinBuffer, batchIndex);
        SourceBatch& batch = source.batches[batchIndex];
        batch.groupID = selection.skinSectionID;
        batch.indexStart = selection.indexStart + static_cast<u32>(selection.level) * 65'536u;
        batch.indexCount = selection.indexCount;

        for (u32 unitIndex = 0; unitIndex < layout.skin.batches.size; ++unitIndex)
        {
            const M2::M2Batch& input = *layout.skin.batches.GetElement(skinBuffer, unitIndex);
            if (input.skinSectionIndex != batchIndex)
                continue;
            SourceTextureUnit& unit = batch.textureUnits.emplace_back();
            unit.shaderID = input.shaderID;
            unit.materialIndex = input.materialIndex;
            unit.materialLayer = input.materialLayer;
            unit.authoredTextureCount = input.textureCount;
            unit.flags = input.flags;
            unit.textureIndices.reserve(input.textureCount);
            for (u32 texture = 0; texture < input.textureCount; ++texture)
            {
                const u32 lookupIndex = input.textureLookupID + texture;
                if (lookupIndex < layout.md21.textureCombinationList.size)
                    unit.textureIndices.push_back(*layout.md21.textureCombinationList.GetElement(rootBuffer, lookupIndex));
            }
        }
    }

    MeshoptimizerTiming::Add(MeshoptimizerTiming::SourcePreparation, sourcePreparationStart);
    return CookAndAdd(runtime, source, outputPath);
}

bool ModelV2Builder::ConvertWMOAndAdd(Runtime* runtime, Wmo::Layout& layout,
    const std::vector<std::array<u64, 3>>& materialTextureAssetIDs,
    const std::vector<u64>& decorationModelAssetIDs, const std::string& outputPath)
{
    const auto sourcePreparationStart = MeshoptimizerTiming::Start();
    SourceModel source;
    source.isWMO = true;
    source.materials.resize(layout.momt.data.size());
    std::vector<std::array<u32, 3>> materialTextureIndices(layout.momt.data.size());
    for (std::array<u32, 3>& indices : materialTextureIndices)
        indices.fill(std::numeric_limits<u32>::max());

    for (u32 materialIndex = 0; materialIndex < layout.momt.data.size(); ++materialIndex)
    {
        const Wmo::MOMT::Material& input = layout.momt.data[materialIndex];
        SourceMaterial& material = source.materials[materialIndex];
        material.blendMode = ConvertWMOBlendMode(input.blendMode);
        material.sourceBlendMode = input.blendMode;
        material.kind = SourceMaterialKind::WMO;
        static_assert(sizeof(input.flags) <= sizeof(material.sourceFlags));
        std::memcpy(&material.sourceFlags, &input.flags, sizeof(input.flags));
        material.isUnlit = input.flags.NoLighting;
        material.isUnfogged = input.flags.NoFog;
        material.isTwoSided = input.flags.TwoSided;
        const u16 samplerID = static_cast<u16>((!input.flags.ClampTextureS ? 1u : 0u) | (!input.flags.ClampTextureT ? 2u : 0u));
        for (u32 texture = 0; texture < 3; ++texture)
        {
            const u64 assetID = materialIndex < materialTextureAssetIDs.size()
                ? materialTextureAssetIDs[materialIndex][texture] : FileFormat::INVALID_ASSET_ID;
            if (assetID == FileFormat::INVALID_ASSET_ID)
                continue;
            materialTextureIndices[materialIndex][texture] = static_cast<u32>(source.textures.size());
            source.textures.push_back({ assetID, samplerID });
        }
    }

    u32 vertexOffset = 0;
    u32 indexOffset = 0;
    for (u32 groupIndex = 0; groupIndex < layout.groups.size(); ++groupIndex)
    {
        const Wmo::WMOGroup& group = layout.groups[groupIndex];
        const u32 numVertices = static_cast<u32>(group.movt.data.size());
        source.vertices.reserve(source.vertices.size() + numVertices);
        for (u32 vertexIndex = 0; vertexIndex < numVertices; ++vertexIndex)
        {
            SourceVertex vertex;
            vertex.position = CoordinateSpaces::ModelPosToNovus(group.movt.data[vertexIndex].position);
            if (vertexIndex < group.monr.data.size())
            {
                const vec3 inputNormal = group.monr.data[vertexIndex].normal;
                const f32 normalLengthSquared = glm::length2(inputNormal);
                if (normalLengthSquared > std::numeric_limits<f32>::epsilon())
                    vertex.normal = CoordinateSpaces::ModelPosToNovus(inputNormal * glm::inversesqrt(normalLengthSquared));
            }
            if (!group.motvs.empty() && vertexIndex < group.motvs[0].data.size())
                vertex.uv0 = group.motvs[0].data[vertexIndex].uv;
            if (group.motvs.size() > 1 && vertexIndex < group.motvs[1].data.size())
                vertex.uv1 = group.motvs[1].data[vertexIndex].uv;
            source.vertices.push_back(vertex);
        }

        source.indices.reserve(source.indices.size() + group.movi.data.size());
        for (u16 localIndex : group.movi.data)
        {
            if (localIndex >= numVertices)
                return false;
            source.indices.push_back(vertexOffset + localIndex);
        }

        for (const Wmo::MOBA::RenderBatch& input : group.moba.data)
        {
            const u32 materialIndex = (input.materialIDLarge * input.flags.UseMaterialIDLarge) +
                (input.materialIDSmall * !input.flags.UseMaterialIDLarge);
            if (materialIndex >= source.materials.size())
                continue;
            SourceBatch& batch = source.batches.emplace_back();
            batch.groupID = groupIndex;
            batch.indexStart = indexOffset + input.startIndex;
            batch.indexCount = input.indexCount;
            SourceTextureUnit& unit = batch.textureUnits.emplace_back();
            unit.shaderID = static_cast<i16>(layout.momt.data[materialIndex].shaderIndex);
            unit.materialIndex = static_cast<u16>(materialIndex);
            for (u32 textureIndex : materialTextureIndices[materialIndex])
            {
                if (textureIndex != std::numeric_limits<u32>::max())
                    unit.textureIndices.push_back(textureIndex);
            }
            unit.authoredTextureCount = static_cast<u16>(unit.textureIndices.size());
        }
        vertexOffset += numVertices;
        indexOffset += static_cast<u32>(group.movi.data.size());
    }

    source.instances.resize(layout.modd.data.size());
    for (u32 instanceIndex = 0; instanceIndex < layout.modd.data.size(); ++instanceIndex)
    {
        const Wmo::MODD::PlacementInfo& input = layout.modd.data[instanceIndex];
        SourceInstance& instance = source.instances[instanceIndex];
        instance.modelAssetID = instanceIndex < decorationModelAssetIDs.size()
            ? decorationModelAssetIDs[instanceIndex] : FileFormat::INVALID_ASSET_ID;
        instance.position = CoordinateSpaces::ModelPosToNovus(input.position);
        const vec3 placementAngles = CoordinateSpaces::DecorationRotToNovus(glm::eulerAngles(input.rotation));
        instance.rotation = glm::quat_cast(glm::eulerAngleYXZ(placementAngles.y, placementAngles.x, placementAngles.z));
        instance.scale = input.scale;
        instance.color = input.color.bgra;
    }
    source.instanceSets.resize(layout.mods.data.size());
    for (u32 setIndex = 0; setIndex < layout.mods.data.size(); ++setIndex)
    {
        const Wmo::MODS::DoodadSet& input = layout.mods.data[setIndex];
        SourceInstanceSet& set = source.instanceSets[setIndex];
        std::copy(std::begin(input.name), std::end(input.name), set.name.begin());
        set.instanceOffset = input.startIndex;
        set.numInstances = input.count;
    }

    MeshoptimizerTiming::Add(MeshoptimizerTiming::SourcePreparation, sourcePreparationStart);
    return CookAndAdd(runtime, source, outputPath);
}

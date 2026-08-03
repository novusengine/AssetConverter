#pragma once

#include <Base/Types.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

class Bytebuffer;
struct Runtime;

namespace M2
{
    struct Layout;
}

namespace Wmo
{
    struct Layout;
}

class ModelV2Builder
{
public:
    struct MeshoptimizerTimings
    {
        u64 simplificationNanoseconds = 0;
        u64 tangentGenerationNanoseconds = 0;
        u64 vertexRemapNanoseconds = 0;
        u64 vertexCacheNanoseconds = 0;
        u64 overdrawNanoseconds = 0;
        u64 vertexFetchNanoseconds = 0;
        u64 meshletBuildNanoseconds = 0;
        u64 meshletOptimizationNanoseconds = 0;
        u64 meshletBoundsNanoseconds = 0;

        // Inclusive cooker stages. These deliberately overlap the individual
        // meshoptimizer counters above so the report can separate library work
        // from allocation, packing, locking, serialization, and PACT I/O.
        u64 sourcePreparationNanoseconds = 0;
        u64 baseLODAssemblyNanoseconds = 0;
        u64 generatedLODAssemblyNanoseconds = 0;
        u64 materialProcessingNanoseconds = 0;
        u64 geometryCookingNanoseconds = 0;
        u64 serializationNanoseconds = 0;
        u64 pactWriteNanoseconds = 0;

        u64 GetTotalNanoseconds() const;
    };

    static void ResetMeshoptimizerTimings();
    static MeshoptimizerTimings GetMeshoptimizerTimings();
    static bool FlushPendingMaterials(Runtime* runtime);

    // These paths consume the parsed WoW layouts directly. ComplexModel remains
    // an independently generated compatibility output and is not an input to V2.
    static bool ConvertM2AndAdd(Runtime* runtime, std::shared_ptr<Bytebuffer>& rootBuffer, std::shared_ptr<Bytebuffer>& skinBuffer,
        M2::Layout& layout, const std::vector<u64>& textureAssetIDs, const std::string& outputPath);
    static bool ConvertWMOAndAdd(Runtime* runtime, Wmo::Layout& layout,
        const std::vector<std::array<u64, 3>>& materialTextureAssetIDs,
        const std::vector<u64>& decorationModelAssetIDs, const std::string& outputPath);
};

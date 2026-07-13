#pragma once

#include <FileFormat/Novus/NavMesh/NavMesh.h>

#include <Base/Types.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Map
{
    struct Chunk;
}

namespace Adt
{
    struct Layout;
}

namespace NavMesh
{
    struct BuildSettings
    {
        bool useMonotonePartitioning = false;
        bool useMedianFilter = true;
        f32 detailSampleDistance = Terrain::PATCH_SIZE;
        f32 maxEdgeLength = 0.0f;
        f32 maxSimplificationError = 1.8f;
        f32 minRegionRadius = 16.0f;
        f32 mergeRegionRadius = 13.333333f;
        i32 internalSubtileVoxelSize = 0;
    };

    struct BuildTimings
    {
        f64 totalSeconds = 0.0;
        f64 rasterizationSeconds = 0.0;
        f64 compactHeightfieldSeconds = 0.0;
        f64 regionSeconds = 0.0;
        f64 contourSeconds = 0.0;
        f64 polyMeshSeconds = 0.0;
        f64 detailMeshSeconds = 0.0;
        f64 detourAndOutputSeconds = 0.0;

        void Accumulate(const BuildTimings& other)
        {
            totalSeconds += other.totalSeconds;
            rasterizationSeconds += other.rasterizationSeconds;
            compactHeightfieldSeconds += other.compactHeightfieldSeconds;
            regionSeconds += other.regionSeconds;
            contourSeconds += other.contourSeconds;
            polyMeshSeconds += other.polyMeshSeconds;
            detailMeshSeconds += other.detailMeshSeconds;
            detourAndOutputSeconds += other.detourAndOutputSeconds;
        }
    };

    enum class TileBuildResult
    {
        Success,
        Empty,
        SourceMissing,
        Failed
    };

    class SourceStore
    {
    public:
        SourceStore();
        ~SourceStore();

        SourceStore(SourceStore&& other) noexcept;
        SourceStore& operator=(SourceStore&& other) noexcept;

        SourceStore(const SourceStore&) = delete;
        SourceStore& operator=(const SourceStore&) = delete;

        bool Add(u32 chunkX, u32 chunkY, const Map::Chunk& chunk);
        bool Add(u32 chunkX, u32 chunkY, const Adt::Layout& layout);
        void GetSourceIDs(std::vector<u32>& sourceIDs) const;
        void Clear();

    private:
        friend class Worker;

        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    class Worker
    {
    public:
        Worker(const SourceStore& sourceStore, const BuildSettings& buildSettings);
        ~Worker();

        TileBuildResult BuildTile(const std::filesystem::path& outputDirectory, const std::string& mapName, u32 chunkX, u32 chunkY);
        const BuildTimings& GetBuildTimings() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}

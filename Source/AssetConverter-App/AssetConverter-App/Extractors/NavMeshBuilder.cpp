#include "NavMeshBuilder.h"

#include <FileFormat/Novus/Map/MapChunk.h>
#include <FileFormat/Warcraft/ADT/Adt.h>

#include <Recast/Recast.h>
#include <Detour/DetourNavMeshBuilder.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    namespace Settings
    {
        // Keep tile boundaries exact while aligning each terrain patch to ten
        // horizontal voxels. This cuts horizontal voxel work by 36% without
        // discarding any source terrain samples.
        constexpr i32 TILE_VOXEL_SIZE = Terrain::CHUNK_NUM_CELLS_PER_STRIDE * Terrain::CELL_NUM_PATCHES_PER_STRIDE * 10;
        constexpr f32 CELL_SIZE = Terrain::CHUNK_SIZE / static_cast<f32>(TILE_VOXEL_SIZE);
        constexpr f32 CELL_HEIGHT = 0.20f;

        i32 GetWalkableHeight()
        {
            return static_cast<i32>(std::ceil(NavMesh::Agent::HEIGHT / CELL_HEIGHT));
        }

        i32 GetWalkableClimb()
        {
            return static_cast<i32>(std::floor(NavMesh::Agent::MAX_CLIMB / CELL_HEIGHT));
        }

        i32 GetWalkableRadius()
        {
            return static_cast<i32>(std::ceil(NavMesh::Agent::RADIUS / CELL_SIZE));
        }

        i32 GetBorderSize()
        {
            return GetWalkableRadius() + 3;
        }

        f32 GetBorderWorldSize()
        {
            return static_cast<f32>(GetBorderSize()) * CELL_SIZE;
        }

        i32 GetHorizontalVoxelDistance(f32 worldDistance)
        {
            return static_cast<i32>(std::ceil(worldDistance / CELL_SIZE));
        }

        bool IsValidInternalSubtileVoxelSize(i32 internalSubtileVoxelSize)
        {
            return internalSubtileVoxelSize > 0 &&
                internalSubtileVoxelSize < TILE_VOXEL_SIZE &&
                (TILE_VOXEL_SIZE % internalSubtileVoxelSize) == 0;
        }

        i32 GetMaxEdgeLength(const NavMesh::BuildSettings& buildSettings, i32 tileSize)
        {
            if (buildSettings.maxEdgeLength <= 0.0f)
                return tileSize + 1;

            return GetHorizontalVoxelDistance(buildSettings.maxEdgeLength);
        }
    }

    struct NavSourceData
    {
        std::array<f32, NavMesh::TerrainHeight::HEIGHT_COUNT> heights;
        std::array<u64, NavMesh::TerrainHeight::HOLE_COUNT> holes;
    };
    static_assert(sizeof(NavSourceData) ==
        NavMesh::TerrainHeight::HEIGHT_DATA_SIZE +
        NavMesh::TerrainHeight::HOLE_DATA_SIZE);

    struct CellTriangle
    {
        u16 vertexIDs[3];
        u8 patchID;
    };

    struct RecastBuildState
    {
        rcHeightfield* solid = nullptr;
        rcCompactHeightfield* compactHeightfield = nullptr;
        rcContourSet* contourSet = nullptr;
        rcPolyMesh* polyMesh = nullptr;
        rcPolyMeshDetail* detailMesh = nullptr;

        ~RecastBuildState()
        {
            if (solid)
                rcFreeHeightField(solid);

            if (compactHeightfield)
                rcFreeCompactHeightfield(compactHeightfield);

            if (contourSet)
                rcFreeContourSet(contourSet);

            if (polyMesh)
                rcFreePolyMesh(polyMesh);

            if (detailMesh)
                rcFreePolyMeshDetail(detailMesh);
        }
    };

    class PhaseTimer
    {
    public:
        explicit PhaseTimer(f64& elapsedSeconds)
            : _elapsedSeconds(elapsedSeconds)
            , _start(std::chrono::steady_clock::now())
        {
        }

        ~PhaseTimer()
        {
            _elapsedSeconds += std::chrono::duration<f64>(std::chrono::steady_clock::now() - _start).count();
        }

    private:
        f64& _elapsedSeconds;
        std::chrono::steady_clock::time_point _start;
    };

    vec2 GetCellVertexPosition(u32 cellID, u32 vertexID)
    {
        const i32 cellX = cellID % Terrain::CHUNK_NUM_CELLS_PER_STRIDE;
        const i32 cellY = cellID / Terrain::CHUNK_NUM_CELLS_PER_STRIDE;
        const i32 vertexX = vertexID % Terrain::CELL_GRID_ROW_SIZE;
        const i32 vertexY = vertexID / Terrain::CELL_GRID_ROW_SIZE;
        const bool isInnerVertex = vertexX > Terrain::CELL_NUM_PATCHES_PER_STRIDE;

        vec2 vertexOffset;
        vertexOffset.x = -(8.5f * isInnerVertex);
        vertexOffset.y = 0.5f * isInnerVertex;

        const ivec2 globalVertex(vertexX + cellX * Terrain::CELL_NUM_PATCHES_PER_STRIDE,
            vertexY + cellY * Terrain::CELL_NUM_PATCHES_PER_STRIDE);
        const vec2 finalPosition = (vec2(globalVertex) + vertexOffset) * Terrain::PATCH_SIZE;
        return vec2(finalPosition.x, -finalPosition.y);
    }

    vec2 GetChunkWorldOrigin(u32 chunkX, u32 chunkY)
    {
        return -Terrain::MAP_HALF_SIZE + (vec2(chunkX, chunkY) * Terrain::CHUNK_SIZE);
    }

    u32 GetChunkID(u32 chunkX, u32 chunkY)
    {
        return chunkX + chunkY * Terrain::CHUNK_NUM_PER_MAP_STRIDE;
    }

    const std::array<vec2, Terrain::CHUNK_NUM_CELLS * Terrain::CELL_TOTAL_GRID_SIZE>& GetChunkVertexPositions()
    {
        static const std::array<vec2, Terrain::CHUNK_NUM_CELLS * Terrain::CELL_TOTAL_GRID_SIZE> positions = []
        {
            std::array<vec2, Terrain::CHUNK_NUM_CELLS * Terrain::CELL_TOTAL_GRID_SIZE> result;
            for (u32 cellID = 0; cellID < Terrain::CHUNK_NUM_CELLS; cellID++)
            {
                const size_t vertexOffset = static_cast<size_t>(cellID) * Terrain::CELL_TOTAL_GRID_SIZE;
                for (u32 vertexID = 0; vertexID < Terrain::CELL_TOTAL_GRID_SIZE; vertexID++)
                {
                    result[vertexOffset + vertexID] = GetCellVertexPosition(cellID, vertexID);
                }
            }
            return result;
        }();

        return positions;
    }

    const std::array<CellTriangle, Terrain::CELL_NUM_TRIANGLES>& GetCellTriangles()
    {
        static const std::array<CellTriangle, Terrain::CELL_NUM_TRIANGLES> triangles = []
        {
            std::array<CellTriangle, Terrain::CELL_NUM_TRIANGLES> result;
            for (u32 triangleID = 0; triangleID < Terrain::CELL_NUM_TRIANGLES; triangleID++)
            {
                const u32 patchID = triangleID / 4;
                const u32 patchRow = patchID / Terrain::CELL_NUM_PATCHES_PER_STRIDE;
                const u32 patchColumn = patchID % Terrain::CELL_NUM_PATCHES_PER_STRIDE;

                u16 patchVertexIDs[5];
                patchVertexIDs[0] = static_cast<u16>(patchColumn + patchRow * Terrain::CELL_GRID_ROW_SIZE);
                patchVertexIDs[1] = patchVertexIDs[0] + 1;
                patchVertexIDs[2] = patchVertexIDs[0] + Terrain::CELL_GRID_ROW_SIZE;
                patchVertexIDs[3] = patchVertexIDs[2] + 1;
                patchVertexIDs[4] = patchVertexIDs[0] + Terrain::CELL_OUTER_GRID_STRIDE;

                const u32 triangleWithinPatch = triangleID % 4;
                const u32 offsetX = triangleWithinPatch > 1;
                const u32 offsetY = triangleWithinPatch == 0 || triangleWithinPatch == 3;

                CellTriangle& triangle = result[triangleID];
                triangle.vertexIDs[0] = patchVertexIDs[4];
                triangle.vertexIDs[1] = patchVertexIDs[offsetX * 2 + offsetY];
                triangle.vertexIDs[2] = patchVertexIDs[(!offsetY) * 2 + offsetX];
                triangle.patchID = static_cast<u8>(patchID);
            }
            return result;
        }();

        return triangles;
    }

    void AppendSourceGeometry(const NavSourceData& source, u32 chunkX, u32 chunkY, const vec2& paddedMin, const vec2& paddedMax, std::vector<f32>& vertices, std::vector<i32>& triangles)
    {
        const vec2 chunkOrigin = GetChunkWorldOrigin(chunkX, chunkY);
        const auto& chunkVertexPositions = GetChunkVertexPositions();
        const auto& cellTriangles = GetCellTriangles();

        for (u32 cellID = 0; cellID < Terrain::CHUNK_NUM_CELLS; cellID++)
        {
            const u32 cellX = cellID % Terrain::CHUNK_NUM_CELLS_PER_STRIDE;
            const u32 cellY = cellID / Terrain::CHUNK_NUM_CELLS_PER_STRIDE;
            const vec2 cellMin = chunkOrigin + vec2(cellX, cellY) * Terrain::CELL_SIZE;
            const vec2 cellMax = cellMin + Terrain::CELL_SIZE;

            if (cellMax.x < paddedMin.x ||
                cellMax.y < paddedMin.y ||
                cellMin.x > paddedMax.x ||
                cellMin.y > paddedMax.y)
            {
                continue;
            }

            const i32 baseVertex = static_cast<i32>(vertices.size() / 3);
            const size_t heightOffset = static_cast<size_t>(cellID) * Terrain::CELL_TOTAL_GRID_SIZE;

            for (u32 vertexID = 0; vertexID < Terrain::CELL_TOTAL_GRID_SIZE; vertexID++)
            {
                const vec2& localPosition = chunkVertexPositions[heightOffset + vertexID];
                vertices.push_back(chunkOrigin.x + localPosition.x);
                vertices.push_back(source.heights[heightOffset + vertexID]);
                vertices.push_back(chunkOrigin.y - localPosition.y);
            }

            const u64 holes = source.holes[cellID];
            for (const CellTriangle& cellTriangle : cellTriangles)
            {
                if ((holes & (1ull << cellTriangle.patchID)) != 0)
                    continue;

                // Recast requires counter-clockwise winding with a positive Y normal.
                triangles.push_back(baseVertex + cellTriangle.vertexIDs[0]);
                triangles.push_back(baseVertex + cellTriangle.vertexIDs[1]);
                triangles.push_back(baseVertex + cellTriangle.vertexIDs[2]);
            }
        }
    }

    bool WriteTerrainHeightTile(const std::filesystem::path& path, u32 chunkX, u32 chunkY, const NavSourceData& source)
    {
        const vec2 chunkOrigin = GetChunkWorldOrigin(chunkX, chunkY);

        NavMesh::TerrainHeight::Header header;
        header.headerSize = sizeof(header);
        header.chunkX = chunkX;
        header.chunkY = chunkY;
        header.originX = chunkOrigin.x;
        header.originZ = chunkOrigin.y;
        header.chunkSize = Terrain::CHUNK_SIZE;
        header.cellsPerChunkStride = Terrain::CHUNK_NUM_CELLS_PER_STRIDE;
        header.outerVerticesPerCellStride = Terrain::CELL_OUTER_GRID_STRIDE;
        header.innerVerticesPerCellStride = Terrain::CELL_INNER_GRID_STRIDE;
        header.verticesPerCell = Terrain::CELL_TOTAL_GRID_SIZE;
        header.heightCount = static_cast<u32>(source.heights.size());
        header.holeCount = static_cast<u32>(source.holes.size());
        if (!NavMesh::TerrainHeight::IsValidHeader(header))
            return false;

        std::ofstream output(path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!output)
            return false;

        // Heights retain the native 145-value ADT cell order used by
        // GetCellVertexPosition. Hole bit N masks terrain patch N in the cell.
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(source.heights.data()), sizeof(source.heights));
        output.write(reinterpret_cast<const char*>(source.holes.data()), sizeof(source.holes));
        output.flush();
        return output.good();
    }

    rcConfig CreateBaseConfig(const NavMesh::BuildSettings& buildSettings)
    {
        rcConfig config{};
        config.cs = Settings::CELL_SIZE;
        config.ch = Settings::CELL_HEIGHT;
        config.walkableSlopeAngle = NavMesh::Agent::MAX_SLOPE;
        config.walkableHeight = Settings::GetWalkableHeight();
        config.walkableClimb = Settings::GetWalkableClimb();
        config.walkableRadius = Settings::GetWalkableRadius();
        config.borderSize = Settings::GetBorderSize();
        config.tileSize = Settings::TILE_VOXEL_SIZE;
        config.width = config.tileSize + (config.borderSize * 2);
        config.height = config.tileSize + (config.borderSize * 2);
        config.maxEdgeLen = Settings::GetMaxEdgeLength(buildSettings, config.tileSize);
        config.maxSimplificationError = std::max(0.0f, buildSettings.maxSimplificationError);
        config.minRegionArea = rcSqr(Settings::GetHorizontalVoxelDistance(std::max(0.0f, buildSettings.minRegionRadius)));
        config.mergeRegionArea = rcSqr(Settings::GetHorizontalVoxelDistance(std::max(0.0f, buildSettings.mergeRegionRadius)));
        config.maxVertsPerPoly = 6;
        config.detailSampleDist = std::max(0.0f, buildSettings.detailSampleDistance);
        config.detailSampleMaxError = config.ch;
        return config;
    }

    void SetTileBounds(rcConfig& config, u32 chunkX, u32 chunkY, f32 minY, f32 maxY)
    {
        const vec2 tileOrigin = GetChunkWorldOrigin(chunkX, chunkY);
        const f32 borderWorldSize = Settings::GetBorderWorldSize();
        config.bmin[0] = tileOrigin.x - borderWorldSize;
        config.bmin[1] = minY;
        config.bmin[2] = tileOrigin.y - borderWorldSize;
        config.bmax[0] = tileOrigin.x + Terrain::CHUNK_SIZE + borderWorldSize;
        config.bmax[1] = std::max(maxY, minY + config.ch);
        config.bmax[2] = tileOrigin.y + Terrain::CHUNK_SIZE + borderWorldSize;
    }

    void SetSubtileBounds(rcConfig& config, const NavMesh::BuildSettings& buildSettings, u32 chunkX, u32 chunkY, i32 subtileX, i32 subtileY, i32 internalSubtileVoxelSize, f32 minY, f32 maxY)
    {
        const vec2 tileOrigin = GetChunkWorldOrigin(chunkX, chunkY);
        const f32 subtileWorldSize = static_cast<f32>(internalSubtileVoxelSize) * config.cs;
        const f32 borderWorldSize = static_cast<f32>(config.borderSize) * config.cs;
        config.tileSize = internalSubtileVoxelSize;
        config.width = config.tileSize + (config.borderSize * 2);
        config.height = config.tileSize + (config.borderSize * 2);
        config.maxEdgeLen = Settings::GetMaxEdgeLength(buildSettings, config.tileSize);
        config.bmin[0] = tileOrigin.x + static_cast<f32>(subtileX) * subtileWorldSize - borderWorldSize;
        config.bmin[1] = minY;
        config.bmin[2] = tileOrigin.y + static_cast<f32>(subtileY) * subtileWorldSize - borderWorldSize;
        config.bmax[0] = tileOrigin.x + static_cast<f32>(subtileX + 1) * subtileWorldSize + borderWorldSize;
        config.bmax[1] = std::max(maxY, minY + config.ch);
        config.bmax[2] = tileOrigin.y + static_cast<f32>(subtileY + 1) * subtileWorldSize + borderWorldSize;
    }

    bool TriangleIntersectsBounds(const std::vector<f32>& vertices, const i32* triangle, const rcConfig& config)
    {
        f32 minX = std::numeric_limits<f32>::max();
        f32 minZ = std::numeric_limits<f32>::max();
        f32 maxX = std::numeric_limits<f32>::lowest();
        f32 maxZ = std::numeric_limits<f32>::lowest();

        for (u32 vertexIndex = 0; vertexIndex < 3; vertexIndex++)
        {
            const f32* vertex = &vertices[static_cast<size_t>(triangle[vertexIndex]) * 3];
            minX = std::min(minX, vertex[0]);
            minZ = std::min(minZ, vertex[2]);
            maxX = std::max(maxX, vertex[0]);
            maxZ = std::max(maxZ, vertex[2]);
        }

        return maxX >= config.bmin[0] &&
            maxZ >= config.bmin[2] &&
            minX <= config.bmax[0] &&
            minZ <= config.bmax[2];
    }

    void AppendTrianglesForBounds(const std::vector<f32>& vertices, const std::vector<i32>& sourceTriangles, const rcConfig& config, std::vector<i32>& filteredTriangles)
    {
        filteredTriangles.clear();
        filteredTriangles.reserve(sourceTriangles.size() / 8);

        for (size_t triangleIndex = 0; triangleIndex < sourceTriangles.size(); triangleIndex += 3)
        {
            const i32* triangle = &sourceTriangles[triangleIndex];
            if (!TriangleIntersectsBounds(vertices, triangle, config))
                continue;

            filteredTriangles.push_back(triangle[0]);
            filteredTriangles.push_back(triangle[1]);
            filteredTriangles.push_back(triangle[2]);
        }
    }

    NavMesh::TileBuildResult BuildRecastMesh(rcContext& context, NavMesh::BuildTimings& timings, const NavMesh::BuildSettings& buildSettings, const rcConfig& config, const std::vector<f32>& vertices, const std::vector<i32>& triangles, RecastBuildState& state)
    {
        if (vertices.empty() || triangles.empty())
            return NavMesh::TileBuildResult::Empty;

        {
            PhaseTimer phaseTimer(timings.rasterizationSeconds);
            state.solid = rcAllocHeightfield();
            if (!state.solid || !rcCreateHeightfield(&context, *state.solid, config.width, config.height, config.bmin, config.bmax, config.cs, config.ch))
                return NavMesh::TileBuildResult::Failed;

            const i32 triangleCount = static_cast<i32>(triangles.size() / 3);
            std::vector<u8> areas(triangleCount, RC_NULL_AREA);
            rcMarkWalkableTriangles(&context, config.walkableSlopeAngle, vertices.data(), static_cast<i32>(vertices.size() / 3), triangles.data(), triangleCount, areas.data());
            if (!rcRasterizeTriangles(&context, vertices.data(), static_cast<i32>(vertices.size() / 3), triangles.data(), areas.data(), triangleCount, *state.solid, config.walkableClimb))
                return NavMesh::TileBuildResult::Failed;
        }

        {
            PhaseTimer phaseTimer(timings.compactHeightfieldSeconds);
            rcFilterLowHangingWalkableObstacles(&context, config.walkableClimb, *state.solid);
            rcFilterLedgeSpans(&context, config.walkableHeight, config.walkableClimb, *state.solid);
            rcFilterWalkableLowHeightSpans(&context, config.walkableHeight, *state.solid);

            state.compactHeightfield = rcAllocCompactHeightfield();
            if (!state.compactHeightfield || !rcBuildCompactHeightfield(&context, config.walkableHeight, config.walkableClimb, *state.solid, *state.compactHeightfield))
                return NavMesh::TileBuildResult::Failed;

            rcFreeHeightField(state.solid);
            state.solid = nullptr;
        }

        {
            PhaseTimer phaseTimer(timings.regionSeconds);
            if (!rcErodeWalkableArea(&context, config.walkableRadius, *state.compactHeightfield))
                return NavMesh::TileBuildResult::Failed;

            if (buildSettings.useMedianFilter && !rcMedianFilterWalkableArea(&context, *state.compactHeightfield))
                return NavMesh::TileBuildResult::Failed;

            const bool regionsBuilt = buildSettings.useMonotonePartitioning
                ? rcBuildRegionsMonotone(&context, *state.compactHeightfield, config.borderSize, config.minRegionArea, config.mergeRegionArea)
                : rcBuildDistanceField(&context, *state.compactHeightfield) &&
                    rcBuildRegions(&context, *state.compactHeightfield, config.borderSize, config.minRegionArea, config.mergeRegionArea);
            if (!regionsBuilt)
                return NavMesh::TileBuildResult::Failed;
        }

        {
            PhaseTimer phaseTimer(timings.contourSeconds);
            state.contourSet = rcAllocContourSet();
            if (!state.contourSet || !rcBuildContours(&context, *state.compactHeightfield, config.maxSimplificationError, config.maxEdgeLen, *state.contourSet))
                return NavMesh::TileBuildResult::Failed;
        }

        {
            PhaseTimer phaseTimer(timings.polyMeshSeconds);
            state.polyMesh = rcAllocPolyMesh();
            if (!state.polyMesh || !rcBuildPolyMesh(&context, *state.contourSet, config.maxVertsPerPoly, *state.polyMesh))
                return NavMesh::TileBuildResult::Failed;

            rcFreeContourSet(state.contourSet);
            state.contourSet = nullptr;

            if (state.polyMesh->npolys == 0)
                return NavMesh::TileBuildResult::Empty;

            for (i32 i = 0; i < state.polyMesh->npolys; i++)
            {
                if (state.polyMesh->areas[i] == RC_WALKABLE_AREA)
                    state.polyMesh->flags[i] |= 0x1;
            }
        }

        {
            PhaseTimer phaseTimer(timings.detailMeshSeconds);
            state.detailMesh = rcAllocPolyMeshDetail();
            if (!state.detailMesh || !rcBuildPolyMeshDetail(&context, *state.polyMesh, *state.compactHeightfield, config.detailSampleDist, config.detailSampleMaxError, *state.detailMesh))
                return NavMesh::TileBuildResult::Failed;

            rcFreeCompactHeightfield(state.compactHeightfield);
            state.compactHeightfield = nullptr;

            if (state.detailMesh->nmeshes == 0)
                return NavMesh::TileBuildResult::Empty;
        }

        return NavMesh::TileBuildResult::Success;
    }

    NavMesh::TileBuildResult WriteDetourTile(NavMesh::BuildTimings& timings, const std::filesystem::path& path, u32 chunkX, u32 chunkY, const rcConfig& config, const rcPolyMesh& polyMesh, const rcPolyMeshDetail& detailMesh)
    {
        PhaseTimer outputTimer(timings.detourAndOutputSeconds);
        dtNavMeshCreateParams params{};
        params.verts = polyMesh.verts;
        params.vertCount = polyMesh.nverts;
        params.polys = polyMesh.polys;
        params.polyAreas = polyMesh.areas;
        params.polyFlags = polyMesh.flags;
        params.polyCount = polyMesh.npolys;
        params.nvp = polyMesh.nvp;
        params.detailMeshes = detailMesh.meshes;
        params.detailVerts = detailMesh.verts;
        params.detailVertsCount = detailMesh.nverts;
        params.detailTris = detailMesh.tris;
        params.detailTriCount = detailMesh.ntris;
        params.walkableHeight = NavMesh::Agent::HEIGHT;
        params.walkableRadius = NavMesh::Agent::RADIUS;
        params.walkableClimb = NavMesh::Agent::MAX_CLIMB;
        params.cs = config.cs;
        params.ch = config.ch;
        params.buildBvTree = true;
        params.tileX = static_cast<i32>(chunkX);
        params.tileY = static_cast<i32>(chunkY);
        params.tileLayer = NavMesh::TILE_LAYER;
        rcVcopy(params.bmin, polyMesh.bmin);
        rcVcopy(params.bmax, polyMesh.bmax);

        u8* rawNavData = nullptr;
        i32 navDataSize = 0;
        if (!dtCreateNavMeshData(&params, &rawNavData, &navDataSize) || !rawNavData || navDataSize <= 0)
        {
            if (rawNavData)
                dtFree(rawNavData);

            return NavMesh::TileBuildResult::Failed;
        }

        std::unique_ptr<u8, decltype(&dtFree)> navData(rawNavData, &dtFree);
        std::ofstream output(path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!output)
            return NavMesh::TileBuildResult::Failed;

        output.write(reinterpret_cast<const char*>(navData.get()), navDataSize);
        output.flush();
        return output.good() ? NavMesh::TileBuildResult::Success : NavMesh::TileBuildResult::Failed;
    }

    NavMesh::TileBuildResult BuildSingleNavMeshTile(rcContext& context, NavMesh::BuildTimings& timings, const NavMesh::BuildSettings& buildSettings, const std::filesystem::path& path, u32 chunkX, u32 chunkY, const rcConfig& config, const std::vector<f32>& vertices, const std::vector<i32>& triangles)
    {
        RecastBuildState state;
        const NavMesh::TileBuildResult buildResult = BuildRecastMesh(context, timings, buildSettings, config, vertices, triangles, state);
        if (buildResult != NavMesh::TileBuildResult::Success)
            return buildResult;

        return WriteDetourTile(timings, path, chunkX, chunkY, config, *state.polyMesh, *state.detailMesh);
    }

    NavMesh::TileBuildResult BuildSubtiledNavMeshTile(rcContext& context, NavMesh::BuildTimings& timings, const NavMesh::BuildSettings& buildSettings, const std::filesystem::path& path, u32 chunkX, u32 chunkY, const rcConfig& outerConfig, const std::vector<f32>& vertices, const std::vector<i32>& triangles, f32 minY, f32 maxY)
    {
        const i32 subtilesPerAxis = Settings::TILE_VOXEL_SIZE / buildSettings.internalSubtileVoxelSize;
        std::vector<std::unique_ptr<RecastBuildState>> substates;
        std::vector<rcPolyMesh*> polyMeshes;
        std::vector<rcPolyMeshDetail*> detailMeshes;
        std::vector<i32> filteredTriangles;

        substates.reserve(static_cast<size_t>(subtilesPerAxis) * subtilesPerAxis);
        polyMeshes.reserve(substates.capacity());
        detailMeshes.reserve(substates.capacity());

        for (i32 subtileY = 0; subtileY < subtilesPerAxis; subtileY++)
        {
            for (i32 subtileX = 0; subtileX < subtilesPerAxis; subtileX++)
            {
                rcConfig subtileConfig = outerConfig;
                SetSubtileBounds(subtileConfig, buildSettings, chunkX, chunkY, subtileX, subtileY, buildSettings.internalSubtileVoxelSize, minY, maxY);
                AppendTrianglesForBounds(vertices, triangles, subtileConfig, filteredTriangles);
                if (filteredTriangles.empty())
                    continue;

                std::unique_ptr<RecastBuildState> state = std::make_unique<RecastBuildState>();
                const NavMesh::TileBuildResult buildResult = BuildRecastMesh(context, timings, buildSettings, subtileConfig, vertices, filteredTriangles, *state);
                if (buildResult == NavMesh::TileBuildResult::Failed)
                    return buildResult;

                if (buildResult == NavMesh::TileBuildResult::Empty)
                    continue;

                polyMeshes.push_back(state->polyMesh);
                detailMeshes.push_back(state->detailMesh);
                substates.push_back(std::move(state));
            }
        }

        if (substates.empty())
            return NavMesh::TileBuildResult::Empty;

        RecastBuildState mergedState;
        {
            PhaseTimer phaseTimer(timings.polyMeshSeconds);
            mergedState.polyMesh = rcAllocPolyMesh();
            if (!mergedState.polyMesh || !rcMergePolyMeshes(&context, polyMeshes.data(), static_cast<i32>(polyMeshes.size()), *mergedState.polyMesh))
                return NavMesh::TileBuildResult::Failed;

            if (mergedState.polyMesh->npolys == 0)
                return NavMesh::TileBuildResult::Empty;
        }

        {
            PhaseTimer phaseTimer(timings.detailMeshSeconds);
            mergedState.detailMesh = rcAllocPolyMeshDetail();
            if (!mergedState.detailMesh || !rcMergePolyMeshDetails(&context, detailMeshes.data(), static_cast<i32>(detailMeshes.size()), *mergedState.detailMesh))
                return NavMesh::TileBuildResult::Failed;

            if (mergedState.detailMesh->nmeshes == 0)
                return NavMesh::TileBuildResult::Empty;
        }

        return WriteDetourTile(timings, path, chunkX, chunkY, outerConfig, *mergedState.polyMesh, *mergedState.detailMesh);
    }

    NavMesh::TileBuildResult BuildNavMeshTile(rcContext& context, NavMesh::BuildTimings& timings, const NavMesh::BuildSettings& buildSettings, const std::filesystem::path& path, u32 chunkX, u32 chunkY, const std::vector<f32>& vertices, const std::vector<i32>& triangles)
    {
        PhaseTimer totalTimer(timings.totalSeconds);
        if (vertices.empty() || triangles.empty())
            return NavMesh::TileBuildResult::Empty;

        vec3 geometryMin;
        vec3 geometryMax;
        rcCalcBounds(vertices.data(), static_cast<i32>(vertices.size() / 3), &geometryMin.x, &geometryMax.x);

        rcConfig config = CreateBaseConfig(buildSettings);
        SetTileBounds(config, chunkX, chunkY, geometryMin.y, geometryMax.y);

        if (Settings::IsValidInternalSubtileVoxelSize(buildSettings.internalSubtileVoxelSize))
            return BuildSubtiledNavMeshTile(context, timings, buildSettings, path, chunkX, chunkY, config, vertices, triangles, geometryMin.y, geometryMax.y);

        return BuildSingleNavMeshTile(context, timings, buildSettings, path, chunkX, chunkY, config, vertices, triangles);
    }
}

struct NavMesh::SourceStore::Impl
{
    std::array<std::unique_ptr<NavSourceData>, Terrain::CHUNK_NUM_PER_MAP> sources;
};

struct NavMesh::Worker::Impl
{
    Impl(const SourceStore& sourceStore, const BuildSettings& buildSettings)
        : sourceStore(sourceStore)
        , buildSettings(buildSettings)
    {
        constexpr u32 maxCellsPerTile = (Terrain::CHUNK_NUM_CELLS_PER_STRIDE + 2) * (Terrain::CHUNK_NUM_CELLS_PER_STRIDE + 2);
        vertices.reserve(maxCellsPerTile * Terrain::CELL_TOTAL_GRID_SIZE * 3);
        triangles.reserve(maxCellsPerTile * Terrain::CELL_NUM_INDICES);
    }

    const SourceStore& sourceStore;
    BuildSettings buildSettings;
    rcContext context;
    BuildTimings timings;
    std::vector<f32> vertices;
    std::vector<i32> triangles;
};

NavMesh::SourceStore::SourceStore()
    : _impl(std::make_unique<Impl>())
{
}

NavMesh::SourceStore::~SourceStore() = default;
NavMesh::SourceStore::SourceStore(SourceStore&& other) noexcept = default;
NavMesh::SourceStore& NavMesh::SourceStore::operator=(SourceStore&& other) noexcept = default;

bool NavMesh::SourceStore::Add(u32 chunkX, u32 chunkY, const Map::Chunk& chunk)
{
    if (!_impl ||
        chunkX >= Terrain::CHUNK_NUM_PER_MAP_STRIDE ||
        chunkY >= Terrain::CHUNK_NUM_PER_MAP_STRIDE)
    {
        return false;
    }

    std::unique_ptr<NavSourceData>& source = _impl->sources[GetChunkID(chunkX, chunkY)];
    if (source)
        return false;

    // The extraction task owns each chunk ID exactly once, so parallel workers
    // write distinct array elements before the navmesh pass begins.
    source = std::make_unique<NavSourceData>();
    std::memcpy(source->heights.data(), chunk.cellsData.heightField, sizeof(chunk.cellsData.heightField));
    std::memcpy(source->holes.data(), chunk.cellsData.holes, sizeof(chunk.cellsData.holes));
    return true;
}

bool NavMesh::SourceStore::Add(u32 chunkX, u32 chunkY, const Adt::Layout& layout)
{
    if (!_impl ||
        chunkX >= Terrain::CHUNK_NUM_PER_MAP_STRIDE ||
        chunkY >= Terrain::CHUNK_NUM_PER_MAP_STRIDE ||
        layout.cellInfos.size() < Terrain::CHUNK_NUM_CELLS)
    {
        return false;
    }

    std::unique_ptr<NavSourceData>& source = _impl->sources[GetChunkID(chunkX, chunkY)];
    if (source)
        return false;

    source = std::make_unique<NavSourceData>();
    for (u32 cellID = 0; cellID < Terrain::CHUNK_NUM_CELLS; cellID++)
    {
        const Adt::CellInfo& cellInfo = layout.cellInfos[cellID];
        const Adt::MCNK& chunkInfo = cellInfo.mcnk;
        const size_t heightOffset = static_cast<size_t>(cellID) * Terrain::CELL_TOTAL_GRID_SIZE;
        for (u32 vertexID = 0; vertexID < Terrain::CELL_TOTAL_GRID_SIZE; vertexID++)
        {
            source->heights[heightOffset + vertexID] = chunkInfo.position.z + cellInfo.mcvt.heightMap[vertexID];
        }

        if (chunkInfo.flags.HighResHoles)
        {
            source->holes[cellID] = chunkInfo.holesHighResA | (static_cast<u64>(chunkInfo.holesHighResB) << 32);
            continue;
        }

        u64 highResolutionHoles = 0;
        for (u32 row = 0; row < 4; row++)
        {
            for (u32 column = 0; column < 4; column++)
            {
                const u32 lowResolutionBit = row * 4 + column;
                if ((chunkInfo.holesLowRes & (1u << lowResolutionBit)) == 0)
                    continue;

                const u32 highResolutionBit = row * 16 + column * 2;
                highResolutionHoles |= 1ull << highResolutionBit;
                highResolutionHoles |= 1ull << (highResolutionBit + 1);
                highResolutionHoles |= 1ull << (highResolutionBit + Terrain::CELL_INNER_GRID_STRIDE);
                highResolutionHoles |= 1ull << (highResolutionBit + Terrain::CELL_INNER_GRID_STRIDE + 1);
            }
        }
        source->holes[cellID] = highResolutionHoles;
    }

    return true;
}

void NavMesh::SourceStore::GetSourceIDs(std::vector<u32>& sourceIDs) const
{
    sourceIDs.clear();
    if (!_impl)
        return;

    sourceIDs.reserve(Terrain::CHUNK_NUM_PER_MAP);
    for (u32 sourceID = 0; sourceID < _impl->sources.size(); sourceID++)
    {
        if (_impl->sources[sourceID])
            sourceIDs.push_back(sourceID);
    }
}

void NavMesh::SourceStore::Clear()
{
    if (_impl)
    {
        for (std::unique_ptr<NavSourceData>& source : _impl->sources)
        {
            source.reset();
        }
    }
}

NavMesh::Worker::Worker(const SourceStore& sourceStore, const BuildSettings& buildSettings)
    : _impl(std::make_unique<Impl>(sourceStore, buildSettings))
{
}

NavMesh::Worker::~Worker() = default;

const NavMesh::BuildTimings& NavMesh::Worker::GetBuildTimings() const
{
    return _impl->timings;
}

NavMesh::TileBuildResult NavMesh::Worker::BuildTile(const std::filesystem::path& outputDirectory, const std::string& mapName, u32 chunkX, u32 chunkY)
{
    const NavSourceData* targetSource = _impl->sourceStore._impl->sources[GetChunkID(chunkX, chunkY)].get();
    if (!targetSource)
        return TileBuildResult::SourceMissing;

    _impl->vertices.clear();
    _impl->triangles.clear();

    const vec2 tileMin = GetChunkWorldOrigin(chunkX, chunkY);
    const vec2 tileMax = tileMin + Terrain::CHUNK_SIZE;
    const f32 borderWorldSize = Settings::GetBorderWorldSize();
    const vec2 paddedMin = tileMin - borderWorldSize;
    const vec2 paddedMax = tileMax + borderWorldSize;

    const i32 minChunkX = std::max(0, static_cast<i32>(chunkX) - 1);
    const i32 minChunkY = std::max(0, static_cast<i32>(chunkY) - 1);
    const i32 maxChunkX = std::min(static_cast<i32>(Terrain::CHUNK_NUM_PER_MAP_STRIDE) - 1, static_cast<i32>(chunkX) + 1);
    const i32 maxChunkY = std::min(static_cast<i32>(Terrain::CHUNK_NUM_PER_MAP_STRIDE) - 1, static_cast<i32>(chunkY) + 1);

    for (i32 sourceChunkY = minChunkY; sourceChunkY <= maxChunkY; sourceChunkY++)
    {
        for (i32 sourceChunkX = minChunkX; sourceChunkX <= maxChunkX; sourceChunkX++)
        {
            const NavSourceData* source = _impl->sourceStore._impl->sources[GetChunkID(sourceChunkX, sourceChunkY)].get();
            if (!source)
                continue;

            AppendSourceGeometry(*source, sourceChunkX, sourceChunkY, paddedMin, paddedMax, _impl->vertices, _impl->triangles);
        }
    }

    const std::string tileName = mapName + "_" + std::to_string(chunkX) + "_" + std::to_string(chunkY);
    const std::filesystem::path navMeshOutputPath = outputDirectory / (tileName + NavMesh::TILE_FILE_EXTENSION);
    const TileBuildResult buildResult = BuildNavMeshTile(_impl->context, _impl->timings, _impl->buildSettings, navMeshOutputPath, chunkX, chunkY, _impl->vertices, _impl->triangles);
    if (buildResult != TileBuildResult::Success)
        return buildResult;

    const std::filesystem::path heightOutputPath = outputDirectory / (tileName + NavMesh::TerrainHeight::FILE_EXTENSION);
    if (!WriteTerrainHeightTile(heightOutputPath, chunkX, chunkY, *targetSource))
    {
        std::error_code error;
        std::filesystem::remove(navMeshOutputPath, error);
        std::filesystem::remove(heightOutputPath, error);
        return TileBuildResult::Failed;
    }

    return TileBuildResult::Success;
}

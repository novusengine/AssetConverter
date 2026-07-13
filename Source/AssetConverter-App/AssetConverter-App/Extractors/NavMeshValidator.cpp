#include "NavMeshValidator.h"

#include <FileFormat/Novus/NavMesh/NavMesh.h>

#include <Base/Util/DebugHandler.h>

#include <FileFormat/Shared.h>

#include <Detour/DetourNavMesh.h>
#include <Detour/DetourNavMeshQuery.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <utility>

namespace
{
    static_assert(NavMesh::USE_64BIT_POLY_REFS);
    static_assert(sizeof(dtPolyRef) == sizeof(u64), "AssetConverter requires 64-bit Detour polygon references");

    constexpr f32 PORTAL_HORIZONTAL_EPSILON = 0.01f;
    constexpr u16 WALKABLE_POLY_FLAG = 0x1;
    constexpr u32 MAX_DETAILED_FAILURES = 16;
    struct TileData
    {
        std::vector<u8> bytes;

        dtMeshHeader* GetHeader()
        {
            return reinterpret_cast<dtMeshHeader*>(bytes.data());
        }
    };

    struct PortalEdge
    {
        const f32* start = nullptr;
        const f32* end = nullptr;
    };

    struct CrossTileLink
    {
        dtPolyRef source = 0;
        dtPolyRef target = 0;

        explicit operator bool() const
        {
            return source != 0 && target != 0;
        }
    };

    enum class PairValidationResult
    {
        Valid,
        NotTraversable,
        Failed
    };

    std::filesystem::path GetTilePath(const std::filesystem::path& outputDirectory, const std::string& mapName, u32 chunkX, u32 chunkY)
    {
        return outputDirectory / (mapName + "_" + std::to_string(chunkX) + "_" + std::to_string(chunkY) + NavMesh::TILE_FILE_EXTENSION);
    }

    bool LoadTile(const std::filesystem::path& path, u32 expectedChunkX, u32 expectedChunkY, TileData& tileData)
    {
        std::error_code error;
        const uintmax_t fileSize = std::filesystem::file_size(path, error);
        if (error ||
            fileSize < sizeof(dtMeshHeader) ||
            fileSize > static_cast<uintmax_t>(std::numeric_limits<i32>::max()))
        {
            return false;
        }

        std::ifstream input(path, std::ios::in | std::ios::binary);
        if (!input)
            return false;

        tileData.bytes.resize(static_cast<size_t>(fileSize));
        input.read(reinterpret_cast<char*>(tileData.bytes.data()), static_cast<std::streamsize>(tileData.bytes.size()));
        if (!input)
            return false;

        const dtMeshHeader* header = tileData.GetHeader();
        return header->magic == DT_NAVMESH_MAGIC &&
            header->version == DT_NAVMESH_VERSION &&
            header->x == static_cast<i32>(expectedChunkX) &&
            header->y == static_cast<i32>(expectedChunkY) &&
            header->layer == NavMesh::TILE_LAYER &&
            header->polyCount > 0;
    }

    f32 GetSlabCoordinate(const f32* vertex, i32 side)
    {
        if (side == 0 || side == 4)
            return vertex[0];

        return vertex[2];
    }

    void CalculateSlabEndPoints(const f32* start, const f32* end, i32 side, f32* slabMin, f32* slabMax)
    {
        const i32 horizontalAxis = (side == 0 || side == 4) ? 2 : 0;
        if (start[horizontalAxis] < end[horizontalAxis])
        {
            slabMin[0] = start[horizontalAxis];
            slabMin[1] = start[1];
            slabMax[0] = end[horizontalAxis];
            slabMax[1] = end[1];
        }
        else
        {
            slabMin[0] = end[horizontalAxis];
            slabMin[1] = end[1];
            slabMax[0] = start[horizontalAxis];
            slabMax[1] = start[1];
        }
    }

    bool SlabsOverlap(const f32* firstMin, const f32* firstMax, const f32* secondMin, const f32* secondMax, f32 walkableClimb)
    {
        const f32 overlapMin = std::max(firstMin[0] + PORTAL_HORIZONTAL_EPSILON, secondMin[0] + PORTAL_HORIZONTAL_EPSILON);
        const f32 overlapMax = std::min(firstMax[0] - PORTAL_HORIZONTAL_EPSILON, secondMax[0] - PORTAL_HORIZONTAL_EPSILON);
        if (overlapMin > overlapMax)
            return false;

        const f32 firstWidth = firstMax[0] - firstMin[0];
        const f32 secondWidth = secondMax[0] - secondMin[0];
        if (firstWidth <= 0.0f || secondWidth <= 0.0f)
            return false;

        const f32 firstSlope = (firstMax[1] - firstMin[1]) / firstWidth;
        const f32 firstOffset = firstMin[1] - firstSlope * firstMin[0];
        const f32 secondSlope = (secondMax[1] - secondMin[1]) / secondWidth;
        const f32 secondOffset = secondMin[1] - secondSlope * secondMin[0];
        const f32 differenceAtMin = (secondSlope * overlapMin + secondOffset) - (firstSlope * overlapMin + firstOffset);
        const f32 differenceAtMax = (secondSlope * overlapMax + secondOffset) - (firstSlope * overlapMax + firstOffset);

        if (differenceAtMin * differenceAtMax < 0.0f)
            return true;

        const f32 verticalThreshold = walkableClimb * 2.0f;
        return differenceAtMin * differenceAtMin <= verticalThreshold * verticalThreshold ||
            differenceAtMax * differenceAtMax <= verticalThreshold * verticalThreshold;
    }

    std::vector<PortalEdge> CollectPortalEdges(const dtMeshTile& tile, i32 side)
    {
        std::vector<PortalEdge> edges;

        for (i32 polyIndex = 0; polyIndex < tile.header->polyCount; polyIndex++)
        {
            const dtPoly& poly = tile.polys[polyIndex];
            for (u32 edgeIndex = 0; edgeIndex < poly.vertCount; edgeIndex++)
            {
                if (poly.neis[edgeIndex] != (DT_EXT_LINK | static_cast<u16>(side)))
                    continue;

                PortalEdge& edge = edges.emplace_back();
                edge.start = &tile.verts[poly.verts[edgeIndex] * 3];
                edge.end = &tile.verts[poly.verts[(edgeIndex + 1) % poly.vertCount] * 3];
            }
        }

        return edges;
    }

    bool HasMatchingPortal(const dtMeshTile& sourceTile, i32 sourceSide, const dtMeshTile& targetTile, i32 targetSide)
    {
        const std::vector<PortalEdge> sourceEdges = CollectPortalEdges(sourceTile, sourceSide);
        const std::vector<PortalEdge> targetEdges = CollectPortalEdges(targetTile, targetSide);

        for (const PortalEdge& sourceEdge : sourceEdges)
        {
            f32 sourceMin[2];
            f32 sourceMax[2];
            CalculateSlabEndPoints(sourceEdge.start, sourceEdge.end, sourceSide, sourceMin, sourceMax);
            const f32 sourcePosition = GetSlabCoordinate(sourceEdge.start, sourceSide);

            for (const PortalEdge& targetEdge : targetEdges)
            {
                if (std::abs(sourcePosition - GetSlabCoordinate(targetEdge.start, targetSide)) > PORTAL_HORIZONTAL_EPSILON)
                    continue;

                f32 targetMin[2];
                f32 targetMax[2];
                CalculateSlabEndPoints(targetEdge.start, targetEdge.end, targetSide, targetMin, targetMax);
                if (SlabsOverlap(sourceMin, sourceMax, targetMin, targetMax, targetTile.header->walkableClimb))
                    return true;
            }
        }

        return false;
    }

    CrossTileLink FindCrossTileLink(const dtNavMesh& navMesh, const dtMeshTile& sourceTile, const dtMeshTile& targetTile)
    {
        const dtPolyRef sourceBase = navMesh.getPolyRefBase(&sourceTile);

        for (i32 polyIndex = 0; polyIndex < sourceTile.header->polyCount; polyIndex++)
        {
            const dtPoly& poly = sourceTile.polys[polyIndex];
            for (u32 linkIndex = poly.firstLink; linkIndex != DT_NULL_LINK; linkIndex = sourceTile.links[linkIndex].next)
            {
                const dtLink& link = sourceTile.links[linkIndex];
                if (!link.ref)
                    continue;

                const dtMeshTile* linkedTile = nullptr;
                const dtPoly* linkedPoly = nullptr;
                navMesh.getTileAndPolyByRefUnsafe(link.ref, &linkedTile, &linkedPoly);
                if (linkedTile == &targetTile)
                    return { sourceBase | static_cast<dtPolyRef>(polyIndex), link.ref };
            }
        }

        return {};
    }

    bool GetPolyCenter(const dtNavMesh& navMesh, dtPolyRef polyRef, f32* center)
    {
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        if (dtStatusFailed(navMesh.getTileAndPolyByRef(polyRef, &tile, &poly)) || !tile || !poly || poly->vertCount == 0)
            return false;

        center[0] = 0.0f;
        center[1] = 0.0f;
        center[2] = 0.0f;

        for (u32 vertexIndex = 0; vertexIndex < poly->vertCount; vertexIndex++)
        {
            const f32* vertex = &tile->verts[poly->verts[vertexIndex] * 3];
            center[0] += vertex[0];
            center[1] += vertex[1];
            center[2] += vertex[2];
        }

        const f32 inverseVertexCount = 1.0f / static_cast<f32>(poly->vertCount);
        center[0] *= inverseVertexCount;
        center[1] *= inverseVertexCount;
        center[2] *= inverseVertexCount;
        return true;
    }

    bool ValidatePath(const dtNavMesh& navMesh, dtNavMeshQuery& query, const dtQueryFilter& filter, const CrossTileLink& link)
    {
        f32 startPosition[3];
        f32 endPosition[3];
        if (!GetPolyCenter(navMesh, link.source, startPosition) || !GetPolyCenter(navMesh, link.target, endPosition))
            return false;

        std::array<dtPolyRef, 8> path;
        i32 pathCount = 0;
        const dtStatus status = query.findPath(link.source, link.target, startPosition, endPosition, &filter, path.data(), &pathCount, static_cast<i32>(path.size()));
        return dtStatusSucceed(status) &&
            !dtStatusDetail(status, DT_PARTIAL_RESULT) &&
            pathCount >= 2 &&
            path[pathCount - 1] == link.target;
    }

    PairValidationResult ValidatePair(const dtNavMesh& navMesh, dtNavMeshQuery& query, const dtQueryFilter& filter, u32 sourceX, u32 sourceY, u32 targetX, u32 targetY, i32 sourceSide, i32 targetSide)
    {
        const dtMeshTile* sourceTile = navMesh.getTileAt(sourceX, sourceY, 0);
        const dtMeshTile* targetTile = navMesh.getTileAt(targetX, targetY, 0);
        if (!sourceTile || !targetTile)
            return PairValidationResult::Failed;

        const bool matchingPortal = HasMatchingPortal(*sourceTile, sourceSide, *targetTile, targetSide);
        const CrossTileLink forwardLink = FindCrossTileLink(navMesh, *sourceTile, *targetTile);
        const CrossTileLink reverseLink = FindCrossTileLink(navMesh, *targetTile, *sourceTile);

        PairValidationResult result = PairValidationResult::NotTraversable;
        if (matchingPortal || forwardLink || reverseLink)
        {
            result = forwardLink &&
                reverseLink &&
                ValidatePath(navMesh, query, filter, forwardLink) &&
                ValidatePath(navMesh, query, filter, reverseLink)
                ? PairValidationResult::Valid
                : PairValidationResult::Failed;
        }

        return result;
    }
}

NavMesh::SeamValidationResult NavMesh::ValidateSeams(const std::filesystem::path& outputDirectory, const std::string& mapName, const std::vector<u32>& tileIDs)
{
    std::array<bool, Terrain::CHUNK_NUM_PER_MAP> availableTiles{};
    for (u32 tileID : tileIDs)
    {
        if (tileID < availableTiles.size())
            availableTiles[tileID] = true;
    }

    SeamValidationResult result;
    std::array<std::unique_ptr<TileData>, Terrain::CHUNK_NUM_PER_MAP> tileData;
    std::array<bool, Terrain::CHUNK_NUM_PER_MAP> loadedTiles{};
    f32 minimumHeight = std::numeric_limits<f32>::max();
    i32 maximumPolys = 0;
    i32 loadedTileCount = 0;

    for (u32 tileID = 0; tileID < availableTiles.size(); tileID++)
    {
        if (!availableTiles[tileID])
            continue;

        const u32 chunkX = tileID % Terrain::CHUNK_NUM_PER_MAP_STRIDE;
        const u32 chunkY = tileID / Terrain::CHUNK_NUM_PER_MAP_STRIDE;
        std::unique_ptr<TileData> data = std::make_unique<TileData>();
        if (!LoadTile(GetTilePath(outputDirectory, mapName, chunkX, chunkY), chunkX, chunkY, *data))
        {
            NC_LOG_ERROR("[NavMesh Validator] Failed to load NavMesh tile ({}_{}_{})", mapName, chunkX, chunkY);
            continue;
        }

        const dtMeshHeader* header = data->GetHeader();
        minimumHeight = std::min(minimumHeight, header->bmin[1]);
        maximumPolys = std::max(maximumPolys, header->polyCount);
        tileData[tileID] = std::move(data);
        loadedTileCount++;
    }

    std::unique_ptr<dtNavMesh, decltype(&dtFreeNavMesh)> navMesh(dtAllocNavMesh(), &dtFreeNavMesh);
    bool navMeshReady = false;
    if (navMesh && loadedTileCount > 0)
    {
        dtNavMeshParams params{};
        params.orig[0] = -Terrain::MAP_HALF_SIZE;
        params.orig[1] = minimumHeight;
        params.orig[2] = -Terrain::MAP_HALF_SIZE;
        params.tileWidth = Terrain::CHUNK_SIZE;
        params.tileHeight = Terrain::CHUNK_SIZE;
        params.maxTiles = loadedTileCount;
        params.maxPolys = maximumPolys;
        navMeshReady = dtStatusSucceed(navMesh->init(&params));
    }

    if (navMeshReady)
    {
        for (u32 tileID = 0; tileID < tileData.size(); tileID++)
        {
            if (!tileData[tileID])
                continue;

            TileData& data = *tileData[tileID];
            if (dtStatusFailed(navMesh->addTile(data.bytes.data(), static_cast<i32>(data.bytes.size()), 0, 0, nullptr)))
            {
                const u32 chunkX = tileID % Terrain::CHUNK_NUM_PER_MAP_STRIDE;
                const u32 chunkY = tileID / Terrain::CHUNK_NUM_PER_MAP_STRIDE;
                NC_LOG_ERROR("[NavMesh Validator] Failed to add NavMesh tile ({}_{}_{})", mapName, chunkX, chunkY);
                continue;
            }

            loadedTiles[tileID] = true;
        }
    }

    dtNavMeshQuery query;
    const bool queryReady = navMeshReady && dtStatusSucceed(query.init(navMesh.get(), 64));
    dtQueryFilter filter;
    filter.setIncludeFlags(WALKABLE_POLY_FLAG);
    filter.setExcludeFlags(0);

    for (u32 tileID = 0; tileID < availableTiles.size(); tileID++)
    {
        if (!availableTiles[tileID])
            continue;

        const u32 chunkX = tileID % Terrain::CHUNK_NUM_PER_MAP_STRIDE;
        const u32 chunkY = tileID / Terrain::CHUNK_NUM_PER_MAP_STRIDE;

        const struct
        {
            u32 targetX;
            u32 targetY;
            i32 sourceSide;
            i32 targetSide;
        } neighbors[] =
        {
            { chunkX + 1, chunkY, 0, 4 },
            { chunkX, chunkY + 1, 2, 6 }
        };

        for (const auto& neighbor : neighbors)
        {
            if (neighbor.targetX >= Terrain::CHUNK_NUM_PER_MAP_STRIDE ||
                neighbor.targetY >= Terrain::CHUNK_NUM_PER_MAP_STRIDE)
            {
                continue;
            }

            const u32 targetTileID = neighbor.targetX + neighbor.targetY * Terrain::CHUNK_NUM_PER_MAP_STRIDE;
            if (!availableTiles[targetTileID])
                continue;

            result.adjacentPairs++;
            const PairValidationResult pairResult =
                queryReady && loadedTiles[tileID] && loadedTiles[targetTileID]
                ? ValidatePair(*navMesh, query, filter, chunkX, chunkY, neighbor.targetX, neighbor.targetY, neighbor.sourceSide, neighbor.targetSide)
                : PairValidationResult::Failed;
            if (pairResult == PairValidationResult::Valid)
            {
                result.validatedPairs++;
            }
            else if (pairResult == PairValidationResult::NotTraversable)
            {
                result.skippedPairs++;
            }
            else
            {
                result.failedPairs++;
                if (result.failedPairs <= MAX_DETAILED_FAILURES)
                {
                    NC_LOG_ERROR("[NavMesh Validator] Failed seam validation for {} tiles ({}_{} -> {}_{})", mapName, chunkX, chunkY, neighbor.targetX, neighbor.targetY);
                }
            }
        }
    }

    return result;
}

#include "MapExtractor.h"
#include "NavMeshBuilder.h"
#include "NavMeshValidator.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Blp/BlpConvert.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Extractors/ClientDBExtractor.h"
#include "AssetConverter-App/Util/JoltStream.h"
#include "AssetConverter-App/Util/ServiceLocator.h"

#include <Base/Container/ConcurrentQueue.h>
#include <Base/Util/StringUtils.h>
#include <Base/Util/DebugHandler.h>

#include <FileFormat/Novus/ClientDB/ClientDB.h>
#include <FileFormat/Novus/Map/Map.h>
#include <FileFormat/Novus/Map/MapChunk.h>
#include <FileFormat/Novus/Model/ComplexModel.h>
#include <FileFormat/Warcraft/ADT/Adt.h>
#include <FileFormat/Warcraft/Parsers/WdtParser.h>
#include <FileFormat/Warcraft/Parsers/AdtParser.h>

#include <MetaGen/Shared/ClientDB/ClientDB.h>

#include <enkiTS/TaskScheduler.h>
#include <glm/gtx/euler_angles.inl>

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <tracy/Tracy.hpp>

#include <robinhood/robinhood.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

using namespace ClientDB;

namespace
{
    struct MapSelection
    {
        robin_hood::unordered_set<u32> ids;
        robin_hood::unordered_set<std::string> internalNames;

        bool IsRestricted() const
        {
            return !ids.empty() || !internalNames.empty();
        }

        bool Contains(u32 id, const std::string& internalName) const
        {
            if (!IsRestricted())
                return true;

            return ids.contains(id) || internalNames.contains(internalName);
        }
    };

    MapSelection LoadMapSelection()
    {
        Runtime* runtime = ServiceLocator::GetRuntime();
        MapSelection selection;

        const auto& extractionConfig = runtime->json["Extraction"];
        if (!extractionConfig.contains("MapSelection"))
            return selection;

        const auto& mapSelectionConfig = extractionConfig["MapSelection"];
        if (mapSelectionConfig.contains("IDs") && mapSelectionConfig["IDs"].is_array())
        {
            for (const auto& idValue : mapSelectionConfig["IDs"])
            {
                if (idValue.is_number_unsigned())
                {
                    selection.ids.insert(idValue.get<u32>());
                }
                else if (idValue.is_number_integer())
                {
                    const i64 signedID = idValue.get<i64>();
                    if (signedID >= 0 && signedID <= std::numeric_limits<u32>::max())
                        selection.ids.insert(static_cast<u32>(signedID));
                }
            }
        }

        if (mapSelectionConfig.contains("InternalNames") && mapSelectionConfig["InternalNames"].is_array())
        {
            for (const auto& nameValue : mapSelectionConfig["InternalNames"])
            {
                if (!nameValue.is_string())
                    continue;

                std::string internalName = nameValue.get<std::string>();
                StringUtils::ToLower(internalName);
                selection.internalNames.insert(std::move(internalName));
            }
        }

        return selection;
    }

    bool PrepareNavMeshOutputDirectory(const std::filesystem::path& outputDirectory, const std::string& mapName)
    {
        std::error_code error;
        std::filesystem::create_directories(outputDirectory, error);
        if (error)
        {
            NC_LOG_ERROR("[Map Extractor] Failed to create NavMesh output directory for {}: {}", mapName, error.message());
            return false;
        }

        bool success = true;
        std::filesystem::directory_iterator iterator(outputDirectory, error);
        const std::filesystem::directory_iterator end;
        while (!error && iterator != end)
        {
            const std::filesystem::directory_entry& entry = *iterator;

            if (!entry.is_regular_file(error))
            {
                error.clear();
                iterator.increment(error);
                continue;
            }

            const std::filesystem::path& path = entry.path();
            const std::string extension = path.extension().string();
            if (extension != NavMesh::TILE_FILE_EXTENSION && extension != NavMesh::TerrainHeight::FILE_EXTENSION)
            {
                iterator.increment(error);
                continue;
            }

            std::error_code removeError;
            std::filesystem::remove(path, removeError);
            if (removeError)
            {
                NC_LOG_ERROR("[Map Extractor] Failed to remove stale NavMesh artifact {}: {}", path.string(), removeError.message());
                success = false;
            }

            iterator.increment(error);
        }

        if (error)
        {
            NC_LOG_ERROR("[Map Extractor] Failed to scan NavMesh output directory for {}: {}", mapName, error.message());
            success = false;
        }

        return success;
    }
}

vec2 GetCellVertexPosition(u32 cellID, u32 vertexID)
{
    const i32 cellX = ((cellID % Terrain::CHUNK_NUM_CELLS_PER_STRIDE));
    const i32 cellY = ((cellID / Terrain::CHUNK_NUM_CELLS_PER_STRIDE));

    const i32 vX = vertexID % 17;
    const i32 vY = vertexID / 17;

    bool isOddRow = vX > 8;

    vec2 vertexOffset;
    vertexOffset.x = -(8.5f * isOddRow);
    vertexOffset.y = (0.5f * isOddRow);

    ivec2 globalVertex = ivec2(vX + cellX * 8, vY + cellY * 8);

    vec2 finalPos = (vec2(globalVertex) + vertexOffset) * Terrain::PATCH_SIZE;

    return vec2(finalPos.x, -finalPos.y);
}

void MapExtractor::Process(bool extractMapAssets, bool generateNavMesh)
{
    ZoneScopedN("MapExtractor::Process");

    Runtime* runtime = ServiceLocator::GetRuntime();
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    const auto& navMeshConfig = runtime->json["Extraction"]["NavMesh"];
    const bool validateNavMesh = generateNavMesh && navMeshConfig.value("Validate", true);
    NavMesh::BuildSettings navMeshBuildSettings;
    navMeshBuildSettings.useMonotonePartitioning = generateNavMesh && navMeshConfig.value("UseMonotonePartitioning", navMeshBuildSettings.useMonotonePartitioning);
    navMeshBuildSettings.useMedianFilter = navMeshConfig.value("UseMedianFilter", navMeshBuildSettings.useMedianFilter);
    navMeshBuildSettings.detailSampleDistance = navMeshConfig.value("DetailSampleDistance", navMeshBuildSettings.detailSampleDistance);
    navMeshBuildSettings.maxEdgeLength = navMeshConfig.value("MaxEdgeLength", navMeshBuildSettings.maxEdgeLength);
    navMeshBuildSettings.maxSimplificationError = navMeshConfig.value("MaxSimplificationError", navMeshBuildSettings.maxSimplificationError);
    navMeshBuildSettings.minRegionRadius = navMeshConfig.value("MinRegionRadius", navMeshBuildSettings.minRegionRadius);
    navMeshBuildSettings.mergeRegionRadius = navMeshConfig.value("MergeRegionRadius", navMeshBuildSettings.mergeRegionRadius);
    navMeshBuildSettings.internalSubtileVoxelSize = navMeshConfig.value("InternalSubtileVoxelSize", navMeshBuildSettings.internalSubtileVoxelSize);
    const MapSelection mapSelection = LoadMapSelection();

    auto& mapStorage = ClientDBExtractor::mapStorage;
    u32 numMapEntries = mapStorage.GetNumRows();
    NC_LOG_INFO("[Map Extractor] Processing {0} maps", numMapEntries);
    if (mapSelection.IsRestricted())
    {
        NC_LOG_INFO("[Map Extractor] Map selection enabled ({} ID(s), {} internal name(s))", mapSelection.ids.size(), mapSelection.internalNames.size());
    }

    mapStorage.Each([&](const u32 id, const MetaGen::Shared::ClientDB::MapRecord& map) -> bool
    {
        ZoneScopedN("MapExtractor::Process::Each");

        std::string internalName = mapStorage.GetString(map.nameInternal);
        StringUtils::ToLower(internalName);
        if (!mapSelection.Contains(id, internalName))
            return true;

        static char formatBuffer[512] = { 0 };
        i32 length = StringUtils::FormatString(&formatBuffer[0], 512, "world/maps/%s/%s.wdt", internalName.c_str(), internalName.c_str());
        if (length <= 0)
            return true;

        std::string wdtPath(&formatBuffer[0], length);

        u32 wdtFileID = cascLoader->GetFileIDFromListFilePath(wdtPath.data());
        if (!wdtFileID)
            return true;

        std::shared_ptr<Bytebuffer> fileWDT = cascLoader->GetFileByID(wdtFileID);
        if (!fileWDT)
            return true;

        Adt::WdtParser wdtParser = { };

        Adt::Wdt wdt = { };
        if (!wdtParser.TryParse(fileWDT, wdt))
        {
            NC_LOG_WARNING("[Map Extractor] Failed to extract {0} (Corrupt WDT)", internalName);
            return true;
        }

        Map::MapHeader mapHeader = { };
        mapHeader.flags.UseMapObjectAsBase = wdt.mphd.flags.UseGlobalMapObj;

        if (mapHeader.flags.UseMapObjectAsBase)
        {
            if (!extractMapAssets)
                return true;

            if (!wdt.modf.data.size())
                return true;

            const Adt::MODF::PlacementInfo& placementInfo = wdt.modf.data[0];
            if (!placementInfo.flags.EntryIsFiledataID || placementInfo.fileID == 0)
                return true;

            // Skip map if placement file doesn't exist
            if (!cascLoader->InCascAndListFile(placementInfo.fileID))
            {
                NC_LOG_ERROR("Skipped map {0} because placement file doesn't exist", internalName);
                return true;
            }

            Terrain::Placement& placement = mapHeader.placement;
            {
                placement.uniqueID = placementInfo.uniqueID;
                placement.nameHash = placementInfo.fileID;
                placement.position = CoordinateSpaces::PlacementPosToNovus(placementInfo.position);

                vec3 placementRotation = glm::radians(CoordinateSpaces::PlacementRotToNovus(placementInfo.rotation));
                glm::mat4 matrix = glm::eulerAngleYXZ(placementRotation.y, placementRotation.x, placementRotation.z);
                placement.rotation = glm::quat_cast(matrix);

                bool hasScale = placementInfo.flags.HasScale;
                placement.scale = (placementInfo.scale * hasScale) + (1024 * !hasScale);
            }

            u32 placementFileID = static_cast<u32>(placement.nameHash);
            const std::string& filePath = cascLoader->GetFilePathFromListFileID(placementFileID);
            std::filesystem::path wmoPath = std::filesystem::path("model") / std::filesystem::path(filePath).replace_extension(Model::FILE_EXTENSION);
            wmoPath.make_preferred();
            std::string wmoPathStr = wmoPath.string();
            std::transform(wmoPathStr.begin(), wmoPathStr.end(), wmoPathStr.begin(), ::tolower);
            std::replace(wmoPathStr.begin(), wmoPathStr.end(), '\\', '/');

            u64 nameHash = XXHash64::hash(wmoPathStr.c_str(), wmoPathStr.size(), 0);
            placement.nameHash = nameHash;
        }
        else
        {
            std::filesystem::path navOutputDirectory;
            if (generateNavMesh)
            {
                navOutputDirectory = runtime->paths.navMesh / internalName;
                if (!PrepareNavMeshOutputDirectory(navOutputDirectory, internalName))
                    return true;
            }

            const auto terrainExtractionStart = std::chrono::steady_clock::now();
            moodycamel::ConcurrentQueue<u64> mapChunkHashes;
            NavMesh::SourceStore navSources;

            enki::TaskSet convertMapTask(Terrain::CHUNK_NUM_PER_MAP, [&runtime, &cascLoader, &map, &wdt, &internalName, &mapChunkHashes, &navSources, extractMapAssets, generateNavMesh, id](enki::TaskSetPartition range, uint32_t threadNum)
            {
                ZoneScopedN("MapExtractor::Process::Each::ConvertMapTask");
                Adt::Parser adtParser = { };

                std::vector<u8> outBytes;
                std::shared_ptr<Bytebuffer> buffer;
                if (extractMapAssets)
                {
                    outBytes.reserve(Terrain::CHUNK_ALPHAMAP_TOTAL_BYTE_SIZE);
                    buffer = Bytebuffer::Borrow<8388608>();
                }

                for (u32 chunkID = range.start; chunkID < range.end; chunkID++)
                {
                    u32 originalChunkGridPosX = chunkID % 64;
                    u32 originalChunkGridPosY = chunkID / 64;

                    const Adt::MAIN::AreaInfo& areaInfo = wdt.main.areaInfos[originalChunkGridPosX][originalChunkGridPosY];
                    if (!areaInfo.flags.IsUsed)
                        continue;

                    const Adt::MAID::FileIDs& fileIDs = wdt.maid.fileIDs[originalChunkGridPosX][originalChunkGridPosY];
                    if (fileIDs.adtRootFileID == 0 ||
                        (extractMapAssets && (fileIDs.adtTextureFileID == 0 || fileIDs.adtObject1FileID == 0)))
                    {
                        continue;
                    }

                    std::shared_ptr<Bytebuffer> rootBuffer = cascLoader->GetFileByID(fileIDs.adtRootFileID);
                    std::shared_ptr<Bytebuffer> textBuffer;
                    std::shared_ptr<Bytebuffer> objBuffer;
                    if (extractMapAssets)
                    {
                        textBuffer = cascLoader->GetFileByID(fileIDs.adtTextureFileID);
                        objBuffer = cascLoader->GetFileByID(fileIDs.adtObject1FileID);
                    }

                    if (!rootBuffer)
                        continue;

                    u32 chunkGridPosX = chunkID / 64;
                    u32 chunkGridPosY = chunkID % 64;
                    u32 newChunkID = chunkGridPosX + (chunkGridPosY * Terrain::CHUNK_NUM_PER_MAP_STRIDE);

                    ZoneScopedN("MapExtractor::Process::Each::ConvertMapTask::Convert");
                    Adt::Layout adt = { };
                    {
                        adt.mapID = id;
                        adt.chunkID = newChunkID;
                    }

                    Adt::Parser::Context context = { };
                    if (!adtParser.TryParse(context, rootBuffer, textBuffer, objBuffer, wdt, adt))
                        continue;

                    // Post Processing
                    if (extractMapAssets)
                    {
                        auto& liquidObjects = ClientDBExtractor::liquidObjectStorage;
                        auto& liquidTypes = ClientDBExtractor::liquidTypeStorage;
                        auto& liquidMaterials = ClientDBExtractor::liquidMaterialStorage;

                        u32 numInstances = static_cast<u32>(adt.mh2o.instances.size());
                        for (u32 i = 0; i < numInstances; i++)
                        {
                            auto& liquidInstance = adt.mh2o.instances[i];
                            u16 liquidVertexFormat = liquidInstance.liquidVertexFormat;

                            if (liquidVertexFormat >= 42)
                            {
                                if (liquidInstance.liquidType == 2)
                                {
                                    liquidVertexFormat = 2;
                                }
                                else
                                {
                                    i16 liquidTypeID = -1;

                                    if (liquidObjects.Has(liquidVertexFormat))
                                    {
                                        auto& liquidObject = liquidObjects.Get<MetaGen::Shared::ClientDB::LiquidObjectRecord>(liquidVertexFormat);
                                        liquidTypeID = liquidObject.liquidTypeID;
                                    }
                                    else
                                    {
                                        liquidTypeID = liquidInstance.liquidType;
                                    }

                                    if (liquidTypes.Has(liquidTypeID))
                                    {
                                        auto& liquidType = liquidTypes.Get<MetaGen::Shared::ClientDB::LiquidTypeRecord>(liquidTypeID);

                                        if (liquidMaterials.Has(liquidType.materialID))
                                        {
                                            auto& liquidMaterial = liquidMaterials.Get<MetaGen::Shared::ClientDB::LiquidMaterialRecord>(liquidType.materialID);
                                            liquidVertexFormat = liquidMaterial.liquidVertexFormat;
                                        }
                                    }
                                }

                            }

                            if (liquidInstance.vertexDataOffset == 0 && liquidInstance.liquidType != 2)
                            {
                                liquidVertexFormat = 2;
                            }

                            if (liquidVertexFormat == 2)
                            {
                                liquidInstance.width = 8;
                                liquidInstance.height = 8;
                                liquidInstance.offsetX = 0;
                                liquidInstance.offsetY = 0;
                            }

                            liquidInstance.liquidVertexFormat = liquidVertexFormat;

                            if (liquidInstance.liquidVertexFormat == 2)
                                liquidInstance.vertexDataOffset = std::numeric_limits<u32>().max();
                        }
                    }

                    if (generateNavMesh && !extractMapAssets)
                    {
                        if (!navSources.Add(chunkGridPosX, chunkGridPosY, adt))
                        {
                            NC_LOG_ERROR("[Map Extractor] Failed to retain NavMesh source for Map Tile ({}_{}_{})", internalName, chunkGridPosX, chunkGridPosY);
                        }
                        continue;
                    }

                    Map::Chunk chunk = { };
                    std::vector<Terrain::Placement> modelPlacements;
                    Map::LiquidInfo liquidInfo;
                    std::vector<u8> physicsData;
                    if (!Map::Chunk::FromADT(adt, chunk, modelPlacements, liquidInfo))
                        continue;

                    if (generateNavMesh && !navSources.Add(chunkGridPosX, chunkGridPosY, chunk))
                    {
                        NC_LOG_ERROR("[Map Extractor] Failed to retain NavMesh source for Map Tile ({}_{}_{})", internalName, chunkGridPosX, chunkGridPosY);
                    }

                    // Post Processing
                    {
                        for (u32 i = 0; i < modelPlacements.size(); i++)
                        {
                            Terrain::Placement& placementInfo = modelPlacements[i];

                            if (placementInfo.nameHash == 0 ||
                                placementInfo.nameHash == std::numeric_limits<u64>().max())
                                continue;

                            u32 placementFileID = static_cast<u32>(placementInfo.nameHash);
                            if (!cascLoader->InCascAndListFile(placementFileID))
                            {
                                NC_LOG_ERROR("Skipped model placement because file doesn't exist");
                                continue;
                            }

                            const std::string& modelPathStr = cascLoader->GetFilePathFromListFileID(placementFileID);
                            std::filesystem::path modelPath = std::filesystem::path("model") / std::filesystem::path(modelPathStr).replace_extension(Model::FILE_EXTENSION);
                            modelPath.make_preferred();
                            std::string modelPathHashStr = modelPath.string();
                            std::transform(modelPathHashStr.begin(), modelPathHashStr.end(), modelPathHashStr.begin(), ::tolower);
                            std::replace(modelPathHashStr.begin(), modelPathHashStr.end(), '\\', '/');

                            u64 nameHash = XXHash64::hash(modelPathHashStr.c_str(), modelPathHashStr.size(), 0);
                            placementInfo.nameHash = nameHash;
                        }

                        // 0 = r, 1 = g, 2 = b, 3 = a
                        u32 swizzleMap[Terrain::CHUNK_ALPHAMAP_CELL_NUM_CHANNELS] =
                        {
                            2,1,0,3
                        };

                        std::shared_ptr<Bytebuffer> alphaMapBuffer = Bytebuffer::Borrow<Terrain::CHUNK_ALPHAMAP_TOTAL_BYTE_SIZE>();
                        memset(alphaMapBuffer->GetDataPointer(), 0, Terrain::CHUNK_ALPHAMAP_TOTAL_BYTE_SIZE);

                        bool isAlphaMapSet = false;

                        for (u16 i = 0; i < Terrain::CHUNK_NUM_CELLS; i++)
                        {
                            u16 cellIndex = i;

                            const u32 numLayers = static_cast<u32>(adt.cellInfos[i].mcly.data.size());
                            const u32 basePixelDestination = (i * Terrain::CHUNK_ALPHAMAP_CELL_RESOLUTION * Terrain::CHUNK_ALPHAMAP_CELL_NUM_CHANNELS);

                            for (u32 j = 0; j < 4; j++)
                            {
                                u32 fileID = static_cast<u32>(chunk.cellsData.layerTextureIDs[cellIndex][j]);
                                if (fileID == 0 || fileID == std::numeric_limits<u32>().max())
                                    continue;

                                std::filesystem::path texturePath = cascLoader->GetFilePathFromListFileID(fileID);
                                if (texturePath.empty())
                                {
                                    chunk.cellsData.layerTextureIDs[cellIndex][j] = std::numeric_limits<u64>().max();
                                    continue;
                                }

                                texturePath = std::filesystem::path("texture") / texturePath;
                                texturePath.replace_extension("dds").make_preferred();

                                std::string texturePathStr = texturePath.string();
                                std::transform(texturePathStr.begin(), texturePathStr.end(), texturePathStr.begin(), ::tolower);
                                std::replace(texturePathStr.begin(), texturePathStr.end(), '\\', '/');

                                u64 textureNameHash = XXHash64::hash(texturePathStr.c_str(), texturePathStr.length(), 0);
                                chunk.cellsData.layerTextureIDs[cellIndex][j] = textureNameHash;

                                // If the layer has alpha data, add it to our per-chunk alphamap
                                if (j > 0)
                                {
                                    u32 channel = swizzleMap[j - 1];

                                    for (u32 pixel = 0; pixel < Terrain::CHUNK_ALPHAMAP_CELL_RESOLUTION; pixel++)
                                    {
                                        u32 dst = basePixelDestination + (pixel * Terrain::CHUNK_ALPHAMAP_CELL_NUM_CHANNELS) + channel;

                                        u8 pixelValue = adt.cellInfos[i].mcal.data[j - 1].alphaMap[pixel];
                                        isAlphaMapSet |= pixelValue != 0;

                                        alphaMapBuffer->GetDataPointer()[dst] = pixelValue;
                                    }
                                }
                            }

                            // Convert Old Alpha to New Alpha
                            if (!wdt.mphd.flags.UseBigAlpha && numLayers > 1)
                            {
                                const u32 basePixelDestination = (i * Terrain::CHUNK_ALPHAMAP_CELL_RESOLUTION * Terrain::CHUNK_ALPHAMAP_CELL_NUM_CHANNELS);

                                const vec4 alphaR = vec4(1, 0, 0, 0);
                                const vec4 alphaG = vec4(0, 1, 0, 0);
                                const vec4 alphaB = vec4(0, 0, 1, 0);
                                const vec4 alphaA = vec4(0, 0, 0, 1);

                                for (u32 pixel = 0; pixel < Terrain::CHUNK_ALPHAMAP_CELL_RESOLUTION; pixel++)
                                {
                                    u32 redDst = basePixelDestination + (pixel * Terrain::CHUNK_ALPHAMAP_CELL_NUM_CHANNELS) + swizzleMap[0];
                                    u32 greenDst = basePixelDestination + (pixel * Terrain::CHUNK_ALPHAMAP_CELL_NUM_CHANNELS) + swizzleMap[1];
                                    u32 blueDst = basePixelDestination + (pixel * Terrain::CHUNK_ALPHAMAP_CELL_NUM_CHANNELS) + swizzleMap[2];
                                    u32 alphaDst = basePixelDestination + (pixel * Terrain::CHUNK_ALPHAMAP_CELL_NUM_CHANNELS) + swizzleMap[3];

                                    f32 redPixelFloat = alphaMapBuffer->GetDataPointer()[redDst] / 255.f;
                                    f32 greenPixelFloat = alphaMapBuffer->GetDataPointer()[greenDst] / 255.f;
                                    f32 bluePixelFloat = alphaMapBuffer->GetDataPointer()[blueDst] / 255.f;

                                    vec4 accumulated = alphaR;
                                    accumulated = glm::mix(accumulated, alphaG, redPixelFloat);
                                    accumulated = glm::mix(accumulated, alphaB, greenPixelFloat);
                                    accumulated = glm::mix(accumulated, alphaA, bluePixelFloat);
                                    accumulated = glm::clamp(accumulated, 0.f, 1.f);

                                    u8 redPixelByte = static_cast<u8>(glm::round(accumulated.g * 255));
                                    u8 greenPixelByte = static_cast<u8>(glm::round(accumulated.b * 255));
                                    u8 bluePixelByte = static_cast<u8>(glm::round(accumulated.a * 255));

                                    alphaMapBuffer->GetDataPointer()[redDst] = redPixelByte;
                                    alphaMapBuffer->GetDataPointer()[greenDst] = greenPixelByte;
                                    alphaMapBuffer->GetDataPointer()[blueDst] = bluePixelByte;
                                    alphaMapBuffer->GetDataPointer()[alphaDst] = 1;
                                }
                            }
                        }

                        std::string localChunkBlendMapPath = "texture/blendmaps/" + internalName + "/" + internalName + "_" + std::to_string(chunkGridPosX) + "_" + std::to_string(chunkGridPosY) + ".dds";
                        chunk.chunkAlphaMapTextureHash = (XXHash64::hash(localChunkBlendMapPath.c_str(), localChunkBlendMapPath.length(), 0) * isAlphaMapSet) + (std::numeric_limits<u64>().max() * !isAlphaMapSet);

                        if (isAlphaMapSet)
                        {
                            BLP::BlpConvert blpConvert;
                            outBytes.clear();

                            if (!blpConvert.ConvertRawToBuffer(64, 64, Terrain::CHUNK_NUM_CELLS, alphaMapBuffer->GetDataPointer(), Terrain::CHUNK_ALPHAMAP_TOTAL_BYTE_SIZE, BLP::InputFormat::BGRA_8UB, BLP::Format::BC1, outBytes, false))
                            {
                                runtime->pactInfo.MarkFailed();
                                NC_LOG_ERROR("[Map Extractor] Failed to convert blend map {0}", localChunkBlendMapPath);
                                continue;
                            }

                            auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, outBytes.size());
                            if (!manifest.AddFile(runtime, localChunkBlendMapPath, outBytes))
                            {
                                NC_LOG_ERROR("[Map Extractor] Failed to add blend map {0} to PACT storage", localChunkBlendMapPath);
                                continue;
                            }
                        }

                        // if build physics shapes
                        {
                            constexpr u32 numVerticesPerChunk = Terrain::CHUNK_NUM_CELLS * Terrain::CELL_TOTAL_GRID_SIZE;
                            constexpr u32 numTrianglePerChunk = Terrain::CHUNK_NUM_CELLS * Terrain::CELL_NUM_TRIANGLES;

                            JPH::VertexList vertexList;
                            JPH::IndexedTriangleList triangleList;
                            vertexList.reserve(numVerticesPerChunk);
                            triangleList.reserve(numTrianglePerChunk);

                            u32 patchVertexIDs[5] = { 0 };
                            uvec2 triangleComponentOffsets = uvec2(0, 0);

                            for (u32 cellID = 0; cellID < Terrain::CHUNK_NUM_CELLS; cellID++)
                            {
                                for (u32 i = 0; i < Terrain::CELL_TOTAL_GRID_SIZE; i++)
                                {
                                    f32 height = chunk.cellsData.heightField[cellID][i];

                                    vec2 pos = GetCellVertexPosition(cellID, i);
                                    assert(pos.x <= Terrain::CHUNK_SIZE);
                                    assert(pos.y <= Terrain::CHUNK_SIZE);

                                    vertexList.push_back({ pos.x, height, pos.y });
                                }

                                const u32 cellVertexOffset = cellID * Terrain::CELL_TOTAL_GRID_SIZE;
                                const u64 holeData = chunk.cellsData.holes[cellID];
                                for (u32 i = 0; i < Terrain::CELL_NUM_TRIANGLES; i++)
                                {
                                    u32 triangleID = i;
                                    u32 patchID = triangleID / 4;
                                    u32 patchRow = patchID / 8;
                                    u32 patchColumn = patchID % 8;

                                    // Top Left is calculated like this
                                    patchVertexIDs[0] = patchColumn + (patchRow * Terrain::CELL_GRID_ROW_SIZE);

                                    // Top Right is always +1 from Top Left
                                    patchVertexIDs[1] = patchVertexIDs[0] + 1;

                                    // Bottom Left is always NUM_VERTICES_PER_PATCH_ROW from the Top Left vertex
                                    patchVertexIDs[2] = patchVertexIDs[0] + Terrain::CELL_GRID_ROW_SIZE;

                                    // Bottom Right is always +1 from Bottom Left
                                    patchVertexIDs[3] = patchVertexIDs[2] + 1;

                                    // Center is always NUM_VERTICES_PER_OUTER_PATCH_ROW from Top Left
                                    patchVertexIDs[4] = patchVertexIDs[0] + Terrain::CELL_OUTER_GRID_STRIDE;

                                    u32 triangleWithinPatch = triangleID % 4; // 0 - top, 1 - left, 2 - bottom, 3 - right
                                    triangleComponentOffsets = uvec2(triangleWithinPatch > 1, // Identify if we are within bottom or right triangle
                                        triangleWithinPatch == 0 || triangleWithinPatch == 3); // Identify if we are within the top or right triangle

                                    u32 vertexID1 = cellVertexOffset + patchVertexIDs[4];
                                    u32 vertexID2 = cellVertexOffset + patchVertexIDs[triangleComponentOffsets.x * 2 + triangleComponentOffsets.y];
                                    u32 vertexID3 = cellVertexOffset + patchVertexIDs[(!triangleComponentOffsets.y) * 2 + triangleComponentOffsets.x];

                                    if ((holeData & (1ull << patchID)) != 0)
                                        continue;

                                    triangleList.push_back({ vertexID3, vertexID2, vertexID1 });
                                }
                            }

                            JPH::MeshShapeSettings shapeSetting(vertexList, triangleList);
                            JPH::ShapeSettings::ShapeResult shapeResult = shapeSetting.Create();
                            JPH::ShapeRefC shape = shapeResult.Get();

                            JPH::Shape::ShapeToIDMap shapeMap;
                            JPH::Shape::MaterialToIDMap materialMap;

                            std::shared_ptr<Bytebuffer> joltChunkBuffer = Bytebuffer::Borrow<16777216>();
                            JoltStream joltStream(joltChunkBuffer);
                            shape->SaveWithChildren(joltStream, shapeMap, materialMap);

                            if (!joltStream.IsFailed() && joltChunkBuffer->writtenData > 0)
                            {
                                physicsData.resize(joltChunkBuffer->writtenData);
                                memcpy(&physicsData[0], joltChunkBuffer->GetDataPointer(), joltChunkBuffer->writtenData);
                            }
                        }

                        buffer->Reset();
                        if (chunk.Save(buffer, modelPlacements, liquidInfo, physicsData))
                        {
                            std::string localChunkPath = "map/" + internalName + "/" + internalName + "_" + std::to_string(chunkGridPosX) + "_" + std::to_string(chunkGridPosY) + Map::CHUNK_FILE_EXTENSION;

                            auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, buffer->writtenData);
                            if (manifest.AddFile(runtime, localChunkPath, buffer))
                            {
                                u64 hash = XXHash64::hash(localChunkPath.c_str(), localChunkPath.length(), 0);
                                mapChunkHashes.enqueue(hash);
                            }
                            else
                            {
                                NC_LOG_ERROR("[Map Extractor] Failed to add Map Tile to PACT storage ({}_{}_{})", internalName, chunkGridPosX, chunkGridPosY);
                            }
                        }
                        else
                        {
                            NC_LOG_ERROR("[Map Extractor] Failed to save Map Tile ({}_{}_{})", internalName, chunkGridPosX, chunkGridPosY);
                        }
                    }
                }
            });

            convertMapTask.m_Priority = enki::TaskPriority::TASK_PRIORITY_HIGH;
            runtime->scheduler.AddTaskSetToPipe(&convertMapTask);
            runtime->scheduler.WaitforTask(&convertMapTask);
            const f64 terrainExtractionSeconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - terrainExtractionStart).count();

            if (generateNavMesh)
            {
                std::vector<u32> navChunkIDs;
                navSources.GetSourceIDs(navChunkIDs);

                if (!navChunkIDs.empty())
                {
                    const auto navMeshBuildStart = std::chrono::steady_clock::now();
                    const u32 numWorkers = std::max(1u, runtime->scheduler.GetNumTaskThreads());
                    moodycamel::ConcurrentQueue<u32> builtNavTileIDs;
                    std::vector<std::unique_ptr<NavMesh::Worker>> navMeshWorkers;
                    navMeshWorkers.reserve(numWorkers);

                    for (u32 workerIndex = 0; workerIndex < numWorkers; workerIndex++)
                    {
                        navMeshWorkers.push_back(std::make_unique<NavMesh::Worker>(navSources, navMeshBuildSettings));
                    }

                    enki::TaskSet buildNavMeshTask(static_cast<u32>(navChunkIDs.size()), [&internalName, &navChunkIDs, &builtNavTileIDs, &navMeshWorkers, &navOutputDirectory](enki::TaskSetPartition range, uint32_t threadNum)
                    {
                        ZoneScopedN("MapExtractor::Process::Each::BuildNavMeshTask");
                        NavMesh::Worker& worker = *navMeshWorkers[threadNum];

                        for (u32 navIndex = range.start; navIndex < range.end; navIndex++)
                        {
                            const u32 tileID = navChunkIDs[navIndex];
                            const u32 chunkGridPosX = tileID % Terrain::CHUNK_NUM_PER_MAP_STRIDE;
                            const u32 chunkGridPosY = tileID / Terrain::CHUNK_NUM_PER_MAP_STRIDE;
                            const NavMesh::TileBuildResult result = worker.BuildTile(navOutputDirectory, internalName, chunkGridPosX, chunkGridPosY);

                            if (result == NavMesh::TileBuildResult::Success)
                            {
                                builtNavTileIDs.enqueue(tileID);
                            }
                            else if (result == NavMesh::TileBuildResult::SourceMissing)
                            {
                                NC_LOG_ERROR("[Map Extractor] Missing NavMesh source for Map Tile ({}_{}_{})", internalName, chunkGridPosX, chunkGridPosY);
                            }
                            else if (result == NavMesh::TileBuildResult::Failed)
                            {
                                NC_LOG_ERROR("[Map Extractor] Failed to generate NavMesh for Map Tile ({}_{}_{})", internalName, chunkGridPosX, chunkGridPosY);
                            }
                        }
                    });

                    buildNavMeshTask.m_Priority = enki::TaskPriority::TASK_PRIORITY_HIGH;
                    runtime->scheduler.AddTaskSetToPipe(&buildNavMeshTask);
                    runtime->scheduler.WaitforTask(&buildNavMeshTask);
                    NavMesh::BuildTimings buildTimings;
                    for (const std::unique_ptr<NavMesh::Worker>& worker : navMeshWorkers)
                    {
                        buildTimings.Accumulate(worker->GetBuildTimings());
                    }
                    navMeshWorkers.clear();
                    navSources.Clear();
                    const f64 navMeshBuildSeconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - navMeshBuildStart).count();

                    std::vector<u32> builtNavTiles;
                    builtNavTiles.reserve(navChunkIDs.size());

                    u32 builtNavTileID = 0;
                    while (builtNavTileIDs.try_dequeue(builtNavTileID))
                    {
                        builtNavTiles.push_back(builtNavTileID);
                    }

                    std::sort(builtNavTiles.begin(), builtNavTiles.end());
                    const auto validationStart = std::chrono::steady_clock::now();
                    if (builtNavTiles.empty())
                    {
                        NC_LOG_WARNING("[NavMesh Validator] {} produced no NavMesh tiles to validate", internalName);
                    }
                    else if (validateNavMesh)
                    {
                        const NavMesh::SeamValidationResult validation = NavMesh::ValidateSeams(navOutputDirectory, internalName, builtNavTiles);
                        if (validation.failedPairs == 0)
                        {
                            NC_LOG_INFO("[NavMesh Validator] {} validated {} traversable seams across {} adjacent tile pairs ({} non-traversable)", internalName, validation.validatedPairs, validation.adjacentPairs, validation.skippedPairs);
                        }
                        else
                        {
                            NC_LOG_ERROR("[NavMesh Validator] {} failed {} of {} adjacent seam checks", internalName, validation.failedPairs, validation.adjacentPairs);
                        }
                    }
                    else
                    {
                        NC_LOG_INFO("[NavMesh Validator] Skipped validation for {}", internalName);
                    }
                    const f64 validationSeconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - validationStart).count();
                    NC_LOG_INFO("[NavMesh Performance] {}: source {}s, build {}s, validation {}s, {} tiles", internalName, terrainExtractionSeconds, navMeshBuildSeconds, validationSeconds, builtNavTiles.size());
                    NC_LOG_INFO("[NavMesh Build Phases] {} worker-seconds: total {}, raster {}, compact {}, regions {}, contours {}, polymesh {}, detail {}, Detour/output {}", internalName, buildTimings.totalSeconds, buildTimings.rasterizationSeconds, buildTimings.compactHeightfieldSeconds, buildTimings.regionSeconds, buildTimings.contourSeconds, buildTimings.polyMeshSeconds, buildTimings.detailMeshSeconds, buildTimings.detourAndOutputSeconds);
                }
                else
                {
                    navSources.Clear();
                    NC_LOG_INFO("[NavMesh Performance] {}: source {}s, no terrain tiles", internalName, terrainExtractionSeconds);
                }
            }

            if (extractMapAssets)
            {
                u64 chunkHash = 0;
                u32 numHashes = static_cast<u32>(mapChunkHashes.size_approx());
                mapHeader.chunkHashes.reserve(numHashes);

                while (mapChunkHashes.try_dequeue(chunkHash))
                {
                    mapHeader.chunkHashes.push_back(chunkHash);
                }
            }
        }

        if (!extractMapAssets)
            return true;

        std::shared_ptr<Bytebuffer> buffer = Bytebuffer::Borrow<1048576>();
        if (mapHeader.Save(buffer))
        {
            std::string mapHeaderPath = "map/" + internalName + "/" + internalName + Map::HEADER_FILE_EXTENSION;
            auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, buffer->writtenData);
            if (manifest.AddFile(runtime, mapHeaderPath, buffer))
            {
                NC_LOG_INFO("[Map Extractor] Extracted {0}", internalName);
            }
            else
            {
                NC_LOG_WARNING("[Map Extractor] Failed to add {0} to PACT storage", internalName);
            }
        }
        else
        {
            NC_LOG_WARNING("[Map Extractor] Failed to extract {0}", internalName);
        }

        return true;
    });
}

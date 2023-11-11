#include "MapExtractor.h"
#include "AssetConverter/Runtime.h"
#include "AssetConverter/Blp/BlpConvert.h"
#include "AssetConverter/Casc/CascLoader.h"
#include "AssetConverter/Extractors/ClientDBExtractor.h"
#include "AssetConverter/Util/ServiceLocator.h"

#include <Base/Util/StringUtils.h>
#include <Base/Util/DebugHandler.h>
#include <Base/Memory/FileWriter.h>

#include <FileFormat/Novus/ClientDB/ClientDB.h>
#include <FileFormat/Novus/ClientDB/Definitions.h>
#include <FileFormat/Novus/Map/Map.h>
#include <FileFormat/Novus/Map/MapChunk.h>
#include <FileFormat/Warcraft/ADT/Adt.h>
#include <FileFormat/Warcraft/Parsers/WdtParser.h>
#include <FileFormat/Warcraft/Parsers/AdtParser.h>

#include <enkiTS/TaskScheduler.h>
#include <glm/gtx/euler_angles.inl>

#include <string_view>

using namespace ClientDB;

void MapExtractor::Process()
{
    Runtime* runtime = ServiceLocator::GetRuntime();
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    Storage<Definitions::Map> maps = ClientDBExtractor::GetMapStorage();

    bool createChunkAlphaMaps = runtime->json["Extraction"]["Map"]["BlendMaps"];

    u32 numMapEntries = maps.Count();
    DebugHandler::Print("[Map Extractor] Processing {0} maps", numMapEntries);

    for (const Definitions::Map& map : maps)
    {
        const std::string& mapInternalName = maps.GetString(map.internalName);

        static char formatBuffer[512] = { 0 };
        i32 length = StringUtils::FormatString(&formatBuffer[0], 512, "world/maps/%s/%s.wdt", mapInternalName.c_str(), mapInternalName.c_str());
        if (length <= 0)
            continue;

        std::string wdtPath(&formatBuffer[0], length);
        std::transform(wdtPath.begin(), wdtPath.end(), wdtPath.begin(), [](unsigned char c) { return std::tolower(c); });

        u32 wdtFileID = cascLoader->GetFileIDFromListFilePath(wdtPath.data());
        if (!wdtFileID)
            continue;

        std::shared_ptr<Bytebuffer> fileWDT = cascLoader->GetFileByID(wdtFileID);
        if (!fileWDT)
            continue;

        Adt::WdtParser wdtParser = { };

        Adt::Wdt wdt = { };
        if (!wdtParser.TryParse(fileWDT, wdt))
        {
            DebugHandler::PrintWarning("[Map Extractor] Failed to extract {0} (Corrupt WDT)", mapInternalName);
            continue;
        }

        std::filesystem::create_directories(runtime->paths.map / mapInternalName);

        Map::Layout layout = { };
        layout.flags.UseMapObjectAsBase = wdt.mphd.flags.UseGlobalMapObj;

        if (layout.flags.UseMapObjectAsBase)
        {
            if (!wdt.modf.data.size())
                continue;

            const Adt::MODF::PlacementInfo& placementInfo = wdt.modf.data[0];
            if (!placementInfo.flags.EntryIsFiledataID || placementInfo.fileID == 0)
                continue;

            // Skip map if placement file doesn't exist
            if (!cascLoader->FileExistsInCasc(placementInfo.fileID))
            {
                DebugHandler::PrintError("Skipped map {0} because placement file doesn't exist", mapInternalName);
                continue;
            }

            Terrain::Placement& placement = layout.placement;
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

            const std::string& filePath = cascLoader->GetFilePathFromListFileID(placement.nameHash);
            fs::path wmoPath = fs::path(filePath).replace_extension(".complexmodel");

            u32 nameHash = StringUtils::fnv1a_32(wmoPath.string().c_str(), wmoPath.string().size());
            placement.nameHash = nameHash;
        }
        else
        {
            std::filesystem::create_directories(runtime->paths.textureBlendMap / mapInternalName);

            enki::TaskSet convertMapTask(Terrain::CHUNK_NUM_PER_MAP, [&runtime, &cascLoader, &map, &mapInternalName, &wdt, createChunkAlphaMaps](enki::TaskSetPartition range, uint32_t threadNum)
            {
                Adt::Parser adtParser = { };

                for (u32 chunkID = range.start; chunkID < range.end; chunkID++)
                {
                    u32 chunkGridPosX = chunkID % 64;
                    u32 chunkGridPosY = chunkID / 64;

                    const Adt::MAIN::AreaInfo& areaInfo = wdt.main.areaInfos[chunkGridPosX][chunkGridPosY];
                    if (!areaInfo.flags.IsUsed)
                        continue;

                    const Adt::MAID::FileIDs& fileIDs = wdt.maid.fileIDs[chunkGridPosX][chunkGridPosY];
                    if (fileIDs.adtRootFileID == 0 || fileIDs.adtTextureFileID == 0 || fileIDs.adtObject1FileID == 0)
                        continue;

                    std::shared_ptr<Bytebuffer> rootBuffer = cascLoader->GetFileByID(fileIDs.adtRootFileID);
                    std::shared_ptr<Bytebuffer> textBuffer = cascLoader->GetFileByID(fileIDs.adtTextureFileID);
                    std::shared_ptr<Bytebuffer> objBuffer = cascLoader->GetFileByID(fileIDs.adtObject1FileID);

                    if (!rootBuffer)
                        continue;

                    Adt::Layout adt = { };
                    {
                        adt.mapID = map.GetID();
                        adt.chunkID = chunkID;
                    }

                    Adt::Parser::Context context = { };

                    auto liquidObjects = ClientDBExtractor::GetLiquidObjectStorage();
                    auto liquidTypes = ClientDBExtractor::GetLiquidTypeStorage();
                    auto liquidMaterials = ClientDBExtractor::GetLiquidMaterialStorage();

                    context.liquidObjects = &liquidObjects;
                    context.liquidTypes = &liquidTypes;
                    context.liquidMaterials = &liquidMaterials;

                    if (!adtParser.TryParse(context, rootBuffer, textBuffer, objBuffer, wdt, adt))
                        continue;

                    Map::Chunk chunk = { };
                    if (!Map::Chunk::FromADT(adt, chunk))
                        continue;

                    // Post Processing
                    {
                        for (u32 i = 0; i < chunk.mapObjectPlacements.size(); i++)
                        {
                            Terrain::Placement& placementInfo = chunk.mapObjectPlacements[i];

                            if (placementInfo.nameHash == 0 ||
                                placementInfo.nameHash == std::numeric_limits<u32>().max())
                                continue;

                            if (!cascLoader->FileExistsInCasc(placementInfo.nameHash))
                            {
                                DebugHandler::PrintError("Skipped map object placement because file doesn't exist");
                                continue;
                            }

                            const std::string& wmoPathStr = cascLoader->GetFilePathFromListFileID(placementInfo.nameHash);
                            fs::path wmoPath = fs::path(wmoPathStr).replace_extension(".complexmodel");

                            u32 nameHash = StringUtils::fnv1a_32(wmoPath.string().c_str(), wmoPath.string().size());
                            placementInfo.nameHash = nameHash;
                        }

                        for (u32 i = 0; i < chunk.complexModelPlacements.size(); i++)
                        {
                            Terrain::Placement& placementInfo = chunk.complexModelPlacements[i];

                            if (placementInfo.nameHash == 0 ||
                                placementInfo.nameHash == std::numeric_limits<u32>().max())
                                continue;

                            if (!cascLoader->FileExistsInCasc(placementInfo.nameHash))
                            {
                                DebugHandler::PrintError("Skipped complex model placement because file doesn't exist");
                                continue;
                            }

                            const std::string& m2PathStr = cascLoader->GetFilePathFromListFileID(placementInfo.nameHash);
                            fs::path m2Path = fs::path(m2PathStr).replace_extension(".complexmodel");

                            u32 nameHash = StringUtils::fnv1a_32(m2Path.string().c_str(), m2Path.string().size());
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
                            Map::Cell& cell = chunk.cells[i];

                            const u32 numLayers = static_cast<u32>(adt.cellInfos[i].mcly.data.size());
                            const u32 basePixelDestination = (i * Terrain::CHUNK_ALPHAMAP_CELL_RESOLUTION * Terrain::CHUNK_ALPHAMAP_CELL_NUM_CHANNELS);

                            for (u32 j = 0; j < 4; j++)
                            {
                                u32 fileID = cell.layers[j].textureID;
                                if (fileID == 0 || fileID == std::numeric_limits<u32>().max())
                                    continue;

                                fs::path texturePath = cascLoader->GetFilePathFromListFileID(fileID);
                                if (texturePath.empty())
                                {
                                    cell.layers[j].textureID = std::numeric_limits<u32>().max();
                                    continue;
                                }

                                texturePath.replace_extension("dds").make_preferred();

                                std::string texturePathStr = texturePath.string();
                                std::transform(texturePathStr.begin(), texturePathStr.end(), texturePathStr.begin(), ::tolower);
                                std::replace(texturePathStr.begin(), texturePathStr.end(), '\\', '/');

                                u32 textureNameHash = StringUtils::fnv1a_32(texturePathStr.c_str(), texturePathStr.length());
                                cell.layers[j].textureID = textureNameHash;

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

                        std::string localChunkBlendMapPath = "blendmaps/" + mapInternalName + "/" + mapInternalName + "_" + std::to_string(chunkGridPosX) + "_" + std::to_string(chunkGridPosY) + ".dds";
                        chunk.chunkAlphaMapTextureHash = (StringUtils::fnv1a_32(localChunkBlendMapPath.c_str(), localChunkBlendMapPath.length()) * isAlphaMapSet) + (std::numeric_limits<u32>().max() * !isAlphaMapSet);

                        if (createChunkAlphaMaps && isAlphaMapSet)
                        {
                            std::string chunkBlendMapOutputPath = (runtime->paths.texture / localChunkBlendMapPath).string();
                            
                            BLP::BlpConvert blpConvert;
                            blpConvert.ConvertRaw(64, 64, Terrain::CHUNK_NUM_CELLS, alphaMapBuffer->GetDataPointer(), Terrain::CHUNK_ALPHAMAP_TOTAL_BYTE_SIZE, BLP::InputFormat::BGRA_8UB, BLP::Format::BC1, chunkBlendMapOutputPath, false);
                        }

                        std::string localChunkPath = mapInternalName + "/" + mapInternalName + "_" + std::to_string(chunkGridPosX) + "_" + std::to_string(chunkGridPosY) + ".chunk";
                        std::string chunkOutputPath = (runtime->paths.map / localChunkPath).string();
                        chunk.Save(chunkOutputPath);
                    }
                }
            });

            convertMapTask.m_Priority = enki::TaskPriority::TASK_PRIORITY_HIGH;
            runtime->scheduler.AddTaskSetToPipe(&convertMapTask);
            runtime->scheduler.WaitforTask(&convertMapTask);
        }

        std::shared_ptr<Bytebuffer> buffer = Bytebuffer::Borrow<sizeof(Map::Layout)>();

        std::string localMapPath = mapInternalName + "/" + mapInternalName + ".map";
        FileWriter fileWriter(runtime->paths.map / localMapPath, buffer);

        if (!buffer->Put(layout))
            continue;

        if (fileWriter.Write())
        {
            DebugHandler::Print("[Map Extractor] Extracted {0}", mapInternalName);
        }
        else
        {
            DebugHandler::PrintWarning("[Map Extractor] Failed to extract {0}", mapInternalName);
        }
    }
}

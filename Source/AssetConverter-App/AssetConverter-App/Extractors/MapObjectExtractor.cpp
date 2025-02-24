#include "MapObjectExtractor.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/JoltStream.h"
#include "AssetConverter-App/Util/ServiceLocator.h"

#include <Base/Container/ConcurrentQueue.h>
#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <FileFormat/Novus/Model/ComplexModel.h>
#include <FileFormat/Novus/Model/MapObject.h>
#include <FileFormat/Shared.h>
#include <FileFormat/Warcraft/WMO/Wmo.h>
#include <FileFormat/Warcraft/Parsers/WmoParser.h>

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <filesystem>
namespace fs = std::filesystem;

void MapObjectExtractor::Process()
{
    Runtime* runtime = ServiceLocator::GetRuntime();
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    const CascListFile& listFile = cascLoader->GetListFile();
    const std::vector<u32>& wmoFileIDs = listFile.GetWMOFileIDs();

    struct FileListEntry
    {
    public:
        u32 fileID = 0;
        std::string fileName;
        std::string path;
    };

    u32 numFiles = static_cast<u32>(wmoFileIDs.size());
    moodycamel::ConcurrentQueue<FileListEntry> fileListQueue(numFiles);

    enki::TaskSet processWMOList(numFiles, [&runtime, &cascLoader, &fileListQueue, &wmoFileIDs](enki::TaskSetPartition range, uint32_t threadNum)
    {
        for (u32 index = range.start; index < range.end; index++)
        {
            u32 wmoFileID = wmoFileIDs[index];

            // Determine if the wmo is a root file
            {
                u32 bytesToSkip = sizeof(u32) + sizeof(u32) + sizeof(MVER);
                u32 bytesToRead = bytesToSkip + sizeof(u32);

                std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFilePartialByID(wmoFileID, bytesToRead);
                if (buffer == nullptr)
                    continue;

                buffer->SkipRead(bytesToSkip);

                u32 chunkToken = 0;
                if (!buffer->GetU32(chunkToken))
                    continue;

                if (chunkToken != 'MOHD')
                    continue;
            }

            std::string pathStr = cascLoader->GetFilePathFromListFileID(wmoFileID);
            std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::tolower);

            fs::path outputPath = (runtime->paths.complexModel / pathStr).replace_extension(Model::FILE_EXTENSION);
            fs::create_directories(outputPath.parent_path());

            FileListEntry fileListEntry;
            fileListEntry.fileID = wmoFileID;
            fileListEntry.fileName = outputPath.filename().string();
            fileListEntry.path = outputPath.string();

            fileListQueue.enqueue(fileListEntry);
        }
    });

    runtime->scheduler.AddTaskSetToPipe(&processWMOList);
    runtime->scheduler.WaitforTask(&processWMOList);

    std::mutex printMutex;
    u32 numProcessedFiles = 0;
    u16 progressFlags = 0;

    u32 numRootFiles = static_cast<u32>(fileListQueue.size_approx());
    NC_LOG_INFO("[MapObject Extractor] Processing {0} files", numRootFiles);

    enki::TaskSet convertWMOTask(numRootFiles, [&runtime, &cascLoader, &fileListQueue, &numProcessedFiles, &progressFlags, &printMutex, numRootFiles](enki::TaskSetPartition range, uint32_t threadNum)
    {
        Wmo::Parser wmoParser = { };

        FileListEntry fileListEntry;
        while(fileListQueue.try_dequeue(fileListEntry))
        {
            Wmo::Layout wmo = { };
            std::shared_ptr<Bytebuffer> rootBuffer = cascLoader->GetFileByID(fileListEntry.fileID);
            if (!wmoParser.TryParse(Wmo::Parser::ParseType::Root, rootBuffer, wmo))
                continue;

            for (u32 i = 0; i < wmo.mohd.groupCount; i++)
            {
                u32 fileID = wmo.gfid.data[i].fileID;
                if (fileID == 0)
                    continue;

                std::shared_ptr<Bytebuffer> groupBuffer = cascLoader->GetFileByID(fileID);
                if (!groupBuffer)
                    continue;

                if (!wmoParser.TryParse(Wmo::Parser::ParseType::Group, groupBuffer, wmo))
                    continue;
            }

            Model::MapObject mapObject = { };
            if (!Model::MapObject::FromWMO(wmo, mapObject))
                continue;

            // Post Processing
            {
                std::string pathAsString = "";

                // Convert Material FileIDs to TextureHash
                for (u32 i = 0; i < mapObject.materials.size(); i++)
                {
                    Model::MapObject::Material& material = mapObject.materials[i];

                    for (u32 j = 0; j < 3; j++)
                    {
                        u32 textureFileID = material.textureID[j];
                        if (textureFileID == std::numeric_limits<u32>().max())
                            continue;

                        if (!cascLoader->InCascAndListFile(textureFileID))
                            continue;

                        const std::string& cascFilePath = cascLoader->GetFilePathFromListFileID(textureFileID);
                        if (cascFilePath.size() == 0)
                        {
                            material.textureID[j] = std::numeric_limits<u32>().max();
                            continue;
                        }

                        fs::path texturePath = cascFilePath;
                        texturePath.replace_extension("dds").make_preferred();

                        pathAsString = texturePath.string();
                        std::transform(pathAsString.begin(), pathAsString.end(), pathAsString.begin(), ::tolower);
                        std::replace(pathAsString.begin(), pathAsString.end(), '\\', '/');

                        material.textureID[j] = StringUtils::fnv1a_32(pathAsString.c_str(), pathAsString.length());
                    }
                }

                // Convert Decoration FileIDs to PathHash
                {
                    for (u32 i = 0; i < mapObject.decorations.size(); i++)
                    {
                        Model::MapObject::Decoration& decoration = mapObject.decorations[i];

                        u32 decorationFileID = decoration.nameID;
                        if (decorationFileID == std::numeric_limits<u32>().max())
                            continue;

                        const std::string& cascFilePath = cascLoader->GetFilePathFromListFileID(decorationFileID);
                        if (cascFilePath.size() == 0)
                        {
                            decoration.nameID = std::numeric_limits<u32>().max();
                            continue;
                        }

                        fs::path cmodelPath = cascFilePath;
                        cmodelPath.replace_extension(Model::FILE_EXTENSION);

                        pathAsString = cmodelPath.string();
                        std::transform(pathAsString.begin(), pathAsString.end(), pathAsString.begin(), ::tolower);

                        decoration.nameID = StringUtils::fnv1a_32(pathAsString.c_str(), pathAsString.length());
                    }
                }
            }

            Model::ComplexModel cmodel;
            if (!Model::ComplexModel::FromMapObject(mapObject, cmodel))
                continue;

            // if build physics shapes
            {
                u32 numCollisionVertices = static_cast<u32>(cmodel.collisionVertexPositions.size());
                u32 numCollisionIndices = static_cast<u32>(cmodel.collisionIndices.size());
                u32 indexRemainder = numCollisionIndices % 3;

                if (numCollisionVertices > 0 && numCollisionIndices > 0 && indexRemainder == 0)
                {
                    u32 numTriangles = numCollisionIndices / 3;

                    JPH::VertexList vertexList;
                    vertexList.reserve(numCollisionVertices);

                    JPH::IndexedTriangleList triangleList;
                    triangleList.reserve(numTriangles);

                    for (u32 i = 0; i < numCollisionVertices; i++)
                    {
                        const vec3& vertexPos = cmodel.collisionVertexPositions[i];
                        vertexList.push_back({ vertexPos.x, vertexPos.y, vertexPos.z });
                    }

                    for (u32 i = 0; i < numTriangles; i++)
                    {
                        u32 offset = i * 3;

                        u32 indexA = cmodel.collisionIndices[offset + 2];
                        u32 indexB = cmodel.collisionIndices[offset + 1];
                        u32 indexC = cmodel.collisionIndices[offset + 0];

                        triangleList.push_back({ indexA, indexB, indexC });
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
                        cmodel.physicsData.resize(joltChunkBuffer->writtenData);
                        memcpy(&cmodel.physicsData[0], joltChunkBuffer->GetDataPointer(), joltChunkBuffer->writtenData);
                    }
                }
            }

            bool result = cmodel.Save(fileListEntry.path);
            if (runtime->isInDebugMode)
            {
                if (result)
                {
                    NC_LOG_INFO("[MapObject Extractor] Extracted {0}", fileListEntry.fileName);
                }
                else
                {
                    NC_LOG_WARNING("[MapObject Extractor] Failed to extract {0}", fileListEntry.fileName);
                }
            }

            {
                std::scoped_lock scopedLock(printMutex);

                f32 processedFiles = static_cast<f32>(++numProcessedFiles);
                f32 progress = (processedFiles / static_cast<f32>(numRootFiles - 1)) * 10.0f;
                u32 bitToCheck = static_cast<u32>(progress);
                u32 bitMask = 1 << bitToCheck;

                bool reportStatus = (progressFlags & bitMask) == 0;
                if (reportStatus)
                {
                    progressFlags |= bitMask;
                    NC_LOG_INFO("[MapObject Extractor] Progress Status ({0:.0f}% / 100%)", progress * 10.0f);
                }
            }
        }
    });

    convertWMOTask.m_Priority = enki::TaskPriority::TASK_PRIORITY_HIGH;
    runtime->scheduler.AddTaskSetToPipe(&convertWMOTask);
    runtime->scheduler.WaitforTask(&convertWMOTask);
}
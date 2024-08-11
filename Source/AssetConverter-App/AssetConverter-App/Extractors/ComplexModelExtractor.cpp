#include "ComplexModelExtractor.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/JoltStream.h"
#include "AssetConverter-App/Util/ServiceLocator.h"

#include <Base/Container/ConcurrentQueue.h>
#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <FileFormat/Novus/Model/ComplexModel.h>
#include <FileFormat/Shared.h>
#include <FileFormat/Warcraft/M2/M2.h>
#include <FileFormat/Warcraft/Parsers/M2Parser.h>

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <filesystem>
namespace fs = std::filesystem;

void ComplexModelExtractor::Process()
{
    Runtime* runtime = ServiceLocator::GetRuntime();
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    const CascListFile& listFile = cascLoader->GetListFile();
    const std::vector<u32>& m2FileIDs = listFile.GetM2FileIDs();

    struct FileListEntry
    {
        u32 fileID = 0;
        std::string fileName;
        std::string path;
    };

    u32 numFiles = static_cast<u32>(m2FileIDs.size());
    moodycamel::ConcurrentQueue<FileListEntry> fileListQueue(numFiles);

    enki::TaskSet processM2List(numFiles, [&runtime, &cascLoader, &fileListQueue, &m2FileIDs](enki::TaskSetPartition range, uint32_t threadNum)
    {
        for (u32 index = range.start; index < range.end; index++)
        {
            u32 m2FileID = m2FileIDs[index];

            switch (m2FileID)
            {
                case 5779493:
                case 5779495:
                    continue;

                default:
                    break;
            }

            if (!cascLoader->InCascAndListFile(m2FileID))
                continue;

            std::string pathStr = cascLoader->GetFilePathFromListFileID(m2FileID);
            std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::tolower);

            fs::path outputPath = (runtime->paths.complexModel / pathStr).replace_extension(Model::FILE_EXTENSION);
            fs::create_directories(outputPath.parent_path());

            FileListEntry fileListEntry;
            fileListEntry.fileID = m2FileID;
            fileListEntry.fileName = outputPath.filename().string();
            fileListEntry.path = outputPath.string();

            fileListQueue.enqueue(fileListEntry);
        }
    });

    runtime->scheduler.AddTaskSetToPipe(&processM2List);
    runtime->scheduler.WaitforTask(&processM2List);

    std::mutex printMutex;
    u32 numProcessedFiles = 0;
    u16 progressFlags = 0;

    u32 numModelsToProcess = static_cast<u32>(fileListQueue.size_approx());
    NC_LOG_INFO("[ComplexModel Extractor] Processing {0} files", numModelsToProcess);

    enki::TaskSet convertM2Task(numModelsToProcess, [&runtime, &cascLoader, &fileListQueue, &numProcessedFiles, &progressFlags, &printMutex, numModelsToProcess](enki::TaskSetPartition range, uint32_t threadNum)
    {
        M2::Parser m2Parser = {};

        FileListEntry fileListEntry;
        while(fileListQueue.try_dequeue(fileListEntry))
        {
            std::shared_ptr<Bytebuffer> rootBuffer = cascLoader->GetFileByID(fileListEntry.fileID);
            if (!rootBuffer || rootBuffer->size == 0 || rootBuffer->writtenData == 0)
                continue;

            M2::Layout m2 = { };
            if (!m2Parser.TryParse(M2::Parser::ParseType::Root, rootBuffer, m2))
            {
                NC_LOG_WARNING("Tried to parse M2 Root but failed {0}", fileListEntry.fileID);
                continue;
            }

            std::shared_ptr<Bytebuffer> skinBuffer = cascLoader->GetFileByID(m2.sfid.skinFileIDs[0]);
            if (!skinBuffer || skinBuffer->size == 0 || skinBuffer->writtenData == 0)
                continue;

            if (!m2Parser.TryParse(M2::Parser::ParseType::Skin, skinBuffer, m2))
                continue;

            Model::ComplexModel cmodel = { };
            if (!Model::ComplexModel::FromM2(rootBuffer, skinBuffer, m2, cmodel))
                continue;

            // Post Process
            {
                for (u32 i = 0; i < cmodel.textures.size(); i++)
                {
                    Model::ComplexModel::Texture& texture = cmodel.textures[i];

                    u32 fileID = texture.textureHash; // This has not been converted to a textureHash yet.
                    texture.textureHash = std::numeric_limits<u32>().max(); // Default to invalid

                    if (fileID == 0 || fileID == std::numeric_limits<u32>().max())
                        continue;

                    if (!cascLoader->InCascAndListFile(fileID))
                        continue;

                    const std::string& cascFilePath = cascLoader->GetFilePathFromListFileID(fileID);
                    if (cascFilePath.size() == 0)
                        continue;

                    fs::path texturePath = cascFilePath;
                    texturePath.replace_extension("dds").make_preferred();

                    std::string textureName = texturePath.string();
                    std::transform(textureName.begin(), textureName.end(), textureName.begin(), ::tolower);
                    std::replace(textureName.begin(), textureName.end(), '\\', '/');

                    texture.textureHash = StringUtils::fnv1a_32(textureName.c_str(), textureName.length());
                }

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

                            u16 indexA = cmodel.collisionIndices[offset + 2];
                            u16 indexB = cmodel.collisionIndices[offset + 1];
                            u16 indexC = cmodel.collisionIndices[offset + 0];

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
            }

            bool result = cmodel.Save(fileListEntry.path);
            if (runtime->isInDebugMode)
            {
                if (result)
                {
                    NC_LOG_INFO("[ComplexModel Extractor] Extracted {0}", fileListEntry.fileName);
                }
                else
                {
                    NC_LOG_WARNING("[ComplexModel Extractor] Failed to extract {0}", fileListEntry.fileName);
                }
            }

            {
                std::scoped_lock scopedLock(printMutex);
                
                f32 progress = (static_cast<f32>(numProcessedFiles++) / static_cast<f32>(numModelsToProcess - 1)) * 10.0f;
                u32 bitToCheck = static_cast<u32>(progress);
                u32 bitMask = 1 << bitToCheck;
                
                bool reportStatus = (progressFlags & bitMask) == 0;
                if (reportStatus)
                {
                    progressFlags |= bitMask;
                    NC_LOG_INFO("[ComplexModel Extractor] Progress Status ({0:.0f}% / 100%)", progress * 10.0f);
                }
            }
        }
    });

    convertM2Task.m_Priority = enki::TaskPriority::TASK_PRIORITY_HIGH;
    runtime->scheduler.AddTaskSetToPipe(&convertM2Task);
    runtime->scheduler.WaitforTask(&convertM2Task);
}
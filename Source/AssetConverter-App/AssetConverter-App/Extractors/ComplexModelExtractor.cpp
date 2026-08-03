#include "ComplexModelExtractor.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/JoltStream.h"
#include "AssetConverter-App/Util/ServiceLocator.h"
#include "AssetConverter-App/Model/ModelV2Builder.h"

#include <Base/Container/ConcurrentQueue.h>
#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <FileFormat/Novus/Model/ComplexModel.h>
#include <FileFormat/Novus/Model/Model.h>
#include <FileFormat/Shared.h>
#include <FileFormat/Warcraft/M2/M2.h>
#include <FileFormat/Warcraft/Parsers/M2Parser.h>

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <xxhash/xxhash64.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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
        std::string modelV2Path;
    };

    u32 numFiles = static_cast<u32>(m2FileIDs.size());
    moodycamel::ConcurrentQueue<FileListEntry> fileListQueue(numFiles);

    enki::TaskSet processM2List(numFiles, [&runtime, &cascLoader, &fileListQueue, &m2FileIDs](enki::TaskSetPartition range, uint32_t threadNum)
    {
        for (u32 index = range.start; index < range.end; index++)
        {
            u32 m2FileID = m2FileIDs[index];
    
            // Known source files that currently fail M2 conversion.
            switch (m2FileID)
            {
                case 5779493:
                case 5779495:
                case 6705204:
                    continue;
    
                default:
                    break;
            }
    
            if (!cascLoader->InCascAndListFile(m2FileID))
                continue;
    
            std::string pathStr = cascLoader->GetFilePathFromListFileID(m2FileID);
            std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::tolower);
    
            fs::path outputPath = fs::path("model") / pathStr;
            outputPath.replace_extension(Model::FILE_EXTENSION);
    
            FileListEntry fileListEntry;
            fileListEntry.fileID = m2FileID;
            fileListEntry.fileName = outputPath.filename().string();
            fileListEntry.path = outputPath.generic_string();

            outputPath.replace_extension(FileFormat::Model::FILE_EXTENSION);
            fileListEntry.modelV2Path = outputPath.generic_string();
    
            fileListQueue.enqueue(fileListEntry);
        }
    });
    
    runtime->scheduler.AddTaskSetToPipe(&processM2List);
    runtime->scheduler.WaitforTask(&processM2List);

    std::mutex printMutex;
    std::atomic<u64> modelV2Nanoseconds = 0;
    std::atomic<u64> legacyNanoseconds = 0;
    std::atomic<u32> modelV2Succeeded = 0;
    std::atomic<u32> legacySucceeded = 0;
    u32 numProcessedFiles = 0;
    u16 progressFlags = 0;

    u32 numModelsToProcess = static_cast<u32>(fileListQueue.size_approx());
    NC_LOG_INFO("[ComplexModel Extractor] Processing {0} files", numModelsToProcess);
    ModelV2Builder::ResetMeshoptimizerTimings();
    const auto extractorWallStart = std::chrono::steady_clock::now();

    enki::TaskSet convertM2Task(numModelsToProcess, [&runtime, &cascLoader, &fileListQueue, &numProcessedFiles, &progressFlags, &printMutex,
        &modelV2Nanoseconds, &legacyNanoseconds, &modelV2Succeeded, &legacySucceeded, numModelsToProcess](enki::TaskSetPartition range, uint32_t threadNum)
    {
        M2::Parser m2Parser = {};
        std::shared_ptr<Bytebuffer> buffer;

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

            const auto modelV2Start = std::chrono::steady_clock::now();
            std::vector<u64> modelV2TextureAssetIDs(m2.md21.textures.size, FileFormat::INVALID_ASSET_ID);
            for (u32 textureIndex = 0; textureIndex < modelV2TextureAssetIDs.size() && textureIndex < m2.txid.textureFileIDs.size(); ++textureIndex)
            {
                const u32 fileID = m2.txid.textureFileIDs[textureIndex];
                if (fileID == 0 || fileID == std::numeric_limits<u32>().max() || !cascLoader->InCascAndListFile(fileID))
                    continue;
                const std::string& cascFilePath = cascLoader->GetFilePathFromListFileID(fileID);
                if (cascFilePath.empty())
                    continue;
                fs::path texturePath = fs::path("texture") / cascFilePath;
                texturePath.replace_extension("dds");
                std::string textureName = texturePath.generic_string();
                std::transform(textureName.begin(), textureName.end(), textureName.begin(), ::tolower);
                modelV2TextureAssetIDs[textureIndex] = XXHash64::hash(textureName.c_str(), textureName.length(), 0);
            }
            const bool modelV2Extracted = ModelV2Builder::ConvertM2AndAdd(runtime, rootBuffer, skinBuffer, m2,
                modelV2TextureAssetIDs, fileListEntry.modelV2Path);
            modelV2Nanoseconds.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - modelV2Start).count()), std::memory_order_relaxed);
            if (modelV2Extracted)
                modelV2Succeeded.fetch_add(1, std::memory_order_relaxed);
            else
                NC_LOG_WARNING("[ComplexModel Extractor] Failed to extract Model V2 {0}", fileListEntry.fileName);

            const auto legacyStart = std::chrono::steady_clock::now();
            Model::ComplexModel cmodel = { };
            if (!Model::ComplexModel::FromM2(rootBuffer, skinBuffer, m2, cmodel))
                continue;

            // Post Process
            {
                for (u32 i = 0; i < cmodel.textures.size(); i++)
                {
                    Model::ComplexModel::Texture& texture = cmodel.textures[i];

                    u32 fileID = static_cast<u32>(texture.textureHash); // This has not been converted to a textureHash yet.
                    texture.textureHash = std::numeric_limits<u64>().max(); // Default to invalid

                    if (fileID == 0 || fileID == std::numeric_limits<u32>().max())
                        continue;

                    if (!cascLoader->InCascAndListFile(fileID))
                        continue;

                    const std::string& cascFilePath = cascLoader->GetFilePathFromListFileID(fileID);
                    if (cascFilePath.size() == 0)
                        continue;

                    fs::path texturePath = fs::path("texture") / cascFilePath;
                    texturePath.replace_extension("dds").make_preferred();

                    std::string textureName = texturePath.string();
                    std::transform(textureName.begin(), textureName.end(), textureName.begin(), ::tolower);
                    std::replace(textureName.begin(), textureName.end(), '\\', '/');

                    texture.textureHash = XXHash64::hash(textureName.c_str(), textureName.length(), 0);
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

            constexpr size_t MAX_SERIALIZED_MODEL_SIZE = 64 * 1024 * 1024;
            const size_t serializedSize = cmodel.GetSerializedSize();
            bool serialized = false;

            if (serializedSize <= MAX_SERIALIZED_MODEL_SIZE)
            {
                if (!buffer || buffer->size < serializedSize)
                    buffer = Bytebuffer::BorrowRuntime(serializedSize);
                else
                    buffer->Reset();

                serialized = cmodel.Save(buffer);
                if (serialized && buffer->writtenData != serializedSize)
                {
                    NC_LOG_ERROR("[ComplexModel Extractor] Serialized size mismatch for {0} (Expected: {1}, Actual: {2})", fileListEntry.fileName, serializedSize, buffer->writtenData);
                    serialized = false;
                }
            }
            else
            {
                NC_LOG_WARNING("[ComplexModel Extractor] {0} exceeds the maximum serialized size ({1} bytes)", fileListEntry.fileName, serializedSize);
            }

            if (serialized)
            {
                auto& manifest = runtime->pactInfo.GetManifestForFile(runtime, buffer->writtenData);
                if (manifest.AddFile(runtime, fileListEntry.path, buffer))
                {
                    legacySucceeded.fetch_add(1, std::memory_order_relaxed);
                    if (runtime->isInDebugMode)
                    {
                        NC_LOG_INFO("[ComplexModel Extractor] Extracted {0}", fileListEntry.fileName);
                    }
                }
                else
                {
                    NC_LOG_WARNING("[ComplexModel Extractor] Failed to add {0} to PACT storage", fileListEntry.fileName);
                }
            }
            else
            {
                NC_LOG_WARNING("[ComplexModel Extractor] Failed to extract {0}", fileListEntry.fileName);
            }

            {
                legacyNanoseconds.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - legacyStart).count()), std::memory_order_relaxed);
                std::scoped_lock scopedLock(printMutex);
                
                const u32 processedFiles = ++numProcessedFiles;
                f32 progress = (static_cast<f32>(processedFiles) / static_cast<f32>(numModelsToProcess)) * 10.0f;
                u32 bitToCheck = static_cast<u32>(progress);
                u32 bitMask = 1u << bitToCheck;
                
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
    if (!ModelV2Builder::FlushPendingMaterials(runtime))
        NC_LOG_ERROR("[ComplexModel Extractor] Failed to emit deferred Model V2 materials");

    const ModelV2Builder::MeshoptimizerTimings meshoptimizerTimings = ModelV2Builder::GetMeshoptimizerTimings();
    const auto seconds = [](u64 nanoseconds) { return static_cast<f64>(nanoseconds) / 1'000'000'000.0; };
    const f64 extractorWallSeconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - extractorWallStart).count();
    NC_LOG_INFO("[ComplexModel Extractor] Timings (shared CASC/parse excluded, worker time): direct Model V2 convert/cook/write {0:.3f}s for {1} files; legacy ComplexModel convert/serialize/write {2:.3f}s for {3} files",
        static_cast<f64>(modelV2Nanoseconds.load(std::memory_order_relaxed)) / 1'000'000'000.0,
        modelV2Succeeded.load(std::memory_order_relaxed),
        static_cast<f64>(legacyNanoseconds.load(std::memory_order_relaxed)) / 1'000'000'000.0,
        legacySucceeded.load(std::memory_order_relaxed));

    // Persist benchmark data because redirected Windows consoles used by
    // unattended extraction do not reliably preserve the logger output.
    std::ofstream timingFile("Data/Pact/model_extraction_timings.txt", std::ios::app);
    timingFile << "M2 DirectModelV2ConvertCookWriteWorkerSeconds=" << static_cast<f64>(modelV2Nanoseconds.load(std::memory_order_relaxed)) / 1'000'000'000.0
               << " Files=" << modelV2Succeeded.load(std::memory_order_relaxed)
               << " LegacyComplexModelConvertSerializeWriteWorkerSeconds=" << static_cast<f64>(legacyNanoseconds.load(std::memory_order_relaxed)) / 1'000'000'000.0
               << " Files=" << legacySucceeded.load(std::memory_order_relaxed)
               << " MeshoptimizerTotalWorkerSeconds=" << seconds(meshoptimizerTimings.GetTotalNanoseconds())
               << " Simplification=" << seconds(meshoptimizerTimings.simplificationNanoseconds)
               << " Tangents=" << seconds(meshoptimizerTimings.tangentGenerationNanoseconds)
               << " VertexRemap=" << seconds(meshoptimizerTimings.vertexRemapNanoseconds)
               << " VertexCache=" << seconds(meshoptimizerTimings.vertexCacheNanoseconds)
               << " Overdraw=" << seconds(meshoptimizerTimings.overdrawNanoseconds)
               << " VertexFetch=" << seconds(meshoptimizerTimings.vertexFetchNanoseconds)
               << " MeshletBuild=" << seconds(meshoptimizerTimings.meshletBuildNanoseconds)
               << " MeshletOptimize=" << seconds(meshoptimizerTimings.meshletOptimizationNanoseconds)
               << " MeshletBounds=" << seconds(meshoptimizerTimings.meshletBoundsNanoseconds)
               << " SourcePreparation=" << seconds(meshoptimizerTimings.sourcePreparationNanoseconds)
               << " BaseLODAssembly=" << seconds(meshoptimizerTimings.baseLODAssemblyNanoseconds)
               << " GeneratedLODAssemblyInclusive=" << seconds(meshoptimizerTimings.generatedLODAssemblyNanoseconds)
               << " MaterialProcessing=" << seconds(meshoptimizerTimings.materialProcessingNanoseconds)
               << " GeometryCookingInclusive=" << seconds(meshoptimizerTimings.geometryCookingNanoseconds)
               << " Serialization=" << seconds(meshoptimizerTimings.serializationNanoseconds)
               << " PactWrite=" << seconds(meshoptimizerTimings.pactWriteNanoseconds)
               << " ExtractorWallSeconds=" << extractorWallSeconds << '\n';
}

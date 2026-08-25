#include "ModelExtractor.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/ServiceLocator.h"
#include "AssetConverter-App/Model/ModelV2Builder.h"

#include <Base/Container/ConcurrentQueue.h>
#include <Base/Util/DebugHandler.h>

#include <FileFormat/Novus/Model/Model.h>
#include <FileFormat/Shared.h>
#include <FileFormat/Warcraft/M2/M2.h>
#include <FileFormat/Warcraft/Parsers/M2Parser.h>

#include <xxhash/xxhash64.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

namespace fs = std::filesystem;

void ModelExtractor::Process()
{
    Runtime* runtime = ServiceLocator::GetRuntime();
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();
    const std::vector<u32>& m2FileIDs = cascLoader->GetListFile().GetM2FileIDs();

    struct FileListEntry
    {
        u32 fileID = 0;
        std::string fileName;
        std::string outputPath;
    };

    const u32 numFiles = static_cast<u32>(m2FileIDs.size());
    moodycamel::ConcurrentQueue<FileListEntry> fileListQueue(numFiles);
    enki::TaskSet processM2List(numFiles, [&cascLoader, &fileListQueue, &m2FileIDs](enki::TaskSetPartition range, u32)
    {
        for (u32 index = range.start; index < range.end; ++index)
        {
            const u32 m2FileID = m2FileIDs[index];
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

            std::string sourcePath = cascLoader->GetFilePathFromListFileID(m2FileID);
            std::transform(sourcePath.begin(), sourcePath.end(), sourcePath.begin(), ::tolower);
            fs::path outputPath = fs::path("model") / sourcePath;
            outputPath.replace_extension(FileFormat::Model::FILE_EXTENSION);
            fileListQueue.enqueue({ m2FileID, outputPath.filename().string(), outputPath.generic_string() });
        }
    });
    runtime->scheduler.AddTaskSetToPipe(&processM2List);
    runtime->scheduler.WaitforTask(&processM2List);

    std::mutex printMutex;
    std::atomic<u64> modelNanoseconds = 0;
    std::atomic<u32> modelsSucceeded = 0;
    u32 numProcessedFiles = 0;
    u16 progressFlags = 0;

    const u32 numModelsToProcess = static_cast<u32>(fileListQueue.size_approx());
    NC_LOG_INFO("[Model Extractor] Processing {0} files", numModelsToProcess);
    ModelV2Builder::ResetMeshoptimizerTimings();
    const auto extractorWallStart = std::chrono::steady_clock::now();

    enki::TaskSet convertM2Task(numModelsToProcess, [&runtime, &cascLoader, &fileListQueue, &numProcessedFiles, &progressFlags, &printMutex, &modelNanoseconds, &modelsSucceeded, numModelsToProcess](enki::TaskSetPartition, u32)
    {
        M2::Parser m2Parser = {};
        FileListEntry fileListEntry;
        while (fileListQueue.try_dequeue(fileListEntry))
        {
            bool extracted = false;
            const auto modelStart = std::chrono::steady_clock::now();
            std::shared_ptr<Bytebuffer> rootBuffer = cascLoader->GetFileByID(fileListEntry.fileID);
            M2::Layout m2 = {};
            if (rootBuffer && rootBuffer->size > 0 && rootBuffer->writtenData > 0 && m2Parser.TryParse(M2::Parser::ParseType::Root, rootBuffer, m2))
            {
                std::shared_ptr<Bytebuffer> skinBuffer = cascLoader->GetFileByID(m2.sfid.skinFileIDs[0]);
                if (skinBuffer && skinBuffer->size > 0 && skinBuffer->writtenData > 0 && m2Parser.TryParse(M2::Parser::ParseType::Skin, skinBuffer, m2))
                {
                    std::vector<u64> textureAssetIDs(m2.md21.textures.size, FileFormat::INVALID_ASSET_ID);
                    for (u32 textureIndex = 0; textureIndex < textureAssetIDs.size() && textureIndex < m2.txid.textureFileIDs.size(); ++textureIndex)
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
                        textureAssetIDs[textureIndex] = XXHash64::hash(textureName.c_str(), textureName.length(), 0);
                    }
                    extracted = ModelV2Builder::ConvertM2AndAdd(runtime, rootBuffer, skinBuffer, m2, textureAssetIDs, fileListEntry.outputPath);
                }
            }

            modelNanoseconds.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - modelStart).count()), std::memory_order_relaxed);
            if (extracted)
                modelsSucceeded.fetch_add(1, std::memory_order_relaxed);
            else
                NC_LOG_WARNING("[Model Extractor] Failed to extract {0} (FileDataID {1})", fileListEntry.fileName, fileListEntry.fileID);

            std::scoped_lock scopedLock(printMutex);
            const u32 processedFiles = ++numProcessedFiles;
            const f32 progress = (static_cast<f32>(processedFiles) / static_cast<f32>(numModelsToProcess)) * 10.0f;
            const u32 bitToCheck = static_cast<u32>(progress);
            const u32 bitMask = 1u << bitToCheck;
            if ((progressFlags & bitMask) == 0)
            {
                progressFlags |= bitMask;
                NC_LOG_INFO("[Model Extractor] Progress Status ({0:.0f}% / 100%)", progress * 10.0f);
            }
        }
    });

    convertM2Task.m_Priority = enki::TaskPriority::TASK_PRIORITY_HIGH;
    runtime->scheduler.AddTaskSetToPipe(&convertM2Task);
    runtime->scheduler.WaitforTask(&convertM2Task);
    if (!ModelV2Builder::FlushPendingMaterials(runtime))
        NC_LOG_ERROR("[Model Extractor] Failed to emit deferred ModelV2 materials");

    const ModelV2Builder::MeshoptimizerTimings timings = ModelV2Builder::GetMeshoptimizerTimings();
    const auto seconds = [](u64 nanoseconds) { return static_cast<f64>(nanoseconds) / 1'000'000'000.0; };
    const f64 extractorWallSeconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - extractorWallStart).count();
    NC_LOG_INFO("[Model Extractor] Timings: ModelV2 assets {0:.3f}s for {1} files", seconds(modelNanoseconds.load(std::memory_order_relaxed)), modelsSucceeded.load(std::memory_order_relaxed));

    std::ofstream timingFile("Data/Pact/model_extraction_timings.txt", std::ios::app);
    timingFile << "M2 ModelV2ConvertCookWriteWorkerSeconds=" << seconds(modelNanoseconds.load(std::memory_order_relaxed))
               << " Files=" << modelsSucceeded.load(std::memory_order_relaxed)
               << " MeshoptimizerTotalWorkerSeconds=" << seconds(timings.GetTotalNanoseconds())
               << " Simplification=" << seconds(timings.simplificationNanoseconds)
               << " Tangents=" << seconds(timings.tangentGenerationNanoseconds)
               << " VertexRemap=" << seconds(timings.vertexRemapNanoseconds)
               << " VertexCache=" << seconds(timings.vertexCacheNanoseconds)
               << " Overdraw=" << seconds(timings.overdrawNanoseconds)
               << " VertexFetch=" << seconds(timings.vertexFetchNanoseconds)
               << " MeshletBuild=" << seconds(timings.meshletBuildNanoseconds)
               << " MeshletOptimize=" << seconds(timings.meshletOptimizationNanoseconds)
               << " MeshletBounds=" << seconds(timings.meshletBoundsNanoseconds)
               << " SourcePreparation=" << seconds(timings.sourcePreparationNanoseconds)
               << " BaseLODAssembly=" << seconds(timings.baseLODAssemblyNanoseconds)
               << " GeneratedLODAssemblyInclusive=" << seconds(timings.generatedLODAssemblyNanoseconds)
               << " MaterialProcessing=" << seconds(timings.materialProcessingNanoseconds)
               << " GeometryCookingInclusive=" << seconds(timings.geometryCookingNanoseconds)
               << " Serialization=" << seconds(timings.serializationNanoseconds)
               << " PactWrite=" << seconds(timings.pactWriteNanoseconds)
               << " ExtractorWallSeconds=" << extractorWallSeconds << '\n';
}

#include "MapObjectExtractor.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/ServiceLocator.h"
#include "AssetConverter-App/Model/ModelV2Builder.h"

#include <Base/Container/ConcurrentQueue.h>
#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <FileFormat/Novus/Model/Model.h>
#include <FileFormat/Shared.h>
#include <FileFormat/Warcraft/WMO/Wmo.h>
#include <FileFormat/Warcraft/Parsers/WmoParser.h>

#include <xxhash/xxhash64.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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
        std::string outputPath;
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

            fs::path outputPath = fs::path("model") / pathStr;
            outputPath.replace_extension(FileFormat::Model::FILE_EXTENSION);

            FileListEntry fileListEntry;
            fileListEntry.fileID = wmoFileID;
            fileListEntry.fileName = outputPath.filename().string();
            fileListEntry.outputPath = outputPath.generic_string();

            fileListQueue.enqueue(fileListEntry);
        }
    });

    runtime->scheduler.AddTaskSetToPipe(&processWMOList);
    runtime->scheduler.WaitforTask(&processWMOList);

    std::mutex printMutex;
    std::atomic<u64> modelV2Nanoseconds = 0;
    std::atomic<u32> modelV2Succeeded = 0;
    u32 numProcessedFiles = 0;
    u16 progressFlags = 0;

    u32 numRootFiles = static_cast<u32>(fileListQueue.size_approx());
    NC_LOG_INFO("[MapObject Extractor] Processing {0} files", numRootFiles);
    ModelV2Builder::ResetMeshoptimizerTimings();
    const auto extractorWallStart = std::chrono::steady_clock::now();

    enki::TaskSet convertWMOTask(numRootFiles, [&runtime, &cascLoader, &fileListQueue, &numProcessedFiles, &progressFlags, &printMutex, &modelV2Nanoseconds, &modelV2Succeeded, numRootFiles](enki::TaskSetPartition range, uint32_t threadNum)
    {
        Wmo::Parser wmoParser = { };
        std::shared_ptr<Bytebuffer> buffer;

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

            const auto modelV2Start = std::chrono::steady_clock::now();
            std::vector<std::array<u64, 3>> modelV2MaterialTextureAssetIDs(wmo.momt.data.size());
            for (std::array<u64, 3>& textureIDs : modelV2MaterialTextureAssetIDs)
                textureIDs.fill(FileFormat::INVALID_ASSET_ID);
            for (u32 materialIndex = 0; materialIndex < wmo.momt.data.size(); ++materialIndex)
            {
                const Wmo::MOMT::Material& material = wmo.momt.data[materialIndex];
                const std::array<u32, 3> fileIDs = { material.texture1FileID, material.texture2FileID, material.texture3FileID };
                for (u32 textureIndex = 0; textureIndex < fileIDs.size(); ++textureIndex)
                {
                    const u32 fileID = fileIDs[textureIndex];
                    if (fileID == 0 || !cascLoader->InCascAndListFile(fileID))
                        continue;
                    const std::string& cascFilePath = cascLoader->GetFilePathFromListFileID(fileID);
                    if (cascFilePath.empty())
                        continue;
                    fs::path texturePath = fs::path("texture") / cascFilePath;
                    texturePath.replace_extension("dds");
                    std::string textureName = texturePath.generic_string();
                    std::transform(textureName.begin(), textureName.end(), textureName.begin(), ::tolower);
                    modelV2MaterialTextureAssetIDs[materialIndex][textureIndex] = XXHash64::hash(textureName.c_str(), textureName.length(), 0);
                }
            }

            std::vector<u64> modelV2DecorationAssetIDs(wmo.modd.data.size(), FileFormat::INVALID_ASSET_ID);
            for (u32 decorationIndex = 0; decorationIndex < wmo.modd.data.size(); ++decorationIndex)
            {
                const u32 modelIndex = wmo.modd.data[decorationIndex].flags.MODIIndex;
                if (modelIndex >= wmo.modi.data.size())
                    continue;
                const u32 fileID = wmo.modi.data[modelIndex].fileID;
                if (!cascLoader->InCascAndListFile(fileID))
                    continue;
                const std::string& cascFilePath = cascLoader->GetFilePathFromListFileID(fileID);
                if (cascFilePath.empty())
                    continue;
                fs::path modelPath = fs::path("model") / cascFilePath;
                modelPath.replace_extension(FileFormat::Model::FILE_EXTENSION);
                std::string modelName = modelPath.generic_string();
                std::transform(modelName.begin(), modelName.end(), modelName.begin(), ::tolower);
                modelV2DecorationAssetIDs[decorationIndex] = XXHash64::hash(modelName.c_str(), modelName.length(), 0);
            }
            const bool modelV2Extracted = ModelV2Builder::ConvertWMOAndAdd(runtime, wmo, modelV2MaterialTextureAssetIDs, modelV2DecorationAssetIDs, fileListEntry.outputPath);
            modelV2Nanoseconds.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - modelV2Start).count()), std::memory_order_relaxed);
            if (modelV2Extracted)
                modelV2Succeeded.fetch_add(1, std::memory_order_relaxed);
            else
                NC_LOG_WARNING("[MapObject Extractor] Failed to extract Model V2 {0}", fileListEntry.fileName);

            {
                std::scoped_lock scopedLock(printMutex);

                const u32 processedFiles = ++numProcessedFiles;
                f32 progress = (static_cast<f32>(processedFiles) / static_cast<f32>(numRootFiles)) * 10.0f;
                u32 bitToCheck = static_cast<u32>(progress);
                u32 bitMask = 1u << bitToCheck;

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
    if (!ModelV2Builder::FlushPendingMaterials(runtime))
        NC_LOG_ERROR("[MapObject Extractor] Failed to emit deferred Model V2 materials");

    const ModelV2Builder::MeshoptimizerTimings meshoptimizerTimings = ModelV2Builder::GetMeshoptimizerTimings();
    const auto seconds = [](u64 nanoseconds) { return static_cast<f64>(nanoseconds) / 1'000'000'000.0; };
    const f64 extractorWallSeconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - extractorWallStart).count();
    NC_LOG_INFO("[MapObject Extractor] Timings (shared CASC/parse excluded, worker time): Model V2 assets {0:.3f}s for {1} files", static_cast<f64>(modelV2Nanoseconds.load(std::memory_order_relaxed)) / 1'000'000'000.0, modelV2Succeeded.load(std::memory_order_relaxed));

    // Persist benchmark data because redirected Windows consoles used by
    // unattended extraction do not reliably preserve the logger output.
    // WMO extraction runs before M2 extraction, so start a fresh report for this run.
    std::ofstream timingFile("Data/Pact/model_extraction_timings.txt", std::ios::trunc);
    timingFile << "WMO DirectModelV2ConvertCookWriteWorkerSeconds=" << static_cast<f64>(modelV2Nanoseconds.load(std::memory_order_relaxed)) / 1'000'000'000.0
               << " Files=" << modelV2Succeeded.load(std::memory_order_relaxed)
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
